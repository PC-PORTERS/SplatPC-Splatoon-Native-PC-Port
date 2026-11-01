param(
  [string]$Ip = '',
  [string]$PublicIp = '',
  [int]$Port = 60000,
  [string]$Exe = '',
  [switch]$IntTrace,
  [switch]$NoGame
)
# Internet test alone at home: the server runs on this PC behind the home
# router (UDP ports 60000-60001 forwarded to this PC), player A plays on the LAN, player B goes
# through a VPN that only routes B's executable (build\vpn\B\Splatoon-v288.exe, NordVPN split
# tunnelling), so B reaches the server and A from another public address, like a remote player.
# The server gives LAN players its LAN address (NEXLAB_ADVERTISE_LAN) and others the public one.
# Logs: build\diagnostics\vpn-<date>-*.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Game = Join-Path $Root 'Game'
if (-not $Exe) {
  $Exe = Join-Path $Game 'Splatoon-v288.exe'   # the current build (the old step5 candidate lacked later fixes)
}
$Logs = Join-Path $Root 'build\diagnostics'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Path $Logs -Force | Out-Null
if (-not (Get-Command py -ErrorAction SilentlyContinue)) { throw 'Python launcher (py) not found.' }
if (-not (Test-Path $Exe)) { throw "Executable not found: $Exe" }
if (-not $NoGame -and (Get-Process -Name 'Splatoon*' -ErrorAction SilentlyContinue)) {
  throw 'A Splatoon process is already running (maybe your own game): close it first.'
}

if (-not $Ip) {
  $Ip = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and $_.PrefixOrigin -ne 'WellKnown' } |
    Select-Object -First 1).IPAddress
}
if (-not $Ip) { throw 'No LAN address found: pass -Ip 192.168.x.y' }
if (-not $PublicIp) { $PublicIp = (Invoke-RestMethod -Uri 'https://api.ipify.org' -TimeoutSec 10).Trim() }
if (-not $PublicIp) { throw 'Public address not found: pass -PublicIp' }
$RegPort = $Port + 2
$Token = -join ((48..57) + (97..122) | Get-Random -Count 24 | ForEach-Object { [char]$_ })
$Db = Join-Path $Root 'build\duo\vpn-accounts.sqlite3'
Remove-Item "$Db*" -Force -ErrorAction SilentlyContinue
foreach ($Name in 'A', 'B') {
  if (-not (Test-Path (Join-Path $Root "build\duo\$Name\online_identity.ini"))) {
    throw "build\duo\$Name\online_identity.ini missing: run scripts\Launch-Duo.ps1 once first."
  }
}
Write-Host "Server: LAN $Ip, public $PublicIp (UDP $Port-$($Port + 1) must be forwarded to $Ip)"
$env:NEXLAB_ADVERTISE_LAN = $Ip
$env:NEXLAB_LAN_PUBLIC = $PublicIp   # public URL of the players on this LAN

