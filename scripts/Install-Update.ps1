param(
  [string]$UpdateDump = '',
  [string]$BaseDump = '',
  [string]$DependencyRoot = '',
  [string]$GameRoot = '',
  [switch]$ValidateOnly,
  [switch]$CheckDependencies
)
# Separate installer for the v288 update, started by Setup.bat after the base install. It never copies
# Nintendo files into the repository: everything is staged under build\ by Build-Update.ps1.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$ManifestDir = Join-Path $Root 'Manifest'

function Fail([string]$Message) {
  Write-Host ''
  Write-Host "ERROR: $Message" -ForegroundColor Red
  exit 2
}

function Read-DumpPath([string]$Prompt) {
  Write-Host $Prompt
  $value = Read-Host 'Folder (contains code, content and meta)'
  if (-not $value) { Fail 'No folder given.' }
  return $value.Trim('"').Trim()
}

function Ask-Folder([string]$Description) {
  $dialog = $null
  $owner = $null
  try {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = $Description
    $dialog.ShowNewFolderButton = $false
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
      return $dialog.SelectedPath
    }
  } catch {
    Write-Host "The folder picker could not open: $($_.Exception.Message)"
  } finally {
    if ($dialog) { $dialog.Dispose() }
    if ($owner) { $owner.Dispose() }
  }
  return ''
}

function Test-DumpFolder([string]$Path, [string]$Label) {
  if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
    Fail "$Label folder not found: $Path"
  }
  foreach ($part in 'code', 'content', 'meta') {
    if (-not (Test-Path -LiteralPath (Join-Path $Path $part) -PathType Container)) {
      Fail "$Label is not a game dump: missing '$part' in $Path"
    }
  }
}

function Test-UpdateDump([string]$Path, $Profile) {
  Test-DumpFolder $Path 'Update dump'
  $rpx = Join-Path $Path 'code\Gambit.rpx'
  if (-not (Test-Path -LiteralPath $rpx -PathType Leaf)) { Fail "Missing code\Gambit.rpx in $Path" }
  $hash = (Get-FileHash -LiteralPath $rpx -Algorithm SHA256).Hash
  if ($hash -ne $Profile.rpx_sha256) {
    Fail "This is not the v288 update: Gambit.rpx SHA-256 is $hash, expected $($Profile.rpx_sha256)."
  }
  [xml]$meta = Get-Content -LiteralPath (Join-Path $Path 'meta\meta.xml') -Raw
  $version = $meta.SelectSingleNode('/menu/title_version').InnerText
  if ($version -ne '288') { Fail "Expected title version 288, found '$version'." }
  Write-Host "Update dump OK: revision 288, RPX hash matches." -ForegroundColor Green
}

function Get-SetupState([string]$Name) {
  $path = Join-Path $Root (Join-Path 'build' $Name)
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return '' }
  return (Get-Content -LiteralPath $path -Raw).Trim()
}

function Get-GameRoot([string]$Override) {
  if ($Override) { return $Override }
  $saved = Get-SetupState 'setup-output.txt'
  if ($saved -and (Test-Path -LiteralPath $saved -PathType Container)) { return $saved }
  return (Join-Path $Root 'Game')
}

function Test-BaseInstall([string]$GamePath) {
  $exe = Join-Path $GamePath 'Splatoon.exe'
  $content = Join-Path $GamePath 'content'
  if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    Fail 'The base game is not built yet. Run Setup.bat (or scripts\Setup.ps1) first, then run this installer.'
  }
  if (-not (Test-Path -LiteralPath $content -PathType Container)) {
    Fail "Base install is incomplete: missing $content. Re-run Setup.bat."
  }
  Write-Host "Base install OK: $exe" -ForegroundColor Green
}

function Find-BaseDump($Profile) {
  # Fallback when Setup's recorded folder is gone: the usual dump locations.
  $downloads = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Downloads\Splatoon [Game] [0005000010176a00]'
  foreach ($candidate in @($downloads, (Join-Path $Root 'dump'), (Join-Path $Root 'game-dump'))) {
    $rpx = Join-Path $candidate 'code\Gambit.rpx'
    if ((Test-Path -LiteralPath $rpx -PathType Leaf) -and
        ((Get-FileHash -LiteralPath $rpx -Algorithm SHA256).Hash -eq $Profile.base_rpx_sha256)) {
      return $candidate
    }
  }
  return $null
}

