param(
  [string]$UpdateDump = '',
  [string]$BaseDump = '',
  [string]$DependencyRoot = '',
  [string]$GameRoot = '',
  [switch]$ValidateOnly,
  [switch]$CheckDependencies
)
# Separate installer for game updates (the image is chosen by its manifest), started by Setup.bat after the base install. It never copies
# Nintendo files into the repository: everything is staged under build\.
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

function Get-ManifestImages() {
  # Every Manifest\*.manifest describes one image; read only what the installer needs.
  foreach ($file in (Get-ChildItem -LiteralPath $ManifestDir -Filter '*.manifest' -File)) {
    [pscustomobject]@{
      Path    = $file.FullName
      TitleId = Get-ManifestValue $file.FullName 'title_id'
      Sha     = Get-ManifestValue $file.FullName 'image_sha256'
      Version = Get-ManifestValue $file.FullName 'app_version'
      Product = Get-ManifestValue $file.FullName 'product_code'
    }
  }
}

function Resolve-UpdateImage([string]$RpxHash) {
  $images = @(Get-ManifestImages)
  $update = $images | Where-Object { $_.TitleId -match '^0x0005000E' -and $_.Sha -eq $RpxHash } | Select-Object -First 1
  if (-not $update) {
    $known = ($images | Where-Object { $_.TitleId -match '^0x0005000E' } | ForEach-Object { "v$($_.Version)" }) -join ', '
    Fail "This is not a supported game update: Gambit.rpx SHA-256 is $RpxHash. Supported updates: $known."
  }
  $base = $images | Where-Object { $_.TitleId -match '^0x00050000' -and $_.Product -eq $update.Product } | Select-Object -First 1
  if (-not $base) { Fail "No base game manifest matches the update v$($update.Version) ($($update.Product))." }
  return [pscustomobject]@{ base_rpx_sha256 = $base.Sha; rpx_sha256 = $update.Sha; version = $update.Version; update_manifest = $update.Path }
}

