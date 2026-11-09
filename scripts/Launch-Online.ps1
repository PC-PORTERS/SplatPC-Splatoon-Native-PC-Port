param([int]$Port = 60000, [switch]$Diag, [string]$NexServer = '', [string]$Nncs1 = '', [string]$Nncs2 = '',
      [switch]$Force, [string]$SocialUrl = '')
# -Diag: network trace with a probe on Talk_makeLabel (0209674C, shop dialogue labels) and the
# import trace (system calls with their guest call chains), in build\diagnostics\online-*-*.txt.
# Online (lab) launch of the experimental update: starts the NEX lab
# server (tools\nexlab\nexserver.py) unless one already runs, launches the game with
# RBREW_ONLINE_SERVER pointing at it, and stops the server it started when the game exits.
# settings.ini is not modified: the plain shortcut keeps launching offline.
$ErrorActionPreference='Stop'
$Root=Split-Path -Parent $PSScriptRoot
$Game=Join-Path $Root 'Game'
# Setup.ps1 installs Game\Splatoon.exe; older installs have Splatoon-v288.exe: the newer one wins.
$Executable=@('Splatoon.exe', 'Splatoon-v288.exe') | ForEach-Object { Join-Path $Game $_ } | Where-Object { Test-Path -LiteralPath $_ } |
    Sort-Object { (Get-Item -LiteralPath $_).LastWriteTime } -Descending | Select-Object -First 1
if(-not $Executable){$Executable=Join-Path $Game 'Splatoon.exe'}
$LogDirectory=Join-Path $Root 'build\diagnostics'
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
$RunName=Get-Date -Format 'yyyyMMdd-HHmmss'
$LauncherLog=Join-Path $LogDirectory ('online-'+$RunName+'-launcher.log')
function Write-LauncherLog([string]$Text){ try { Add-Content -LiteralPath $LauncherLog -Value "$(Get-Date -Format 'HH:mm:ss') $Text" } catch {} }
# The sign-in screen starts this script in a hidden window: any failure must be shown and
# logged, or the player only sees everything close (issue #32).
trap {
  Write-LauncherLog "ERROR: $_"
  try {
    Add-Type -AssemblyName System.Windows.Forms
    [void][System.Windows.Forms.MessageBox]::Show("The online launch failed:`n`n$_`n`nLog: $LauncherLog", 'Splatoon')
  } catch {}
  exit 1
}
if(-not(Test-Path -LiteralPath $Executable)){throw 'Game\Splatoon.exe has not been built yet.'}
# Old run logs are removed after 7 days so they never fill the disk.
Get-ChildItem -LiteralPath $LogDirectory -Filter 'online-*' -File -ErrorAction SilentlyContinue |
  Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-7) } | Remove-Item -Force -ErrorAction SilentlyContinue
# Remote (public) server: -NexServer host:port -Nncs1 ip -Nncs2 ip. Refused without -Force:
# the game is still confined to 127.0.0.0/8 (step 5), so only a local server can answer.
# The defaults keep the lab exactly as before: local server on 127.0.0.1 and the two
# loopback nncs addresses.
if(-not $NexServer){ $NexServer = "127.0.0.1:$Port" }
if(-not $Nncs1){ $Nncs1 = '127.0.0.1' }
if(-not $Nncs2){ $Nncs2 = '127.0.0.2' }
$LocalServer = $NexServer -match '^(127\.|localhost(:|$))'

# B-02: nsysnet still confines every socket to 127.0.0.0/8 (net_policy.h, step 5 not done).
# A remote NEX server can therefore never be reached: say so instead of launching a game
# that fails its login with an obscure communication error. -Force is the explicit override.
if(-not $LocalServer -and -not $Force){
  Write-Host "Remote NEX server requested ($NexServer): pass -Force to use it (the sign-in screen does)." -ForegroundColor Yellow
  exit 1
}

