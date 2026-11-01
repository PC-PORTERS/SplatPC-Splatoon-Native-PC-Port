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
$Executable=Join-Path $Game 'Splatoon-v288.exe'
if(-not(Test-Path -LiteralPath $Executable)){throw 'Experimental update executable has not been built yet.'}
$LogDirectory=Join-Path $Root 'build\diagnostics'
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
$RunName=Get-Date -Format 'yyyyMMdd-HHmmss'
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
  Write-Host "Serveur NEX distant demandé ($NexServer), mais le jeu est encore confiné au réseau local" -ForegroundColor Yellow
  Write-Host "(étape 5 non faite : nsysnet n'autorise que 127.0.0.0/8). Le login échouerait." -ForegroundColor Yellow
  Write-Host "Utilisez le serveur local (sans -NexServer), ou relancez avec -Force pour tester malgré tout."
  exit 1
}

if($Port -lt 1 -or $Port -gt 65534){throw 'Le port NEX doit être compris entre 1 et 65534 (serveur sécurisé : port+1).'}
if(-not(Get-Command py -ErrorAction SilentlyContinue)){throw 'Python 3.12 est requis : lanceur py introuvable.'}
$Python=(& py -3.12 -c 'import sys; print(sys.executable)' 2>$null)
if($LASTEXITCODE -ne 0 -or -not $Python){throw 'Python 3.12 est requis et ne peut pas être lancé.'}
$Python=([string]$Python).Trim()
$Server=$null
$Relay=$null
try {
if($LocalServer){
$Running=Get-CimInstance Win32_Process -Filter "Name like 'py%' or Name like 'python%'" | Where-Object { $_.CommandLine -match 'nexserver\.py' -and ($_.CommandLine -match ('--port[ ="'']+'+$Port+'(?:[ "'']|$)') -or ($Port -eq 60000 -and $_.CommandLine -notmatch '--port')) }
if(-not $Running){
  $Busy=Get-NetUDPEndpoint -LocalPort $Port,($Port+1) -ErrorAction SilentlyContinue
  if($Busy){throw "Port UDP $Port ou $($Port+1) déjà occupé ; fermez le programme concerné."}
  $Script=Join-Path $Root 'tools\nexlab\nexserver.py'
  $Log=Join-Path $LogDirectory ('online-'+$RunName+'-server.log')
  # python.exe itself (py.exe runs it as a child, which Stop-Process would leave running)
  # Python 3.12 was checked before starting the server.
  $Server=Start-Process -FilePath $Python -ArgumentList "`"$Script`"",'--port',"$Port",'--log',"`"$Log`"" -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds 2
  if($Server.HasExited){throw "The NEX lab server did not start; see $Log"}
}
}

  # SpotPass schedule (stages, weapon unlock dates) from Game\boss\planning.json, dated from now.
  & py -3.12 (Join-Path $Root 'tools\boss\make_vssetting.py') *> (Join-Path $LogDirectory ('online-'+$RunName+'-schedule.log'))
  # SpotPass festival (Festival.byaml + textures) from Game\boss\festival.json.
  & py -3.12 (Join-Path $Root 'tools\boss\make_festival.py') *>> (Join-Path $LogDirectory ('online-'+$RunName+'-schedule.log'))
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
  if($SocialUrl -and $SocialUrl -match '^(https://[A-Za-z0-9.\-]+(:\d{1,5})?|http://(127\.0\.0\.1|localhost)(:\d{1,5})?)$'){
    $Relay=Start-Process -FilePath $Python -ArgumentList "`"$(Join-Path $PSScriptRoot 'friends_relay.py')`"",'--game-dir',"`"$Game`"",'--server',$SocialUrl,'--watch-pid',"$($Process.Id)" -WindowStyle Hidden -PassThru
  }
  $Process.WaitForExit()
  $Process.ExitCode | Set-Content -LiteralPath (Join-Path $LogDirectory ('online-'+$RunName+'-exit.txt'))
}
finally {
  if($Relay -and -not $Relay.HasExited){Stop-Process -Id $Relay.Id -Force -ErrorAction SilentlyContinue}
  if($Server -and -not $Server.HasExited){Stop-Process -Id $Server.Id -Force}
}