function Test-UpdateDump([string]$Path) {
  Test-DumpFolder $Path 'Update dump'
  $rpx = Join-Path $Path 'code\Gambit.rpx'
  if (-not (Test-Path -LiteralPath $rpx -PathType Leaf)) { Fail "Missing code\Gambit.rpx in $Path" }
  $image = Resolve-UpdateImage (Get-FileHash -LiteralPath $rpx -Algorithm SHA256).Hash
  [xml]$meta = Get-Content -LiteralPath (Join-Path $Path 'meta\meta.xml') -Raw
  $version = $meta.SelectSingleNode('/menu/title_version').InnerText
  if ($version -ne $image.version) { Fail "Expected title version $($image.version), found '$version'." }
  Write-Host "Update dump OK: revision $($image.version), RPX hash matches." -ForegroundColor Green
  return $image
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
# The update build: recompiles the update image with its manifest and packages
# Splatoon-v<version>.exe next to the base game. $Root may be a mapped drive (see below).
function Invoke-UpdateBuild([string]$Root, [string]$BaseDump, [string]$UpdateDump, [string]$DependencyRoot,
                            [string]$Output = '', [bool]$Diagnostics = $false, [bool]$Optimize = $false, $Image = $null) {
  $Version = $Image.version
  $Work=Join-Path $Root 'build'
  if(-not $Output){$Output=Join-Path $Root 'Game'}
  function Checked([string]$What){if($LASTEXITCODE -ne 0){throw "$What failed: $LASTEXITCODE"}}
  function CopyTree([string]$From,[string]$To){& robocopy $From $To /E /NFL /NDL /NJH /NJS /NP | Out-Null; if($LASTEXITCODE -ge 8){throw "Copy failed: $From"};$global:LASTEXITCODE=0}
  $RevisionProfile = $Image
  foreach($InputCheck in @(@($BaseDump,$RevisionProfile.base_rpx_sha256),@($UpdateDump,$RevisionProfile.rpx_sha256))){
   $InputPath=$InputCheck[0]
   foreach($Directory in @('code','content','meta')){if(-not (Test-Path -LiteralPath (Join-Path $InputPath $Directory) -PathType Container)){throw "Missing $Directory in $InputPath"}}
   if((Get-FileHash -LiteralPath (Join-Path $InputPath 'code\Gambit.rpx') -Algorithm SHA256).Hash -ne $InputCheck[1]){throw "RPX revision mismatch: $InputPath"}
  }
  [xml]$UpdateMeta=Get-Content -LiteralPath (Join-Path $UpdateDump 'meta\meta.xml') -Raw
  if($UpdateMeta.SelectSingleNode('/menu/title_version').InnerText -ne $Version){throw "Expected title version $Version"}
  $VsWhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  # Same Visual Studio range and generator as Setup.ps1 (2022 or 2026).
  $Vs=(& $VsWhere -version '[17.0,19.0)' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)[0]
  if(-not $Vs){throw 'Visual Studio 2022 or 2026 C++ tools required'}
  $Generator=switch([int]$Vs.installationVersion.Split('.')[0]){17{'Visual Studio 17 2022'}18{'Visual Studio 18 2026'}default{throw "Unsupported Visual Studio $($Vs.installationVersion)"}}
  $VcVars=Join-Path $Vs.installationPath 'VC\Auxiliary\Build\vcvars64.bat'
  # As in Setup.ps1: a broken PATH entry (stray quote, trailing ' /M') makes vcvars64.bat abort.
  $env:PATH=($env:PATH -split ';' | ForEach-Object { ($_ -replace '"','' -replace '\s+/[Mm]\s*$','').Trim() } | Where-Object { $_ } | Select-Object -Unique) -join ';'
  $EnvLines=cmd.exe /c "call `"$VcVars`" >nul 2>nul & set"
  foreach($Line in $EnvLines){if($Line -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
  if(-not(Get-Command cl.exe -ErrorAction SilentlyContinue)){throw 'The Visual Studio C++ environment could not be loaded (cl.exe not found).'}
  $Cmake=Join-Path $Vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  if(-not(Test-Path -LiteralPath $Cmake)){$Cmake=(Get-Command cmake -ErrorAction Stop).Source}
  $Ninja=Join-Path $Vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
  if(-not(Test-Path -LiteralPath $Ninja)){$Ninja=(Get-Command ninja -ErrorAction Stop).Source}
  $Toolchain=Join-Path $DependencyRoot 'build\vcpkg\scripts\buildsystems\vcpkg.cmake'
  $Sdl=Join-Path $DependencyRoot 'build\deps\sdl2-vulkan\cmake'
  $Glslang=Join-Path $DependencyRoot 'build\deps\glslang-vulkan\lib\cmake\glslang'
  foreach($Required in @($Toolchain,$Sdl,$Glslang)){if(-not(Test-Path -LiteralPath $Required)){throw "Missing prepared dependency: $Required"}}
  $env:VCPKG_DOWNLOADS=Join-Path $env:TEMP 'SplatoonPortDownloads'
  Write-Output '[1/5] Build revision-aware recompiler'
  $RecompilerBuild=Join-Path $Work 'rebrewu'
  & $Cmake -S (Join-Path $Root 'recompiler') -B $RecompilerBuild -G $Generator -A x64 "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" -DREBREWU_BUILD_TESTS=OFF
  Checked 'Configure recompiler'
  & $Cmake --build $RecompilerBuild --config Release --target rebrewu --parallel -- /p:TrackFileAccess=false
  Checked 'Build recompiler'
  Write-Output '[2/5] Translate update RPX with revision-specific candidates'
  $Generated=Join-Path $Work "generated-v$Version"
  $RecompilerExe=Join-Path $RecompilerBuild 'bin\Release\rebrewu.exe'
  # Imports, entries and hooks of the image all come from its manifest.
  $Manifest=Join-Path $Root (Join-Path 'Manifest' (Split-Path -Leaf $Image.update_manifest))
  if(-not(Test-Path -LiteralPath $Manifest -PathType Leaf)){throw "Missing manifest: $Manifest"}
  & $RecompilerExe recompile --rpx (Join-Path $UpdateDump 'code\Gambit.rpx') --output $Generated --manifest $Manifest --discover-referenced-entries -v
  Checked 'Translate update RPX'
  Write-Output '[3/5] Merge owned content into private staging folder'
  $Merged=Join-Path $Work "merged-v$Version"
  CopyTree $BaseDump $Merged
  CopyTree $UpdateDump $Merged
  Write-Output '[4/5] Build experimental update runtime'
  $GameBuild=Join-Path $Work $(if($Optimize){"game-v$Version-release"}else{"game-v$Version-diagnostic"})
  $FastGuest=if($Optimize){'OFF'}else{'ON'}
  $DiagnosticRuntime=if($Optimize -and -not $Diagnostics){'OFF'}else{'ON'}
  & $Cmake -S (Join-Path $Root 'recompiler\port') -B $GameBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" "-DGAMBIT_GENERATED_DIR=$Generated" "-DRBREW_MANIFEST=$Manifest" "-DSDL2_DIR=$Sdl" "-Dglslang_DIR=$Glslang" -DRBREW_GAME_REVISION=$Version -DRBREW_GRAPHICS_BACKEND=Vulkan -DRBREW_BUILD_GAME=ON "-DRBREW_DIAGNOSTIC_RUNTIME=$DiagnosticRuntime" "-DRBREW_FAST_DIAGNOSTIC_GUEST=$FastGuest" -DRBREW_ENABLE_PPC_FALLBACK=OFF
  Checked 'Configure update game'
  & $Cmake --build $GameBuild --target Gambit -- -j ([Environment]::ProcessorCount)
  Checked 'Build update game'
  Write-Output '[5/5] Package isolated test installation'
  New-Item -ItemType Directory -Path $Output -Force | Out-Null
  $SetupAst=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $Root 'scripts\Setup.ps1'),[ref]$null,[ref]$null)
  foreach($Name in @('Ensure-Folder','Initialize-WiiUFolders')){
   $FunctionAst=$SetupAst.Find({param($Node) $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -eq $Name},$true)
   if(-not $FunctionAst){throw "Setup.ps1: function $Name not found (cannot prepare the install)"}
   . ([scriptblock]::Create($FunctionAst.Extent.Text))
  }
  Initialize-WiiUFolders $Output
  CopyTree (Join-Path $Merged 'content') (Join-Path $Output 'content')
  CopyTree (Join-Path $Merged 'meta') (Join-Path $Output 'meta')
  # The prepared dependencies usually live in this same checkout (DependencyRoot = repository root),
  # so source and destination can be the same folder: there is nothing to copy then.
  $DependencyGame=Join-Path $DependencyRoot 'Game'
  # With a custom Setup output folder there is no <DependencyRoot>\Game: Setup already put the
  # system titles and runtime DLLs into that folder, so there is nothing to copy either.
  $CopyDependencyGame=(Test-Path -LiteralPath $DependencyGame -PathType Container) -and -not ((Resolve-Path -LiteralPath $DependencyGame).Path.TrimEnd('\') -eq (Resolve-Path -LiteralPath $Output).Path.TrimEnd('\'))
  $DependencyTitles=Join-Path $DependencyGame 'mlc01\sys\title'
  if($CopyDependencyGame -and (Test-Path -LiteralPath $DependencyTitles -PathType Container)){CopyTree $DependencyTitles (Join-Path $Output 'mlc01\sys\title')}
  Copy-Item -LiteralPath (Join-Path $GameBuild 'Splatoon.exe') -Destination (Join-Path $Output "Splatoon-v$Version.exe") -Force
  Get-ChildItem -LiteralPath $GameBuild -Filter '*.dll' | Copy-Item -Destination $Output -Force
  if($CopyDependencyGame){Get-ChildItem -LiteralPath $DependencyGame -Filter '*.dll' | Copy-Item -Destination $Output -Force}
  $ZlibDll=Join-Path $Output 'z.dll'
  if(-not(Test-Path -LiteralPath $ZlibDll)){
   $ZlibSource=Join-Path $DependencyRoot 'build\vcpkg\installed\x64-windows\bin\z.dll'
   if(-not(Test-Path -LiteralPath $ZlibSource)){throw "Missing required zlib runtime: $ZlibSource"}
   Copy-Item -LiteralPath $ZlibSource -Destination $ZlibDll
  }
  Write-Output "EXPERIMENTAL BUILD COMPLETE: $Output\Splatoon-v$Version.exe"
}

if ($CheckDependencies) {
  $deps = Resolve-DependencyRoot
  Write-Host "Dependencies OK: $deps" -ForegroundColor Green
  exit 0
}

Write-Host 'Splatoon PC port - game update'
Write-Host 'This keeps your existing installation and only builds the updated game.'
Write-Host ''

$gameDir = Get-GameRoot $GameRoot
Test-BaseInstall $gameDir
if (-not $UpdateDump -and $env:SPLATOON_UPDATE_DUMP) { $UpdateDump = $env:SPLATOON_UPDATE_DUMP }
if (-not $UpdateDump) {
  $UpdateDump = Ask-Folder 'Select the UPDATE dump (contains code, content and meta).'
}
if (-not $UpdateDump) {
  $UpdateDump = Read-DumpPath 'Select the UPDATE dump (folders code, content, meta).'
}
$Profile = Test-UpdateDump $UpdateDump

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

$buildRoot = $Root
$deps = Resolve-DependencyRoot
# As in Setup.ps1: the compiler cannot open include paths with non-ASCII letters, so build
# through a drive letter mapped to the same folder (subst), removed again afterwards.
$buildDrive = $null
if ($Root -match '[^\x00-\x7F]') {
  $mapped = subst | Where-Object { $_ -match '^([A-Z]):\\: => (.*)$' -and $matches[2].TrimEnd('\') -ieq $Root.TrimEnd('\') } |
    ForEach-Object { $_.Substring(0, 2) } | Select-Object -First 1
  if (-not $mapped) {
    $free = @('S','T','U','V','W','X','Y','Z','R','Q','P') | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
    if ($free) { subst "${free}:" "$Root" 2>$null | Out-Null; if (Test-Path -LiteralPath "${free}:\scripts\Install-Update.ps1") { $mapped = "${free}:"; $buildDrive = $mapped } }
  }
  if (-not $mapped) { Fail "Your folder path contains special characters ($Root) and no free drive letter was found. Move the project to a folder like C:\SplatoonPort." }
  $buildRoot = "$mapped\"
  if ($deps.TrimEnd('\') -ieq $Root.TrimEnd('\')) { $deps = "$mapped\" }
}
Write-Host ''
Write-Host 'Starting the update build. This takes 20-60 minutes.' -ForegroundColor Cyan
try {
  Invoke-UpdateBuild -Root $buildRoot -BaseDump $BaseDump -UpdateDump $UpdateDump -DependencyRoot $deps -Output $gameDir -Optimize $true -Image $Profile
} catch {
  Fail "Update build failed: $($_.Exception.Message)"
} finally {
  if ($buildDrive) { subst "$buildDrive" /D 2>$null | Out-Null }
}
Write-Host ''
Write-Host "Update installed. Start the game with $(Join-Path $gameDir ('Splatoon-v' + $Profile.version + '.exe'))." -ForegroundColor Green

$shortcutPath = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Splatoon PC Port.lnk'
if (Test-Path -LiteralPath $shortcutPath -PathType Leaf) {
  $gameDir = Get-GameRoot $GameRoot
  $updateExe = Join-Path $gameDir ('Splatoon-v' + $Profile.version + '.exe')
  if (Test-Path -LiteralPath $updateExe -PathType Leaf) {
    try {
      $shell = New-Object -ComObject WScript.Shell
      $shortcut = $shell.CreateShortcut($shortcutPath)
      $shortcut.TargetPath = $updateExe
      $shortcut.WorkingDirectory = $gameDir
      $shortcut.IconLocation = "$updateExe,0"
      $shortcut.Save()
      Write-Host "Desktop shortcut updated to Splatoon-v$($Profile.version).exe." -ForegroundColor Green
    } catch {
      Write-Host "The desktop shortcut could not be updated: $($_.Exception.Message)" -ForegroundColor Yellow
    }
  }
}
