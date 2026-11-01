param(
  [int]$Port = 60000,
  [string]$ReplayA = '',
  [string]$ReplayB = '',
  [switch]$StrictMemory,
  [switch]$CallRing,
  [switch]$SleepTrace,
  [switch]$CaptureTrace
)
# Two players on this PC: starts the NEX lab server and two copies of the
# game, each in its own folder (build\duo\A and build\duo\B) with its own online identity,
# save copy and settings, windowed. Game\ and Game\save are only read. The server stops when
# both games are closed. Logs: build\diagnostics\duo-<date>-*.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Game = Join-Path $Root 'Game'
$Exe = Join-Path $Game 'Splatoon-v288.exe'
$Logs = Join-Path $Root 'build\diagnostics'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Path $Logs -Force | Out-Null
if (-not (Get-Command py -ErrorAction SilentlyContinue)) { throw 'Python launcher (py) not found.' }

# SpotPass data (schedule, festival) generated from Game\boss\*.json; without it the game stays "offline".
foreach ($Tool in 'make_vssetting.py', 'make_festival.py') {
  cmd /c "py -3.12 `"$(Join-Path $Root "tools\boss\$Tool")`" > `"$(Join-Path $Logs "duo-$Stamp-boss.log")`" 2>&1"
}
$Server = Start-Process py -ArgumentList '-3.12', (Join-Path $Root 'tools\nexlab\nexserver.py'), '--port', $Port,
  '--log', (Join-Path $Logs "duo-$Stamp-server.log"), '--identity-root', (Join-Path $Root 'build\duo') -PassThru -WindowStyle Minimized
try {
  Start-Sleep 3
  $env:RBREW_CONTENT_DIR = Join-Path $Game 'content'
  $env:RBREW_TITLE_METADATA = Join-Path $Game 'meta\meta.xml'
  $env:RBREW_SYSTEM_MLC_DIR = Join-Path $Game 'mlc01'
  $env:RBREW_VK_CACHE_DIR = Join-Path $Game 'vulkan_cache'
  $env:RBREW_BOSS_DIR = Join-Path $Game 'boss'
  $env:RBREW_ONLINE_SERVER = "127.0.0.1:$Port"
  $env:RBREW_DNS_OVERRIDE = 'nncs1.app.nintendowifi.net=127.0.0.1,nncs2.app.nintendowifi.net=127.0.0.2'
  # 2026-10-28: SDL2's DirectInput haptic init fails on this machine ("Haptic error
  # CoCreateInstance"); SDL_Init then returns -1 and gx2_platform_open_window stops before
  # SDL_CreateWindow, so the games run without a window. Skipping DirectInput restores them;
  # controllers keep working through SDL's XInput/HIDAPI paths.
  $env:SDL_DIRECTINPUT_ENABLED = '0'
  # One person plays two windows: the idle one must not be kicked as AFK (test launchers only).
  $env:RBREW_DUO_NO_AFK = '1'
  # Friend list shared by the two games (fp_lab.cpp): each sees the other as an online friend.
  $Friends = Join-Path $Root 'build\duo\friends'
  Remove-Item -Path $Friends -Recurse -Force -ErrorAction SilentlyContinue
  $env:RBREW_FRIENDS_DIR = $Friends
  $Players = @()
  foreach ($Name in 'A', 'B') {
    $Dir = Join-Path $Root "build\duo\$Name"
    New-Item -ItemType Directory -Path (Join-Path $Dir 'save\common') -Force | Out-Null
    # First launch only: the save copy and the identity then stay with this player.
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
    $env:RBREW_INPUT_RECORD = Join-Path $Logs "duo-$Stamp-$Name-input.txt"
    $env:RBREW_NET_TRACE = Join-Path $Logs "duo-$Stamp-$Name-net.txt"
    # Layout capture trace (map name at the intro): opt-in, it logs every capture bind.
    if ($CaptureTrace) { $env:RBREW_CAPTURE_TRACE = Join-Path $Logs "duo-$Stamp-$Name-capture.txt" } else { Remove-Item Env:RBREW_CAPTURE_TRACE -ErrorAction SilentlyContinue }
    $env:RBREW_HANG_SAMPLE = 'freeze'   # names the stuck guest functions in stderr if the game loop freezes
    $Replay = if ($Name -eq 'A') { $ReplayA } else { $ReplayB }
    if ($Replay) {
      if (-not (Test-Path -LiteralPath $Replay -PathType Leaf)) { throw "Replay $Name not found: $Replay" }
      $env:RBREW_INPUT_SCRIPT = (Resolve-Path -LiteralPath $Replay).Path
    } else { Remove-Item Env:RBREW_INPUT_SCRIPT -ErrorAction SilentlyContinue }
    if ($StrictMemory) {
      $env:RBREW_STRICT_MEMORY = '1'
      $env:RBREW_STRICT_MEMORY_DUMP = Join-Path $Logs "duo-$Stamp-$Name-strict-memory.dmp"
    } else {
      Remove-Item Env:RBREW_STRICT_MEMORY -ErrorAction SilentlyContinue
      Remove-Item Env:RBREW_STRICT_MEMORY_DUMP -ErrorAction SilentlyContinue
    }
    if ($CallRing) { $env:RBREW_CALL_RING = '1' } else { Remove-Item Env:RBREW_CALL_RING -ErrorAction SilentlyContinue }
    if ($SleepTrace) { $env:RBREW_SLEEP_TRACE = Join-Path $Logs "duo-$Stamp-$Name-sleep.txt" } else { Remove-Item Env:RBREW_SLEEP_TRACE -ErrorAction SilentlyContinue }
    $Players += Start-Process (Join-Path $Dir 'Splatoon-v288.exe') -WorkingDirectory $Dir -PassThru `
      -RedirectStandardOutput (Join-Path $Logs "duo-$Stamp-$Name-stdout.log") -RedirectStandardError (Join-Path $Logs "duo-$Stamp-$Name-stderr.log")
    Start-Sleep 2
  }
  $Players | ForEach-Object { $_.WaitForExit() }
} finally {
  if (-not $Server.HasExited) { Stop-Process -Id $Server.Id -Force -ErrorAction SilentlyContinue }
  Get-CimInstance Win32_Process -Filter "Name='python.exe'" | Where-Object { $_.CommandLine -like '*nexserver.py*' -and $_.ParentProcessId -eq $Server.Id } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
