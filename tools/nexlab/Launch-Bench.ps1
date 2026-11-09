param(
  [switch]$Build,         # rebuild the Go server first (low priority)
  [switch]$Reset,         # forget the bench accounts, friends and identities
  [string]$ServerRepo = ''
)
# Local bench for the online features: a Go server of its own (public mode on this PC's LAN address, its own
# accounts database) and two windowed players with test identities. Everything lives in build\bench-go: this
# script refuses any path outside it, so it can never touch build\duo\A or build\duo\B or Game\save.
# Never talks to the public server. Logs: build\diagnostics\bench-<date>-*.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Game = Join-Path $Root 'Game'
$Bench = Join-Path $Root 'build\bench-go'
if (-not $ServerRepo) { $ServerRepo = Join-Path (Split-Path -Parent $Root) 'SplatoonServer' }

function Use-Bench([string]$Path) {
  $Full = [System.IO.Path]::GetFullPath($Path)
  if (-not $Full.StartsWith($Bench + '\', [StringComparison]::OrdinalIgnoreCase) -or $Full -match '\\build\\duo(\\|$)') {
    throw "The bench only works inside $Bench (refused: $Full)."
  }
  return $Full
}

$Exe = @('Splatoon.exe', 'Splatoon-v288.exe') | ForEach-Object { Join-Path $Game $_ } | Where-Object { Test-Path -LiteralPath $_ } |
    Sort-Object { (Get-Item -LiteralPath $_).LastWriteTime } -Descending | Select-Object -First 1
if (-not $Exe) { throw 'No game executable in Game\.' }
$ExeName = Split-Path -Leaf $Exe
$Logs = Join-Path $Root 'build\diagnostics'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Path $Logs -Force | Out-Null
New-Item -ItemType Directory -Path $Bench -Force | Out-Null
if ($Reset) {
  foreach ($Name in 'players', 'accounts.sqlite3') { Remove-Item -LiteralPath (Use-Bench (Join-Path $Bench $Name)) -Recurse -Force -ErrorAction SilentlyContinue }
}

# Go server binary
$Server = Use-Bench (Join-Path $Bench 'splatoon-nex.exe')
if ($Build -or -not (Test-Path -LiteralPath $Server)) {
  Push-Location $ServerRepo
  try { cmd /c start "" /belownormal /b /wait go build -o "$Server" .\cmd\splatoon-nex } finally { Pop-Location }
}
if (-not (Test-Path -LiteralPath $Server)) { throw "Go server not built (repository: $ServerRepo)." }

# The public mode refuses the loopback address: use this PC's LAN address.
$Lan = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notmatch '^(127\.|169\.254\.)' } | Select-Object -First 1).IPAddress
if (-not $Lan) { throw 'No LAN address found.' }
$Database = Use-Bench (Join-Path $Bench 'accounts.sqlite3')
$Config = @{ mode = 'public'; listen = '0.0.0.0'; advertise = $Lan; auth_port = 60000; secure_port = 60001; natcheck = $true
  natcheck_addresses = @($Lan); natcheck_single_ip = $true; accounts_db = $Database; registration = 'token'
  registration_token = 'bench-local-token'; registration_port = 60002; integrity_mode = 'off'; min_client_version = '0.1.0' }
$Config | ConvertTo-Json | Set-Content (Use-Bench (Join-Path $Bench 'server.json')) -Encoding ascii

# Test identities (pid in hex with 0x for the accounts command): created once, registered in the bench database only.
$Pids = @{ A = '01000001'; B = '01000002' }
foreach ($Name in 'A', 'B') {
  $Dir = Use-Bench (Join-Path $Bench "players\$Name")
  New-Item -ItemType Directory -Path (Join-Path $Dir 'save\common') -Force | Out-Null
  $Identity = Join-Path $Dir 'online_identity.ini'
  if (-not (Test-Path -LiteralPath $Identity)) {
    $Password = -join ((48..57) + (65..90) + (97..122) | Get-Random -Count 16 | ForEach-Object { [char]$_ })
    "pid=$($Pids[$Name])`npassword=$Password" | Set-Content $Identity -Encoding ascii
    $Password | & $Server accounts --db $Database add "0x$($Pids[$Name])" --password-stdin | Out-Null
  }
}