function Resolve-DependencyRoot() {
  # A dependency root is the folder that CONTAINS build\vcpkg (normally the repository root).
  $candidates = @($Root, (Split-Path -Parent $Root))
  if ($DependencyRoot) { $candidates += $DependencyRoot }
  foreach ($candidate in $candidates) {
    $marker = Join-Path $candidate 'build\vcpkg\scripts\buildsystems\vcpkg.cmake'
    if (Test-Path -LiteralPath $marker -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }
  Fail 'Prepared dependencies not found. Pass -DependencyRoot with the folder that contains build\vcpkg, build\deps\sdl2-vulkan and build\deps\glslang-vulkan (normally the upstream checkout root).'
}

function Get-ManifestValue([string]$Path,[string]$Key){$Line=Get-Content -LiteralPath $Path | Where-Object { $_ -match "^\s*$Key\s*=" } | Select-Object -First 1;if(-not $Line){throw "Missing $Key in $Path"};return (($Line -split '=',2)[1] -split '#')[0].Trim()}
# Image digests come from the manifests: the base game (v16) and the update (288).
$Profile = [pscustomobject]@{base_rpx_sha256 = Get-ManifestValue (Join-Path $ManifestDir 'Gambit_v16.manifest') 'image_sha256'; rpx_sha256 = Get-ManifestValue (Join-Path $ManifestDir 'Gambit_v288.manifest') 'image_sha256'}
if ($CheckDependencies) {
  $deps = Resolve-DependencyRoot
  Write-Host "Dependencies OK: $deps" -ForegroundColor Green
  exit 0
}

Write-Host 'Splatoon PC port - update to revision 288'
Write-Host 'This keeps your existing installation and only builds the updated game.'
Write-Host ''

$gameDir = Get-GameRoot $GameRoot
Test-BaseInstall $gameDir
if (-not $UpdateDump -and $env:SPLATOON_UPDATE_DUMP) { $UpdateDump = $env:SPLATOON_UPDATE_DUMP }
if (-not $UpdateDump) {
  $UpdateDump = Ask-Folder 'Select the UPDATE dump for revision 288 (contains code, content and meta).'
}
if (-not $UpdateDump) {
  $UpdateDump = Read-DumpPath 'Select the UPDATE dump (revision 288, folders code, content, meta).'
}
Test-UpdateDump $UpdateDump $Profile

if (-not $BaseDump) {
  # The folder Setup recorded, if it still holds a dump (it may have moved since).
  $saved = Get-SetupState 'setup-base-dump.txt'
  if ($saved -and (Test-Path -LiteralPath (Join-Path $saved 'code\Gambit.rpx') -PathType Leaf)) { $BaseDump = $saved }
}
if (-not $BaseDump) { $BaseDump = Find-BaseDump $Profile }
if (-not $BaseDump) {
  Write-Host 'The base game dump used by Setup was not found.'
  $BaseDump = Ask-Folder 'Select the BASE game dump used by Setup (contains code\Gambit.rpx).'
}
if (-not $BaseDump) {
  $BaseDump = Read-DumpPath 'Select the BASE game dump (the EUR dump used by Setup; it contains code\Gambit.rpx).'
}
Test-DumpFolder $BaseDump 'Base dump'
$baseRpx = Join-Path $BaseDump 'code\Gambit.rpx'
if (-not (Test-Path -LiteralPath $baseRpx -PathType Leaf)) { Fail "Missing code\Gambit.rpx in $BaseDump" }
$baseHash = (Get-FileHash -LiteralPath $baseRpx -Algorithm SHA256).Hash
if ($baseHash -ne $Profile.base_rpx_sha256) {
  Fail "Base RPX mismatch: $baseHash, expected $($Profile.base_rpx_sha256). Use your EUR AGMP01 dump."
}

if ($ValidateOnly) {
  Write-Host 'Validation only: inputs look correct, nothing was built.' -ForegroundColor Green
  exit 0
}

$buildUpdate = Join-Path $PSScriptRoot 'Build-Update.ps1'
if (-not (Test-Path -LiteralPath $buildUpdate -PathType Leaf)) { Fail "Missing $buildUpdate" }
$deps = Resolve-DependencyRoot
# As in Setup.ps1: the compiler cannot open include paths with non-ASCII letters, so build
# through a drive letter mapped to the same folder (subst), removed again afterwards.
$buildDrive = $null
if ($Root -match '[^\x00-\x7F]') {
  $mapped = subst | Where-Object { $_ -match '^([A-Z]):\\: => (.*)$' -and $matches[2].TrimEnd('\') -ieq $Root.TrimEnd('\') } |
    ForEach-Object { $_.Substring(0, 2) } | Select-Object -First 1
  if (-not $mapped) {
    $free = @('S','T','U','V','W','X','Y','Z','R','Q','P') | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
    if ($free) { subst "${free}:" "$Root" 2>$null | Out-Null; if (Test-Path -LiteralPath "${free}:\scripts\Build-Update.ps1") { $mapped = "${free}:"; $buildDrive = $mapped } }
  }
  if (-not $mapped) { Fail "Your folder path contains special characters ($Root) and no free drive letter was found. Move the project to a folder like C:\SplatoonPort." }
  $buildUpdate = Join-Path "$mapped\" 'scripts\Build-Update.ps1'
  if ($deps.TrimEnd('\') -ieq $Root.TrimEnd('\')) { $deps = "$mapped\" }
}
Write-Host ''
Write-Host 'Starting the update build (same steps as Build-Update.ps1). This takes 20-60 minutes.' -ForegroundColor Cyan
try {
  & $buildUpdate -BaseDump $BaseDump -UpdateDump $UpdateDump -DependencyRoot $deps -Output $gameDir -Optimize
  $buildExit = $LASTEXITCODE
} finally {
  if ($buildDrive) { subst "$buildDrive" /D 2>$null | Out-Null }
}
if ($buildExit -ne 0) { Fail "Update build failed with exit code $buildExit." }
Write-Host ''
Write-Host "Update installed. Start the game with $(Join-Path $gameDir 'Splatoon-v288.exe')." -ForegroundColor Green

$shortcutPath = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Splatoon PC Port.lnk'
if (Test-Path -LiteralPath $shortcutPath -PathType Leaf) {
  $gameDir = Get-GameRoot $GameRoot
  $updateExe = Join-Path $gameDir 'Splatoon-v288.exe'
  if (Test-Path -LiteralPath $updateExe -PathType Leaf) {
    try {
      $shell = New-Object -ComObject WScript.Shell
      $shortcut = $shell.CreateShortcut($shortcutPath)
      $shortcut.TargetPath = $updateExe
      $shortcut.WorkingDirectory = $gameDir
      $shortcut.IconLocation = "$updateExe,0"
      $shortcut.Save()
      Write-Host 'Desktop shortcut updated to Splatoon-v288.exe.' -ForegroundColor Green
    } catch {
      Write-Host "The desktop shortcut could not be updated: $($_.Exception.Message)" -ForegroundColor Yellow
    }
  }
}