if($Port -lt 1 -or $Port -gt 65534){throw 'The NEX port must be between 1 and 65534 (the secure server uses port+1).'}
# Any Python 3.10+ works (the helper tools only use the standard library), found the same way
# as Setup.ps1. Playing online needs it: friends_relay.py is the only one that sends the integrity
# proof, so without Python the server refuses every login. Fail here, before the game starts.
$Python=$null
foreach($Candidate in @(@('py','-3'),@('python'),@('python3'))){
  if(-not(Get-Command $Candidate[0] -ErrorAction SilentlyContinue)){continue}
  $CandidateArgs=@($Candidate | Select-Object -Skip 1)
  try { $Found=(& $Candidate[0] @CandidateArgs -c 'import sys; assert sys.version_info >= (3, 10); print(sys.executable)' 2>$null) } catch { $Found=$null }
  if($LASTEXITCODE -eq 0 -and "$Found".Trim()){ $Python="$Found".Trim(); break }
}
if(-not $Python){ throw 'Python 3.10 or newer is required to play online. Install it from python.org, then try again.' }
Write-LauncherLog "Python: $Python"
$Server=$null
$Relay=$null
try {
if($LocalServer){
$Running=Get-CimInstance Win32_Process -Filter "Name like 'py%' or Name like 'python%'" | Where-Object { $_.CommandLine -match 'nexserver\.py' -and ($_.CommandLine -match ('--port[ ="'']+'+$Port+'(?:[ "'']|$)') -or ($Port -eq 60000 -and $_.CommandLine -notmatch '--port')) }
if(-not $Running){
  $Busy=Get-NetUDPEndpoint -LocalPort $Port,($Port+1) -ErrorAction SilentlyContinue
  if($Busy){throw "UDP port $Port or $($Port+1) is already in use; close the program using it."}
  $Script=Join-Path $Root 'tools\nexlab\nexserver.py'
  $Log=Join-Path $LogDirectory ('online-'+$RunName+'-server.log')
  # python.exe itself (py.exe runs it as a child, which Stop-Process would leave running)
  # Python was checked before starting the server.
  $Server=Start-Process -FilePath $Python -ArgumentList "`"$Script`"",'--port',"$Port",'--log',"`"$Log`"" -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds 2
  if($Server.HasExited){throw "The NEX lab server did not start; see $Log"}
}
}

  # SpotPass schedule (stages, weapon unlock dates) from Game\boss\planning.json, dated from now.
  # SpotPass festival (Festival.byaml + textures) from Game\boss\festival.json.
  # A failing tool must not stop the launch: its output goes to the schedule log.
  if($Python){
    $Schedule=Join-Path $LogDirectory ('online-'+$RunName+'-schedule.log')
    foreach($Tool in 'make_vssetting.py','make_festival.py'){
      cmd /c "`"$Python`" `"$(Join-Path $Root "tools\boss\$Tool")`" >> `"$Schedule`" 2>&1"
      if($LASTEXITCODE -ne 0){ Write-LauncherLog "$Tool failed (exit $LASTEXITCODE), see $Schedule" }
    }
  }
  # Language: settings.ini language=, English by default (main.cpp).
  $env:RBREW_ONLINE_SERVER=$NexServer
  # Friends: the game reads <Game>\friends (one file per ONLINE friend); friends_relay.py fills it from the server
  # when -SocialUrl is given (the sign-in screen passes it). Without it the list stays empty.
  $FriendsDir=Join-Path $Game 'friends'
  New-Item -ItemType Directory -Path $FriendsDir -Force | Out-Null
  $env:RBREW_FRIENDS_DIR=$FriendsDir
  # NAT check before peer-to-peer: Nintendo's NNCS names go to the nncs1/nncs2 addresses
  # (lab loopback by default, the server's two public IPs with -Nncs1/-Nncs2).
  $env:RBREW_DNS_OVERRIDE="nncs1.app.nintendowifi.net=$Nncs1,nncs2.app.nintendowifi.net=$Nncs2"
  # 2026-10-28: SDL2's DirectInput haptic init fails on this machine ("Haptic error
  # CoCreateInstance"); SDL_Init then returns -1 and gx2_platform_open_window stops before
  # SDL_CreateWindow, so the game runs without a window. Skipping DirectInput restores the
  # window; controllers keep working through SDL's XInput/HIDAPI paths.
  $env:SDL_DIRECTINPUT_ENABLED='0'
  # A pseudo in settings.ini (pseudo=...) wins; otherwise a neutral name (never the Windows
  # user name, which other players would see).
  $Settings=Join-Path $Game 'settings.ini'
  $HasPseudo=(Test-Path -LiteralPath $Settings) -and (Select-String -LiteralPath $Settings -Pattern '^\s*pseudo\s*=\s*\S' -Quiet)
  if(-not $HasPseudo){$env:RBREW_PSEUDO='Player'}
  $env:RBREW_STUB_REPORT=Join-Path $LogDirectory ('online-'+$RunName+'-stubs.txt')
  if($Diag){
    $env:RBREW_INPUT_RECORD=Join-Path $LogDirectory ('online-'+$RunName+'-input.txt')   # replayable with RBREW_INPUT_SCRIPT
    $env:RBREW_SCAN_TRACE=Join-Path $LogDirectory ('online-'+$RunName+'-scan.txt')
    $env:RBREW_NET_TRACE=Join-Path $LogDirectory ('online-'+$RunName+'-net.txt')
    $env:RBREW_NET_PROBE='0209674C'
    $env:RBREW_IMPORT_TRACE=Join-Path $LogDirectory ('online-'+$RunName+'-imports.txt')
  }
  $Process=Start-Process -FilePath $Executable -WorkingDirectory $Game -WindowStyle Normal -RedirectStandardOutput (Join-Path $LogDirectory ('online-'+$RunName+'-stdout.log')) -RedirectStandardError (Join-Path $LogDirectory ('online-'+$RunName+'-stderr.log')) -PassThru
  Write-LauncherLog "Game started (server $NexServer)"
  if($Python -and $SocialUrl -and $SocialUrl -match '^(https://[A-Za-z0-9.\-]+(:\d{1,5})?|http://(127\.0\.0\.1|localhost)(:\d{1,5})?)$'){
    $Relay=Start-Process -FilePath $Python -ArgumentList "`"$(Join-Path $PSScriptRoot 'friends_relay.py')`"",'--game-dir',"`"$Game`"",'--server',$SocialUrl,'--watch-pid',"$($Process.Id)" -WindowStyle Hidden -PassThru
  }
  $Process.WaitForExit()
  $Process.ExitCode | Set-Content -LiteralPath (Join-Path $LogDirectory ('online-'+$RunName+'-exit.txt'))
  Write-LauncherLog "Game exited with code $($Process.ExitCode)"
}
finally {
  if($Relay -and -not $Relay.HasExited){Stop-Process -Id $Relay.Id -Force -ErrorAction SilentlyContinue}
  if($Server -and -not $Server.HasExited){Stop-Process -Id $Server.Id -Force}
}


