param(
  [switch]$CaptureTrace,
  [switch]$Ring,
  [switch]$Friends   # test the friends feature: each window gets its own friends folder and relay (see friends_relay.py)
)
# Two players on this PC against the PUBLIC NEX server (scripts\server.txt: server=, nncs1=, nncs2=).
# Same layout as Launch-Duo.ps1 (each player in build\duo\A and build\duo\B, own identity, save and
# settings, windowed) but no local server is started: both games connect to the real one. Both
# identities must be registered on that server (tools\nexlab\accounts.py add <pid>). The two games
# share this PC's router, so this checks the server's matchmaking, not a connection between two
# different networks. Logs: build\diagnostics\duop-<date>-*.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Game = Join-Path $Root 'Game'
$Exe = Join-Path $Game 'Splatoon-v288.exe'
$Logs = Join-Path $Root 'build\diagnostics'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Path $Logs -Force | Out-Null

$Cfg = @{}
$File = Join-Path $PSScriptRoot 'server.txt'
if (-not (Test-Path -LiteralPath $File)) { $File = Join-Path $PSScriptRoot 'server.public.txt' }
if (Test-Path -LiteralPath $File) { foreach ($Line in Get-Content -LiteralPath $File) { if ($Line -match '^\s*(\w+)\s*=\s*(\S+)\s*$') { $Cfg[$Matches[1].ToLower()] = $Matches[2] } } }
if (-not $Cfg['server'] -or -not $Cfg['nncs1'] -or -not $Cfg['nncs2']) { throw 'scripts\server.txt needs server=, nncs1= and nncs2=.' }

foreach ($Tool in 'make_vssetting.py', 'make_festival.py') {
  cmd /c "py -3.12 `"$(Join-Path $Root "tools\boss\$Tool")`" > `"$(Join-Path $Logs "duop-$Stamp-boss.log")`" 2>&1"
}
$env:RBREW_CONTENT_DIR = Join-Path $Game 'content'
$env:RBREW_TITLE_METADATA = Join-Path $Game 'meta\meta.xml'
$env:RBREW_SYSTEM_MLC_DIR = Join-Path $Game 'mlc01'
$env:RBREW_VK_CACHE_DIR = Join-Path $Game 'vulkan_cache'
$env:RBREW_BOSS_DIR = Join-Path $Game 'boss'
$env:RBREW_ONLINE_SERVER = $Cfg['server']
$env:RBREW_DNS_OVERRIDE = "nncs1.app.nintendowifi.net=$($Cfg['nncs1']),nncs2.app.nintendowifi.net=$($Cfg['nncs2'])"
$env:SDL_DIRECTINPUT_ENABLED = '0'
# One person plays two windows: the idle one must not be kicked as AFK (test launchers only).
$env:RBREW_DUO_NO_AFK = '1'
# Default: both windows share one folder, so each sees the other as a friend (files, no server). With -Friends every
# window has its OWN folder and its own relay, like two real players: friends then go through the server (F4).
$SharedFriends = Join-Path $Root 'build\duo\friends'
Remove-Item -Path $SharedFriends -Recurse -Force -ErrorAction SilentlyContinue
if (-not $Friends) { $env:RBREW_FRIENDS_DIR = $SharedFriends } else { Remove-Item Env:RBREW_FRIENDS_DIR -ErrorAction SilentlyContinue }
$SocialUrl = ''
foreach ($ConfigFile in (Join-Path $PSScriptRoot 'server.txt'), (Join-Path $PSScriptRoot 'server.public.txt')) {
  if (-not $SocialUrl -and (Test-Path $ConfigFile)) {
    foreach ($Line in Get-Content $ConfigFile) { if ($Line -match '^\s*register\s*=\s*(https://[A-Za-z0-9.\-]+(:\d{1,5})?)/') { $SocialUrl = $Matches[1] } }
  }
}
$Players = @()
foreach ($Name in 'A', 'B') {
  $Dir = Join-Path $Root "build\duo\$Name"
  New-Item -ItemType Directory -Path (Join-Path $Dir 'save\common') -Force | Out-Null
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
  if ($Friends) {
    $env:RBREW_FRIENDS_DIR = Join-Path $Dir 'friends'
    New-Item -ItemType Directory -Path $env:RBREW_FRIENDS_DIR -Force | Out-Null
  }
  $env:RBREW_NET_TRACE = Join-Path $Logs "duop-$Stamp-$Name-net.txt"
  if ($CaptureTrace) { $env:RBREW_CAPTURE_TRACE = Join-Path $Logs "duop-$Stamp-$Name-capture.txt" } else { Remove-Item Env:RBREW_CAPTURE_TRACE -ErrorAction SilentlyContinue }
  # -Ring: recent guest virtual calls, written next to the net trace when a socket closes.
  if ($Ring) { $env:RBREW_DISPATCH_RING = Join-Path $Logs "duop-$Stamp-$Name-ring.bin" } else { Remove-Item Env:RBREW_DISPATCH_RING -ErrorAction SilentlyContinue }
  $Players += Start-Process (Join-Path $Dir 'Splatoon-v288.exe') -WorkingDirectory $Dir -PassThru `
    -RedirectStandardOutput (Join-Path $Logs "duop-$Stamp-$Name-stdout.log") -RedirectStandardError (Join-Path $Logs "duop-$Stamp-$Name-stderr.log")
  if ($Friends -and $SocialUrl) {
    Start-Process py -ArgumentList '-3.12', "`"$(Join-Path $PSScriptRoot 'friends_relay.py')`"", '--game-dir', "`"$Dir`"", '--server', $SocialUrl, '--watch-pid', "$($Players[-1].Id)" -WindowStyle Hidden
  }
  Start-Sleep 2
}
$Players | ForEach-Object { $_.WaitForExit() }