# SpotPass data (schedule, festival) generated from Game\boss\*.json; without it the game stays "offline".
foreach ($Tool in 'make_vssetting.py', 'make_festival.py') {
  cmd /c "py -3.12 `"$(Join-Path $Root "tools\boss\$Tool")`" > `"$(Join-Path $Logs "duo-$Stamp-boss.log")`" 2>&1"
}
$Server = Start-Process py -ArgumentList '-3.12', (Join-Path $Root 'tools\nexlab\nexserver.py'), '--public', '--listen', '0.0.0.0',
  '--advertise', $PublicIp, '--port', $Port, '--natcheck', '--natcheck-address', $Ip, '--natcheck-address', $PublicIp, '--accounts-db', $Db, '--registration', 'token',
  '--registration-token', $Token, '--registration-port', $RegPort, '--log', (Join-Path $Logs "vpn-$Stamp-server.log") `
  -PassThru -WindowStyle Minimized
$Nat = $null
try {
  Start-Sleep 3
  Start-Sleep 3
  foreach ($Name in 'A', 'B') {
    & py -3.12 (Join-Path $Root 'tools\nexlab\register.py') --server "http://${Ip}:$RegPort" --allow-http --token $Token `
      --identity (Join-Path $Root "build\duo\$Name\online_identity.ini")
    if ($LASTEXITCODE -ne 0) { throw "Registration of player $Name failed (see above)." }
  }
  if ($NoGame) { Write-Host 'Dry run OK: server up, A and B registered. Stopping.'; return }

  $env:RBREW_CONTENT_DIR = Join-Path $Game 'content'
  $env:RBREW_TITLE_METADATA = Join-Path $Game 'meta\meta.xml'
  $env:RBREW_SYSTEM_MLC_DIR = Join-Path $Game 'mlc01'
  $env:RBREW_VK_CACHE_DIR = Join-Path $Game 'vulkan_cache'
  $env:RBREW_BOSS_DIR = Join-Path $Game 'boss'
  $env:RBREW_PEERS = "$Ip,$PublicIp,127.0.0.1"
  $env:SDL_DIRECTINPUT_ENABLED = '0'   # see Launch-Duo.ps1: SDL DirectInput haptic init fails here
  if ($IntTrace) { $env:RBREW_INT_TRACE = '1' } else { Remove-Item Env:RBREW_INT_TRACE -ErrorAction SilentlyContinue }
  $Friends = Join-Path $Root 'build\duo\friends'
  Remove-Item -Path $Friends -Recurse -Force -ErrorAction SilentlyContinue
  $env:RBREW_FRIENDS_DIR = $Friends
  $Players = @()
  foreach ($Name in 'A', 'B') {
    # B lives in its own folder so the VPN can target its executable alone.
    $Dir = if ($Name -eq 'B') { Join-Path $Root 'build\vpn\B' } else { Join-Path $Root 'build\duo\A' }
    $Reach = if ($Name -eq 'B') { $PublicIp } else { $Ip }   # B goes through the VPN and the router
    $env:RBREW_ONLINE_SERVER = "${Reach}:$Port"
    $env:RBREW_DNS_OVERRIDE = "nncs1.app.nintendowifi.net=$Reach,nncs2.app.nintendowifi.net=$Reach"
    New-Item -ItemType Directory -Path (Join-Path $Dir 'save\common') -Force | Out-Null
    if ($Name -eq 'B') { Copy-Item (Join-Path $Root 'build\duo\B\online_identity.ini') $Dir -Force }
    if (-not (Test-Path (Join-Path $Dir 'save\save.dat')) -and (Test-Path (Join-Path $Game 'save\save.dat'))) {
      Copy-Item (Join-Path $Game 'save\save.dat') (Join-Path $Dir 'save\save.dat')
    }
    Copy-Item $Exe (Join-Path $Dir 'Splatoon-v288.exe') -Force
    Get-ChildItem $Game -Filter '*.dll' | Copy-Item -Destination $Dir -Force
    $Ini = @()
    if (Test-Path (Join-Path $Game 'settings.ini')) {
      $Ini = Get-Content (Join-Path $Game 'settings.ini') | Where-Object { $_ -notmatch '^(fullscreen|pseudo|online_server)=' }
    }
    $Ini + @('fullscreen=0', "pseudo=Joueur$Name") | Set-Content (Join-Path $Dir 'settings.ini') -Encoding ascii
    $env:RBREW_AUDIO_MUTE = if ($Name -eq 'B') { '1' } else { '' }
    $env:RBREW_NET_TRACE = Join-Path $Logs "vpn-$Stamp-$Name-net.txt"
    $env:RBREW_HANG_SAMPLE = 'freeze'
    $Players += Start-Process (Join-Path $Dir 'Splatoon-v288.exe') -WorkingDirectory $Dir -PassThru `
      -RedirectStandardOutput (Join-Path $Logs "vpn-$Stamp-$Name-stdout.log") -RedirectStandardError (Join-Path $Logs "vpn-$Stamp-$Name-stderr.log")
    Start-Sleep 2
  }
  $Players | ForEach-Object { $_.WaitForExit() }
} finally {
  foreach ($P in @($Server, $Nat)) { if ($P -and -not $P.HasExited) { Stop-Process -Id $P.Id -Force -ErrorAction SilentlyContinue } }
  Get-CimInstance Win32_Process -Filter "Name='python.exe'" | Where-Object { ($_.CommandLine -like '*nexserver.py*' -or $_.CommandLine -like "*vpn-$Stamp-natcheck.py*") } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  Remove-Item (Join-Path $Logs "vpn-$Stamp-natcheck.py") -Force -ErrorAction SilentlyContinue
}