# Server (kept when one already runs on the bench port)
if (-not (Get-NetUDPEndpoint -LocalPort 60000 -ErrorAction SilentlyContinue)) {
  Start-Process $Server -ArgumentList '--config', "`"$(Join-Path $Bench 'server.json')`"" -WindowStyle Minimized `
    -RedirectStandardOutput (Join-Path $Logs "bench-$Stamp-server.log") -RedirectStandardError (Join-Path $Logs "bench-$Stamp-server.err.log")
  Start-Sleep 3
}

foreach ($Tool in 'make_vssetting.py', 'make_festival.py') {
  cmd /c "py -3 `"$(Join-Path $Root "tools\boss\$Tool")`" > `"$(Join-Path $Logs "bench-$Stamp-boss.log")`" 2>&1"
}
$env:RBREW_CONTENT_DIR = Join-Path $Game 'content'
$env:RBREW_TITLE_METADATA = Join-Path $Game 'meta\meta.xml'
$env:RBREW_SYSTEM_MLC_DIR = Join-Path $Game 'mlc01'
$env:RBREW_VK_CACHE_DIR = Join-Path $Game 'vulkan_cache'
$env:RBREW_BOSS_DIR = Join-Path $Game 'boss'
$env:RBREW_ONLINE_SERVER = "$($Lan):60000"
$env:RBREW_DNS_OVERRIDE = "nncs1.app.nintendowifi.net=$Lan,nncs2.app.nintendowifi.net=$Lan"
$env:SDL_DIRECTINPUT_ENABLED = '0'
$env:RBREW_DUO_NO_AFK = '1'
$Players = @()
foreach ($Name in 'A', 'B') {
  $Dir = Use-Bench (Join-Path $Bench "players\$Name")
  if (-not (Test-Path (Join-Path $Dir 'save\save.dat')) -and (Test-Path (Join-Path $Game 'save\save.dat'))) {
    Copy-Item (Join-Path $Game 'save\save.dat') (Join-Path $Dir 'save\save.dat')
  }
  Copy-Item $Exe (Join-Path $Dir $ExeName) -Force
  Get-ChildItem $Game -Filter '*.dll' | Copy-Item -Destination $Dir -Force
  $Ini = @()
  if (Test-Path (Join-Path $Game 'settings.ini')) {
    $Ini = Get-Content (Join-Path $Game 'settings.ini') | Where-Object { $_ -notmatch '^(fullscreen|pseudo|online_server)=' }
  }
  $Ini + @('fullscreen=0', "pseudo=Joueur$Name") | Set-Content (Join-Path $Dir 'settings.ini') -Encoding ascii
  $env:RBREW_AUDIO_MUTE = if ($Name -eq 'B') { '1' } else { '' }
  $env:RBREW_FRIENDS_DIR = Join-Path $Dir 'friends'
  New-Item -ItemType Directory -Path $env:RBREW_FRIENDS_DIR -Force | Out-Null
  $env:RBREW_NET_TRACE = Join-Path $Logs "bench-$Stamp-$Name-net.txt"
  $Players += Start-Process (Join-Path $Dir $ExeName) -WorkingDirectory $Dir -PassThru `
    -RedirectStandardOutput (Join-Path $Logs "bench-$Stamp-$Name-stdout.log") -RedirectStandardError (Join-Path $Logs "bench-$Stamp-$Name-stderr.log")
  Start-Process py -ArgumentList '-3', "`"$(Join-Path $Root 'scripts\friends_relay.py')`"", '--game-dir', "`"$Dir`"", '--server', 'http://127.0.0.1:60002', '--watch-pid', "$($Players[-1].Id)" -WindowStyle Hidden
  Start-Sleep 2
}
$Players | ForEach-Object { $_.WaitForExit() }
Get-Process splatoon-nex -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Server } | Stop-Process -Force
