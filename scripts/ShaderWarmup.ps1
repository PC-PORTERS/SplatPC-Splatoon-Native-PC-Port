# ShaderWarmup.ps1 - last step of Setup: one unattended start of the game, so
# the first real start does not freeze while the shaders are translated.
#
# The game plays a short pad script in a window (title screen, then the plaza)
# and fills <GameFolder>\vulkan_cache. That cache is built on this PC from the
# player's own game and is the only thing left behind: the save goes to a
# temporary folder (a copy of the existing one, so the plaza is reached), the
# settings are put back, and whatever else the start created is removed.
#
# Never fatal: every failure ends as a warning and Setup carries on.
param(
    [Parameter(Mandatory = $true)][string]$GameFolder,
    [Parameter(Mandatory = $true)][string]$WorkFolder, # scratch space, e.g. Setup's build folder
    [int]$Frames = 3300,                               # frames to play before closing
    [int]$TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'

function Note([string]$Text, [string]$Color = 'Gray') { Write-Host "      $Text" -ForegroundColor $Color }

# Last frame number in the game's frame log.
function Last-Frame([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return 0 }
    $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $size = [Math]::Min(400, $stream.Length)
        [void]$stream.Seek(-$size, 'End')
        $bytes = New-Object byte[] $size
        $read = $stream.Read($bytes, 0, $size)
    } finally { $stream.Dispose() }
    $lines = [Text.Encoding]::ASCII.GetString($bytes, 0, $read) -split "`n"
    for ($i = $lines.Count - 2; $i -ge 1; $i--) { # skip the cut first line and the unfinished last one
        if ($lines[$i] -match '^(\d+),') { return [int]$Matches[1] }
    }
    return 0
}

$exe = Join-Path $GameFolder 'Splatoon.exe'
$settings = Join-Path $GameFolder 'settings.ini'
$diagnostics = Join-Path $GameFolder 'Diagnostics'
$scratch = Join-Path $WorkFolder 'shader-warmup'
$frameLog = Join-Path $scratch 'frames.csv'
$environment = @{
    RBREW_SAVE_DIR      = Join-Path $scratch 'save'
    RBREW_INPUT_SCRIPT  = Join-Path $PSScriptRoot 'shader-warmup-input.txt'
    RBREW_BENCHMARK_CSV = $frameLog
    RBREW_AUDIO_MUTE    = '1'
}
# Settings of the warm-up run: a plain window, no helper windows, no question on closing.
$runSettings = [ordered]@{
    fullscreen = 0; borderless = 0; gamepad_window = 0; phone_gamepad = 0
    fps_overlay = 0; simple_overlay = 0; confirm_quit = 0
}

$process = $null
$previousEnvironment = @{}
$savedSettings = $null
$existing = @()
$existingReports = @()
$started = $false
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
    if (-not (Test-Path -LiteralPath $exe)) { throw 'Splatoon.exe is missing.' }
    $existing = @(Get-ChildItem -LiteralPath $GameFolder -Force | ForEach-Object Name)
    if (Test-Path -LiteralPath $diagnostics) { $existingReports = @(Get-ChildItem -LiteralPath $diagnostics -Force | ForEach-Object Name) }

    if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $environment.RBREW_SAVE_DIR | Out-Null
    $save = Join-Path $GameFolder 'save'
    if (Test-Path -LiteralPath $save) { Copy-Item -Path (Join-Path $save '*') -Destination $environment.RBREW_SAVE_DIR -Recurse -Force }

    $kept = @()
    if (Test-Path -LiteralPath $settings) {
        $savedSettings = [IO.File]::ReadAllBytes($settings)
        $kept = @(Get-Content -LiteralPath $settings | Where-Object { $runSettings.Keys -notcontains ($_ -split '=')[0].Trim() })
    }
    $started = $true
    Set-Content -LiteralPath $settings -Encoding ASCII -Value ($kept + ($runSettings.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }))
    foreach ($name in $environment.Keys) {
        $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
        [Environment]::SetEnvironmentVariable($name, $environment[$name])
    }

    Note 'A game window opens and plays by itself for a minute or two. Leave it alone; it closes by itself.'
    $process = Start-Process -FilePath $exe -WorkingDirectory $GameFolder -PassThru
    $frame = 0
    $shown = -1
    while ($frame -lt $Frames -and -not $process.HasExited -and $watch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        Start-Sleep -Seconds 2
        $frame = Last-Frame $frameLog
        $percent = [Math]::Min(100, 10 * [Math]::Floor(10 * $frame / $Frames))
        if ($percent -ne $shown) { Note "Preparing shaders: $percent %"; $shown = $percent }
    }
    if ($process.HasExited) { throw "the game closed early (exit code $($process.ExitCode))." }
    if ($frame -lt $Frames) { Note "The game was slower than expected; stopping after $TimeoutSeconds seconds." 'Yellow' }
    [void]$process.CloseMainWindow() # a normal close also saves the pipeline cache
    [void]$process.WaitForExit(20000) # still open: the final block ends it
    Note "Shaders prepared in $([int]$watch.Elapsed.TotalSeconds) s; the first start will be smoother." 'Green'
} catch {
    Note "Shader warm-up skipped: $($_.Exception.Message)" 'Yellow'
    Note 'Nothing is broken: the game prepares its shaders during the first start instead.' 'Yellow'
} finally {
    try {
        if ($process -and -not $process.HasExited) { $process.Kill(); [void]$process.WaitForExit(10000) }
        foreach ($name in $previousEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name]) }
        if ($started) {
            if ($null -ne $savedSettings) { [IO.File]::WriteAllBytes($settings, $savedSettings) }
            elseif (Test-Path -LiteralPath $settings) { Remove-Item -LiteralPath $settings -Force }
            # Whatever else this start created, except the cache it was run for.
            Get-ChildItem -LiteralPath $GameFolder -Force | Where-Object { $existing -notcontains $_.Name -and $_.Name -ne 'vulkan_cache' } |
                Remove-Item -Recurse -Force
            if (Test-Path -LiteralPath $diagnostics) {
                Get-ChildItem -LiteralPath $diagnostics -Force | Where-Object { $existingReports -notcontains $_.Name } | Remove-Item -Recurse -Force
            }
        }
        if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
    } catch {
        Note "Could not tidy up after the shader warm-up: $($_.Exception.Message)" 'Yellow'
    }
}
