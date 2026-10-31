# Splatoon 1 PC Port - one-click setup.
#
# Builds the recompiler, recompiles YOUR OWN copy of the game into a native
# Windows program, and creates a ready-to-play game folder. Nothing from
# Nintendo is downloaded or shipped by this project: the game files always come
# from the dump you select. Run it through Setup.bat (double-click).
#
# Re-running is safe: finished steps are skipped, your save and shader cache are
# kept. Use -Clean to rebuild everything from scratch.

param(
    [string]$GameDump,      # folder with code\, content\, meta\ from your Wii U dump
    [string]$Mlc,           # optional local system-resource source; never prompts
    [string]$Output,        # where the playable game folder is created
    [switch]$Clean,         # delete previous build outputs first
    [switch]$NonInteractive # unattended execution without interactive prompts
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------------------
# Fixed inputs
# ---------------------------------------------------------------------------
$Root = Split-Path -Parent $PSScriptRoot
$Recompiler = Join-Path $Root 'recompiler'
$Work = Join-Path $Root 'build'
$Downloads = Join-Path $Work 'downloads'

# Every Manifest\*.manifest describes one supported image; the RPX selects it.
$ManifestDir = Join-Path $Root 'Manifest'
$MinimumFunctionCount = 96300

# Open-source build dependencies (official upstream releases).
$SdlUrl = 'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.zip'
$GlslangUrl = 'https://github.com/KhronosGroup/glslang/archive/refs/tags/16.4.0.tar.gz'
$VcpkgUrl = 'https://github.com/microsoft/vcpkg.git'
$VcpkgPackages = @('zlib', 'curl', 'volk', 'vulkan-headers')

# Cemu's shared fonts are Noto Sans replacements under SIL OFL 1.1,
# not Nintendo's fonts. Pin the exact upstream bytes for unattended setup.
$FontBaseUrl = 'https://raw.githubusercontent.com/cemu-project/Cemu/cc6fea125a5d4f559c662d13c5f4af0b6dd0876f/bin/resources/sharedFonts'
$FallbackFonts = @(
    @{ Name = 'CafeCn.ttf'; Sha256 = '6AFD751A9BB76BC788EFB50A138C6F3765624EE84D8D8203BE04D871ADE40A6A' },
    @{ Name = 'CafeKr.ttf'; Sha256 = 'C3C326F6CE1899E391DC4996B10A0C30558C5B8F36DFBAA418976862B3E2DFFB' },
    @{ Name = 'CafeStd.ttf'; Sha256 = '59E2494727B8E7A7BBF48C8749F2DBC07D438C9FABEAA9847A08EC690F9888A1' },
    @{ Name = 'CafeTw.ttf'; Sha256 = 'C8F9E16345DBC3B6E165FE12435486A748E730EBE6002B5D713EFE73D171D51C' }
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
$script:StepNumber = 0
$script:StageTimings = [ordered]@{}
$script:TotalWatch = [System.Diagnostics.Stopwatch]::StartNew()

function Time-Stage([string]$Name, [scriptblock]$Block) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        . $Block
    } finally {
        $script:StageTimings[$Name] = [math]::Round($sw.Elapsed.TotalSeconds, 2)
    }
}
function Step([string]$Text) {
    $script:StepNumber++
    Write-Host ''
    Write-Host ("[{0}/8] {1}" -f $script:StepNumber, $Text) -ForegroundColor Cyan
}
function Info([string]$Text) { Write-Host "      $Text" }
function Good([string]$Text) { Write-Host "      $Text" -ForegroundColor Green }
function Warn([string]$Text) { Write-Host "      $Text" -ForegroundColor Yellow }
function Fail([string]$Text) {
    Write-Host ''
    Write-Host "SETUP FAILED: $Text" -ForegroundColor Red
    Write-Host 'If you cannot fix this, open an issue on GitHub and paste this window''s text.' -ForegroundColor Red
    if (Get-Command Remove-BuildDrive -ErrorAction SilentlyContinue) { Remove-BuildDrive }
    exit 1
}
function Check-Exit([string]$What) {
    if ($LASTEXITCODE -ne 0) { Fail "$What (exit code $LASTEXITCODE)." }
}
function Ask-YesNo([string]$Question) {
    if ($NonInteractive) { return $false }
    $answer = Read-Host "      $Question [Y/N]"
    return $answer -match '^(y|yes|s|sim)$'
}
function Full-FolderPath([string]$Path) {
    $Path = $Path.Trim().Trim('"')
    if (-not $Path) { return '' }
    try { return [IO.Path]::GetFullPath($Path) }
    catch { Fail "Invalid folder path: $Path. $($_.Exception.Message)" }
}
function Ensure-Folder([string]$Path) {
    [void][IO.Directory]::CreateDirectory($Path)
}
function Initialize-WiiUFolders([string]$GameFolder) {
    # Create an empty MLC layout, like Cemu's first-run setup. The native
    # nn_save provider uses save/ for the current account and save/common/.
    # Directory creation preserves existing progress on subsequent installs.
    $folders = @(
        'save\common',
        'mlc01\sys',
        'mlc01\usr\title\00050000',
        'mlc01\usr\title\0005000c',
        'mlc01\usr\title\0005000e',
        'mlc01\usr\save\00050000',
        'mlc01\usr\boss',
        'mlc01\usr\save\00050010\1004a000\user\common\db',
        'mlc01\usr\save\00050010\1004a100\user\common\db',
        'mlc01\usr\save\00050010\1004a200\user\common\db',
        'mlc01\sys\title\0005001b\10042400\content',
        'mlc01\sys\title\0005001b\10056000\content',
        'mlc01\sys\title\0005001b\1005c000\content'
    )
    foreach ($folder in $folders) { Ensure-Folder (Join-Path $GameFolder $folder) }
}
function Ask-Folder([string]$Description, [string]$Default) {
    $dialog = $null
    $owner = $null
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = $Description
        $dialog.ShowNewFolderButton = $false
        if ($Default -and (Test-Path -LiteralPath $Default -PathType Container)) { $dialog.SelectedPath = $Default }
        $owner = New-Object System.Windows.Forms.Form
        $owner.TopMost = $true
        if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
    } catch {
        Warn "The folder picker could not open: $($_.Exception.Message)"
    } finally {
        if ($dialog) { $dialog.Dispose() }
        if ($owner) { $owner.Dispose() }
    }
    return (Read-Host "      $Description (type or paste the full path)")
}
function File-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToUpperInvariant()
}
function Select-Manifest([string]$RpxPath) {
    $resolver = Join-Path $Recompiler 'tools\plt_resolver.py'
    $output = & $pyExe @pyArgs $resolver select --rpx $RpxPath --manifest-dir $ManifestDir
    if ($LASTEXITCODE -ne 0) { return $null }
    $line = $output | Where-Object { $_ -and $_.Trim() } | Select-Object -Last 1
    if (-not $line) { return $null }
    return ([string]$line).Trim()
}
function Complete-GeneratedModule([string]$Directory) {
    foreach ($name in @('Gambit.h', 'Gambit_data.cpp', 'Gambit_register.cpp')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory $name) -PathType Leaf)) { return $false }
    }
    $registered = @(Select-String -LiteralPath (Join-Path $Directory 'Gambit_register.cpp') -Pattern '^\s+rbrew_register_func\(').Count
    return $registered -ge $MinimumFunctionCount
}
function Download([string]$Url, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) { return }
    Info "Downloading $(Split-Path -Leaf $Destination) ..."
    $partial = "$Destination.part"
    (New-Object Net.WebClient).DownloadFile($Url, $partial)
    Move-Item -LiteralPath $partial -Destination $Destination -Force
}
function Copy-Tree([string]$From, [string]$To) {
    & robocopy $From $To /E /NFL /NDL /NJH /NJS /NP /MT:8 | Out-Null
    if ($LASTEXITCODE -ge 8) { Fail "Copying $From failed (robocopy code $LASTEXITCODE)." }
    $global:LASTEXITCODE = 0
}
function System-ResourceRoots([string]$Dump, [string]$Destination, [string]$Source) {
    $roots = New-Object 'System.Collections.Generic.List[string]'
    $roots.Add((Join-Path $Destination 'mlc01'))
    foreach ($path in @($Source, $env:RBREW_SYSTEM_MLC_DIR, $env:RBREW_MLC_DIR)) {
        if ($path) { $roots.Add((Full-FolderPath $path)) }
    }
    # Locate an MLC or dumped wiiu-system/mlc beside the game/repository.
    # No folder names belonging to this developer's PC are assumed.
    foreach ($start in @($Dump, $Root)) {
        $parent = [IO.DirectoryInfo]::new($start)
        for ($depth = 0; $parent -and $depth -lt 5; $depth++) {
            $roots.Add($parent.FullName)
            $parent = $parent.Parent
        }
    }
    foreach ($base in @((Join-Path $env:APPDATA 'Cemu'), (Join-Path $env:LOCALAPPDATA 'Cemu'))) {
        $roots.Add($base)
        $settings = Join-Path $base 'settings.xml'
        if (-not (Test-Path -LiteralPath $settings -PathType Leaf)) { continue }
        $reader = $null
        try {
            $options = New-Object System.Xml.XmlReaderSettings
            $options.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
            $options.XmlResolver = $null
            $reader = [System.Xml.XmlReader]::Create($settings, $options)
            $document = New-Object System.Xml.XmlDocument
            $document.XmlResolver = $null
            $document.Load($reader)
            $node = $document.SelectSingleNode('/content/mlc_path')
            if ($node -and $node.InnerText.Trim()) {
                $configured = $node.InnerText.Trim()
                if (-not [IO.Path]::IsPathRooted($configured)) { $configured = Join-Path $base $configured }
                $roots.Add($configured)
            }
        } catch {
            Warn "Could not read Cemu's custom MLC setting; continuing automatic setup."
        } finally {
            if ($reader) { $reader.Dispose() }
        }
    }
    return $roots | Select-Object -Unique
}
function Find-SystemResourceFiles([string[]]$Roots, [string]$Relative, [string[]]$Names) {
    foreach ($candidate in $Roots) {
        foreach ($suffix in @('', 'mlc01', 'mlc', 'wiiu-system\mlc')) {
            $base = $candidate
            if ($suffix) { $base = Join-Path $base $suffix }
            $directory = Join-Path $base $Relative
            $complete = $true
            foreach ($name in $Names) {
                $file = Join-Path $directory $name
                if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
                    (Get-Item -LiteralPath $file).Length -eq 0) { $complete = $false; break }
            }
            if ($complete) { return [IO.Path]::GetFullPath($directory) }
        }
    }
    return $null
}
function Copy-ResourceFiles([string]$From, [string]$To, [string[]]$Names) {
    Ensure-Folder $To
    foreach ($name in $Names) {
        $sourceFile = [IO.Path]::GetFullPath((Join-Path $From $name))
        $targetFile = [IO.Path]::GetFullPath((Join-Path $To $name))
        if (-not [string]::Equals($sourceFile, $targetFile, [StringComparison]::OrdinalIgnoreCase)) {
            Copy-Item -LiteralPath $sourceFile -Destination $targetFile -Force
        }
    }
}
function Prepare-WiiUResources([string]$Dump, [string]$Destination, [string]$Source) {
    Initialize-WiiUFolders $Destination
    $roots = @(System-ResourceRoots $Dump $Destination $Source)
    $fontRelative = 'sys\title\0005001b\10042400\content'
    $fontNames = @($FallbackFonts | ForEach-Object { $_.Name })
    $fontDestination = Join-Path $Destination ('mlc01\' + $fontRelative)
    $fontSource = Find-SystemResourceFiles $roots $fontRelative $fontNames
    if ($fontSource) {
        Copy-ResourceFiles $fontSource $fontDestination $fontNames
        Good "Shared fonts ready: $fontSource"
    } else {
        Info 'Installing the same Noto replacement fonts supplied with Cemu ...'
        $fontCache = Join-Path $Downloads 'shared-fonts'
        Ensure-Folder $fontCache
        foreach ($font in $FallbackFonts) {
            $file = Join-Path $fontCache $font.Name
            Download ($FontBaseUrl + '/' + $font.Name) $file
            if ((File-Sha256 $file) -ne $font.Sha256) {
                Remove-Item -LiteralPath $file -Force
                Fail "The replacement font $($font.Name) failed its integrity check. Run Setup.bat again to retry."
            }
        }
        Copy-ResourceFiles $fontCache $fontDestination $fontNames
        $licenses = Join-Path $Destination 'licenses'
        Ensure-Folder $licenses
        Copy-Item -LiteralPath (Join-Path $Root 'LICENSES\cemu-shared-fonts-OFL-1.1.txt') -Destination $licenses -Force
        Good 'Noto replacement fonts installed.'
    }
    # Cemu creates these directories without requiring a system-title dump.
    # Copy real Mii graphics if available; never invent resource/save data.
    $miiRelative = 'sys\title\0005001b\10056000\content'
    $miiNames = @('FFLResHigh.dat', 'FFLResHighLG.dat', 'FFLResMiddle.dat', 'FFLResMiddleLG.dat')
    $miiSource = Find-SystemResourceFiles $roots $miiRelative $miiNames
    if ($miiSource) {
        Copy-ResourceFiles $miiSource (Join-Path $Destination ('mlc01\' + $miiRelative)) $miiNames
        Good "Local Mii graphics found automatically: $miiSource"
    } else {
        Warn 'No local Mii graphics resources found; some Mii graphics may be unavailable.'
    }
    Good 'Wii U folders initialized automatically; no saves or accounts imported.'
}

# ---------------------------------------------------------------------------
Write-Host '============================================================' -ForegroundColor Magenta
Write-Host '   Splatoon 1 PC Port - setup' -ForegroundColor Magenta
Write-Host '   Uses only YOUR OWN game files. Keep this window open.' -ForegroundColor Magenta
Write-Host '============================================================' -ForegroundColor Magenta

if (-not [Environment]::Is64BitOperatingSystem) { Fail 'A 64-bit Windows 10 or 11 is required.' }

# A folder path with non-ASCII letters (a user name with an accent) reaches
# the compiler mangled: Ninja starts cl.exe with an ANSI command line, so
# every include path points nowhere ("Cannot open include file", the accent
# turned into two other characters), issue #10. A junction does not help
# (CMake resolves it back). Build through a drive letter mapped to this same
# folder (subst) instead: nothing is moved or copied, and the letter is
# removed again when setup ends. The game folder keeps the real path.
$ProjectRoot = $Root
$script:BuildDrive = $null
function Remove-BuildDrive {
    if ($script:BuildDrive) { subst "$($script:BuildDrive)" /D 2>$null | Out-Null; $script:BuildDrive = $null }
}
if ($Root -match '[^\x00-\x7F]') {
    # The letter this folder already has from an earlier run, else a free one.
    $mapped = subst | Where-Object { $_ -match '^([A-Z]):\\: => (.*)$' -and $matches[2].TrimEnd('\') -ieq $Root.TrimEnd('\') } |
        ForEach-Object { $_.Substring(0, 2) } | Select-Object -First 1
    if (-not $mapped) {
        $free = @('S','T','U','V','W','X','Y','Z','R','Q','P') | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
        if ($free) { subst "${free}:" "$Root" 2>$null | Out-Null; if (Test-Path -LiteralPath "${free}:\scripts\Setup.ps1") { $mapped = "${free}:"; $script:BuildDrive = $mapped } }
    }
    if (-not $mapped) {
        Fail "Your folder path contains special characters ($Root), which the C++ compiler cannot handle, and no free drive letter was found to work around it. Move the project to a folder like C:\SplatoonPort and run Setup.bat again."
    }
    Info "Your folder path contains special characters; the build runs through drive $mapped (the same folder)."
    $Root = "$mapped\"
    $Recompiler = Join-Path $Root 'recompiler'
    $Work = Join-Path $Root 'build'
    $Downloads = Join-Path $Work 'downloads'
}
# A build folder configured under another path (the folder was moved or
# renamed, or the drive above is new): CMake refuses the old cache. Reset that
# configuration (only inside this project's build folder).
function Reset-StaleCMakeCache([string]$BuildDir) {
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache)) { return }
    $line = Select-String -LiteralPath $cache -Pattern '^CMAKE_CACHEFILE_DIR:INTERNAL=(.*)$' | Select-Object -First 1
    $want = ($BuildDir -replace '\\', '/').TrimEnd('/')
    if ($line -and $line.Matches[0].Groups[1].Value.TrimEnd('/') -ne $want) {
        $files = [IO.Path]::GetFullPath((Join-Path $BuildDir 'CMakeFiles'))
        $inside = [IO.Path]::GetFullPath($Work).TrimEnd('\') + '\'
        if (-not $files.StartsWith($inside, [StringComparison]::OrdinalIgnoreCase)) { return }
        Info "Resetting the build configuration in $BuildDir (it was made for another folder path) ..."
        Remove-Item -LiteralPath $cache -Force
        if (Test-Path -LiteralPath $files) { Remove-Item -LiteralPath $files -Recurse -Force }
    }
}
if ($Clean -and (Test-Path -LiteralPath $Work)) {
    Info 'Removing previous build outputs (-Clean) ...'
    Remove-Item -LiteralPath $Work -Recurse -Force
}
Ensure-Folder $Downloads

# ---------------------------------------------------------------------------
Step 'Checking the tools needed to build'
$script:StepSw = [System.Diagnostics.Stopwatch]::StartNew()
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstaller = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
$vs = $null
$existingVs2022 = $null

if (Test-Path -LiteralPath $vswhere) {
    # 1. Check for supported Visual Studio (v17+ / VS 2022+) with VC C++ Tools already present
    $found = & $vswhere -version '[17.0,19.0)' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
    if ($found -and @($found).Count -gt 0) {
        $vs = @($found)[0]
    } else {
        # 2. Check if VS 2022 is installed but missing the C++ workload
        $foundVs2022 = & $vswhere -version '[17.0,19.0)' -latest -products * -format json | ConvertFrom-Json
        if ($foundVs2022 -and @($foundVs2022).Count -gt 0) {
            $existingVs2022 = @($foundVs2022)[0]
            Warn "Found $($existingVs2022.displayName) ($($existingVs2022.installationVersion)), but the C++ Build Tools workload is missing."
        } else {
            # Check if an older version (e.g. VS 2019) is installed
            $foundAny = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
            if ($foundAny -and @($foundAny).Count -gt 0) {
                $oldVs = @($foundAny)[0]
                Warn "Found $($oldVs.displayName) (version $($oldVs.installationVersion)), but Visual Studio 2022 (v17) or newer is required."
            }
        }
    }
}
$python = $null
foreach ($candidate in @(@('py', '-3'), @('python'), @('python3'))) {
    if (-not (Get-Command $candidate[0] -ErrorAction SilentlyContinue)) { continue }
    $extra = @(); if ($candidate.Count -gt 1) { $extra = $candidate[1..($candidate.Count - 1)] }
    $ok = $null
    try {
        $ErrorActionPreference = 'Continue'
        $ok = & $candidate[0] @extra -c "import sys; print(int(sys.version_info >= (3, 12)))" 2>$null
    } catch { $ok = $null } finally { $ErrorActionPreference = 'Stop' }
    if ("$ok".Trim() -eq '1') { $python = $candidate; break }
}
$git = Get-Command git -ErrorAction SilentlyContinue

$cmake = $null
if ($vs) {
    $vsCmake = Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $vsCmake) { $cmake = $vsCmake }
}
if (-not $cmake) {
    $cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCmd) { $cmake = $cmakeCmd.Source }
}

$missing = @()
if (-not $vs) {
    if ($existingVs2022 -and (Test-Path -LiteralPath $vsInstaller)) {
        $missing += @{
            Name = 'C++ workload & CMake for Visual Studio 2022';
            Type = 'VsModify';
            InstallPath = $existingVs2022.installationPath
        }
    } else {
        $missing += @{
            Name = 'Visual Studio 2022 Build Tools (C++ workload & CMake)';
            Type = 'Winget';
            Id = 'Microsoft.VisualStudio.2022.BuildTools';
            Extra = '--override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended"'
        }
    }
} elseif (-not $cmake) {
    $missing += @{
        Name = 'CMake';
        Type = 'Winget';
        Id = 'Kitware.CMake';
        Extra = ''
    }
}
if (-not $python) {
    $missing += @{
        Name = 'Python 3.12';
        Type = 'Winget';
        Id = 'Python.Python.3.12';
        Extra = ''
    }
}
if (-not $git) {
    $missing += @{
        Name = 'Git';
        Type = 'Winget';
        Id = 'Git.Git';
        Extra = ''
    }
}
if ($missing.Count) {
    Warn 'The following tools or workloads need to be installed/configured:'
    foreach ($m in $missing) { Warn "  - $($m.Name)" }
    if (Ask-YesNo 'Install / configure them now automatically?') {
        foreach ($m in $missing) {
            Info "Configuring $($m.Name) ..."
            if ($m.Type -eq 'VsModify') {
                $modifyArgs = "modify --installPath `"$($m.InstallPath)`" --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended --passive --norestart"
                Info "Launching Visual Studio Installer to add C++ build tools and CMake..."
                Info "Please accept the Windows Administrator (UAC) prompt if it appears."
                $proc = Start-Process -FilePath $vsInstaller -ArgumentList $modifyArgs -Verb RunAs -Wait -PassThru
                if ($proc.ExitCode -ne 0 -and $proc.ExitCode -ne 3010) {
                    Fail "Visual Studio Installer exited with code $($proc.ExitCode). You can also open the 'Visual Studio Installer' app manually, click Modify on VS 2022, and check 'Desktop development with C++'."
                }
            } else {
                if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
                    Fail "winget was not found. Please install $($m.Name) manually."
                }
                $wingetArgs = "install --id $($m.Id) -e --accept-package-agreements --accept-source-agreements $($m.Extra)"
                $proc = Start-Process winget -ArgumentList $wingetArgs -Wait -PassThru -NoNewWindow
                if ($proc.ExitCode -ne 0) {
                    Warn "$($m.Name) installation exited with code $($proc.ExitCode). Retrying with administrator privileges..."
                    $procAdmin = Start-Process winget -ArgumentList $wingetArgs -Wait -PassThru -Verb RunAs
                    if ($procAdmin.ExitCode -ne 0) {
                        Fail "Failed to install $($m.Name) (exit code $($procAdmin.ExitCode)). Please install it manually."
                    }
                }
            }
        }
        Good 'Done! All required tools and workloads have been installed.'
        Good 'Please close this window and run Setup.bat again so Windows picks up the new programs.'
        exit 0
    }
    Fail 'Install the programs listed above (see README), then run Setup.bat again.'
}
$vsMajor = [int]($vs.installationVersion.Split('.')[0])
switch ($vsMajor) {
    17 { $generator = 'Visual Studio 17 2022' }
    18 { $generator = 'Visual Studio 18 2026' }
    default { Fail "Visual Studio version $vsMajor is not supported. Install Visual Studio 2022 (or newer) Build Tools." }
}
if (-not $cmake) {
    $vsCmake = Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $vsCmake) {
        $cmake = $vsCmake
    } else {
        $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
        if (-not $cmakeCommand) { Fail 'CMake was not found. Re-run the Visual Studio Installer and add "C++ CMake tools for Windows".' }
        $cmake = $cmakeCommand.Source
    }
}
$ninja = $null
$vsNinja = Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (Test-Path -LiteralPath $vsNinja) {
    $ninja = $vsNinja
} else {
    $ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninjaCmd) { $ninja = $ninjaCmd.Source }
}
if ($ninja -and $vs) {
    $vcvars = Join-Path $vs.installationPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $vcvars) {
        # A broken PATH entry (a stray quote, or a leftover ' /M' from a bad
        # setx call by some installer) makes vcvars64.bat abort with
        # "... was unexpected at this time", and the compiler is never set up.
        # Clean this process's PATH copy first; the system PATH is not changed.
        $cleanPath = ($env:PATH -split ';' | ForEach-Object { ($_ -replace '"', '' -replace '\s+/[Mm]\s*$', '').Trim() } |
            Where-Object { $_ } | Select-Object -Unique) -join ';'
        $env:PATH = $cleanPath
        $envLines = cmd.exe /c "call `"$vcvars`" >nul & set"
        foreach ($line in $envLines) {
            if ($line -match '^([^=]+)=(.*)$') {
                [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
            }
        }
        if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
            Fail "Visual Studio's C++ environment could not be loaded (cl.exe not found). Open 'x64 Native Tools Command Prompt for VS 2022': if it shows an error, check your PATH environment variable for broken entries (stray quotes), or repair the 'Desktop development with C++' workload in the Visual Studio Installer."
        }
    }
}
$pyExe = $python[0]; $pyArgs = @(); if ($python.Count -gt 1) { $pyArgs = $python[1..($python.Count - 1)] }
Good "Visual Studio $($vs.installationVersion), $generator"
Good "Python: $(& $pyExe @pyArgs --version)"
Good "Git: $((& git --version))"
$script:StageTimings['1. Toolchain & Environment Check'] = [math]::Round($script:StepSw.Elapsed.TotalSeconds, 2)

# ---------------------------------------------------------------------------
Step 'Finding your game files'
$script:StepSw = [System.Diagnostics.Stopwatch]::StartNew()
if (-not $GameDump) {
    # Check known/common locations automatically before prompting
    $candidateDumps = @()
    $downloads = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Downloads'
    if (Test-Path -LiteralPath $downloads -PathType Container) {
        $candidateDumps += @(Get-ChildItem -LiteralPath $downloads -Directory |
            ForEach-Object { $_.FullName })
    }
    $candidateDumps += @((Join-Path $Root 'dump'), (Join-Path $Root 'game-dump'))
    foreach ($candidate in $candidateDumps) {
        $candidateRpx = Join-Path $candidate 'code\Gambit.rpx'
        if ((Test-Path -LiteralPath $candidateRpx -PathType Leaf) -and
            (Select-Manifest $candidateRpx 2>$null)) {
            $GameDump = $candidate
            Good "Automatically detected valid game dump: $GameDump"
            break
        }
    }
}
if (-not $GameDump) {
    Info 'Select the folder of your Splatoon dump. It must contain the folders code, content and meta.'
    $GameDump = Ask-Folder 'Select your Splatoon game dump folder (contains code, content, meta)' $Root
}
# Folder names are arbitrary. Use literal paths throughout setup so names
# containing brackets (common in game dumps) are never treated as wildcards.
$GameDump = Full-FolderPath $GameDump
if (-not $GameDump) { Fail 'No game folder was selected.' }
if (-not (Test-Path -LiteralPath $GameDump -PathType Container)) { Fail "The game dump folder does not exist: $GameDump" }
foreach ($folder in @('code', 'content', 'meta')) {
    if (-not (Test-Path -LiteralPath (Join-Path $GameDump $folder) -PathType Container)) {
        Fail "Your dump is missing the $folder folder. Select the folder that contains code, content and meta; its name does not matter."
    }
}
$rpx = Join-Path $GameDump 'code\Gambit.rpx'
foreach ($required in @($rpx, (Join-Path $GameDump 'meta\meta.xml'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { Fail "Your dump is missing $required. Select the folder that contains code, content and meta." }
}
if (-not (Test-Path -LiteralPath $ManifestDir -PathType Container)) {
    Fail "The Manifest folder is missing: $ManifestDir. Extract the complete repository, including Manifest, and retry."
}
Info 'Checking the game version (this takes a few seconds) ...'
$rpxHash = File-Sha256 $rpx
$SelectedManifest = Select-Manifest $rpx
if (-not $SelectedManifest) {
    Fail "Unsupported game version. None of the manifests in $ManifestDir describes your code\Gambit.rpx (SHA-256 $rpxHash). Add a manifest for this image, or use a supported dump."
}
Good "Matched manifest: $SelectedManifest"

if (-not $Output) { $Output = Join-Path $ProjectRoot 'Game' }
$Output = Full-FolderPath $Output
if (-not $Output) { Fail 'No output folder was specified.' }
if (Test-Path -LiteralPath $Output -PathType Leaf) { Fail "The output path is a file, not a folder: $Output" }
Info "The game will be installed to: $Output"
Prepare-WiiUResources $GameDump $Output $Mlc
$script:StageTimings['2. Game Validation & Resources'] = [math]::Round($script:StepSw.Elapsed.TotalSeconds, 2)

# ---------------------------------------------------------------------------
Step 'Installing open-source libraries (vcpkg: zlib, curl, volk, Vulkan headers)'
$script:StepSw = [System.Diagnostics.Stopwatch]::StartNew()
$vcpkgRoot = Join-Path $Work 'vcpkg'
# vcpkg's own PowerShell bootstrap breaks when its path contains [ or ]
# (for example a download folder named "Splatoon [AGMP01]"): PowerShell reads
# them as wildcards ("A parameter cannot be found that matches parameter name
# 'Raw'"). vcpkg also cannot start itself from a path with non-ASCII letters
# (a user name with an accent): "Abnormal exit with child return code: no
# such file or directory" while detecting the compiler (issue #10). Use a
# plain folder then; LOCALAPPDATA contains the user name too.
if ($vcpkgRoot -match '[\[\]]|[^\x00-\x7F]' -or $ProjectRoot -match '[^\x00-\x7F]') {
    $vcpkgRoot = @((Join-Path $env:LOCALAPPDATA 'SplatoonPort\vcpkg'), (Join-Path $env:ProgramData 'SplatoonPort\vcpkg'),
                   (Join-Path "$env:SystemDrive\" 'SplatoonPort\vcpkg')) |
        Where-Object { $_ -notmatch '[\[\]]|[^\x00-\x7F]' } | Select-Object -First 1
    if (-not $vcpkgRoot) { Fail 'No folder without special characters was found for vcpkg. Move the project to a folder like C:\SplatoonPort and run Setup.bat again.' }
    Info "The project folder contains special characters; vcpkg goes to $vcpkgRoot"
}
$vcpkg = Join-Path $vcpkgRoot 'vcpkg.exe'
if (-not (Test-Path -LiteralPath $vcpkg)) {
    if (-not (Test-Path -LiteralPath (Join-Path $vcpkgRoot '.git'))) {
        & git clone --depth 1 $VcpkgUrl $vcpkgRoot; Check-Exit 'Downloading vcpkg'
    }
    # vcpkg's bootstrap downloads vcpkg.exe with its own small tool; a failed
    # name lookup ("The server name or address could not be resolved") or a
    # dropped connection stopped the whole setup. Retry, then download the
    # same release with Windows' web client (it follows the system proxy), and
    # accept it only with a valid Microsoft signature.
    for ($attempt = 1; $attempt -le 3 -and -not (Test-Path -LiteralPath $vcpkg); ++$attempt) {
        & (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
        if (-not (Test-Path -LiteralPath $vcpkg)) { Warn "Downloading vcpkg.exe failed (attempt $attempt of 3)."; Start-Sleep -Seconds (5 * $attempt) }
    }
    if (-not (Test-Path -LiteralPath $vcpkg)) {
        $metadata = Join-Path $vcpkgRoot 'scripts\vcpkg-tool-metadata.txt'
        $release = if (Test-Path -LiteralPath $metadata) { (ConvertFrom-StringData (Get-Content -LiteralPath $metadata -Raw)).VCPKG_TOOL_RELEASE_TAG } else { $null }
        if ($release) {
            $url = "https://github.com/microsoft/vcpkg-tool/releases/download/$release/vcpkg.exe"
            $partial = "$vcpkg.download"
            for ($attempt = 1; $attempt -le 3 -and -not (Test-Path -LiteralPath $vcpkg); ++$attempt) {
                try {
                    Info "Downloading vcpkg.exe directly (attempt $attempt of 3) ..."
                    Invoke-WebRequest -Uri $url -OutFile $partial -UseBasicParsing
                    $signature = Get-AuthenticodeSignature -LiteralPath $partial
                    if ($signature.Status -eq 'Valid' -and $signature.SignerCertificate.Subject -match 'O=Microsoft Corporation') {
                        Move-Item -LiteralPath $partial -Destination $vcpkg -Force
                    } else {
                        Warn 'The downloaded vcpkg.exe has no valid Microsoft signature; it was discarded.'
                        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
                    }
                } catch {
                    Warn "Download failed: $($_.Exception.Message)"
                    Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
                    Start-Sleep -Seconds (5 * $attempt)
                }
            }
        }
    }
    if (-not (Test-Path -LiteralPath $vcpkg)) {
        Fail ("vcpkg.exe could not be downloaded from GitHub. Your PC could not reach github.com (internet, DNS, VPN, firewall or an ad blocker). " +
              "Check the connection and run Setup.bat again, or download vcpkg.exe with a browser from https://github.com/microsoft/vcpkg-tool/releases " +
              "and put it here: $vcpkg")
    }
    Set-Content -LiteralPath (Join-Path $vcpkgRoot 'vcpkg.disable-metrics') -Value '' -ErrorAction SilentlyContinue
}
& $vcpkg install @VcpkgPackages --triplet x64-windows --disable-metrics; Check-Exit 'Installing vcpkg libraries'
$toolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
$script:StageTimings['3. vcpkg Libraries (zlib/curl/volk)'] = [math]::Round($script:StepSw.Elapsed.TotalSeconds, 2)

# ---------------------------------------------------------------------------
Step 'Building the window and shader-compiler libraries (SDL2, glslang)'
$script:StepSw = [System.Diagnostics.Stopwatch]::StartNew()
$deps = Join-Path $Work 'deps'
$sdlMarker = Join-Path $deps 'sdl2-vulkan\cmake\SDL2VulkanOnly.cmake'
$glslangMarker = Join-Path $deps 'glslang-vulkan\lib\cmake\glslang\GlslangVulkanOnly.cmake'
if (-not ((Test-Path -LiteralPath $sdlMarker) -and (Test-Path -LiteralPath $glslangMarker))) {
    $sdlZip = Join-Path $Downloads 'SDL2-2.32.10.zip'
    $glslangArchive = Join-Path $Downloads 'glslang-16.4.0.tar.gz'
    Download $SdlUrl $sdlZip
    Download $GlslangUrl $glslangArchive
    $sdlSource = Join-Path $Downloads 'SDL2-2.32.10'
    if (-not (Test-Path -LiteralPath $sdlSource)) {
        Info 'Extracting SDL2 source archive (fast zip)...'
        & $pyExe @pyArgs -c "import zipfile, sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" $sdlZip $Downloads
    }
    $depsBuild = Join-Path $Work 'deps-build'
    if (Test-Path -LiteralPath $depsBuild) { Remove-Item -LiteralPath $depsBuild -Recurse -Force }
    $cacheDir = Join-Path $Work 'cache\deps'
    & $pyExe @pyArgs (Join-Path $Recompiler 'tools\build_vulkan_dependencies.py') --sdl-source $sdlSource --glslang-archive $glslangArchive --build-root $depsBuild --prefix $deps --generator $generator --cmake $cmake --cache-dir $cacheDir
    Check-Exit 'Building SDL2/glslang'
}
Good 'Libraries ready.'
$script:StageTimings['4. SDL2 & glslang Compilers'] = [math]::Round($script:StepSw.Elapsed.TotalSeconds, 2)

# ---------------------------------------------------------------------------
Step 'Building the recompiler (RebrewU)'
$script:StepSw = [System.Diagnostics.Stopwatch]::StartNew()
$rebrewuBuild = Join-Path $Work 'rebrewu'
Reset-StaleCMakeCache $rebrewuBuild
& $cmake -S $Recompiler -B $rebrewuBuild -G $generator -A x64 "-DCMAKE_TOOLCHAIN_FILE=$toolchain" -DVCPKG_TARGET_TRIPLET=x64-windows -DREBREWU_BUILD_TESTS=OFF
Check-Exit 'Configuring the recompiler'
$rebrewuBuildArgs = @('--build', $rebrewuBuild, '--config', 'Release', '--target', 'rebrewu', '--parallel')
if ($generator -match 'Visual Studio') { $rebrewuBuildArgs += @('--', '/p:TrackFileAccess=false') }
& $cmake @rebrewuBuildArgs
Check-Exit 'Building the recompiler'
$rebrewu = Get-ChildItem -LiteralPath $rebrewuBuild -Recurse -Filter rebrewu.exe | Select-Object -First 1
if (-not $rebrewu) { Fail 'rebrewu.exe was not produced.' }
Good "Recompiler: $($rebrewu.FullName)"
$script:StageTimings['5. RebrewU Recompiler Build'] = [math]::Round($script:StepSw.Elapsed.TotalSeconds, 2)

# ---------------------------------------------------------------------------
Step 'Recompiling your Gambit.rpx into C++ (a few minutes)'
$script:StepSw = [System.Diagnostics.Stopwatch]::StartNew()
$generated = Join-Path $Work 'generated\Gambit'
$stamp = Join-Path $Work 'generated\source.sha256'
# The manifest carries the entry boundaries, hook list and patches. The RPX
# hash above selects its supported version; RebrewU still validates each
# boundary against the RPX. Include the tool and manifest hashes in the stamp
# so changes regenerate.
$current = $rpxHash + ':' + (File-Sha256 $rebrewu.FullName) + ':' + (File-Sha256 $SelectedManifest)
if (-not ((Complete-GeneratedModule $generated) -and (Test-Path -LiteralPath $stamp) -and ((Get-Content -LiteralPath $stamp -Raw).Trim() -eq $current))) {
    # Safe incremental regeneration: CppEmitter preserves unchanged file timestamps
    # and safely archives obsolete parts to avoid wiping valid compilation units.
    Ensure-Folder $generated
    & $rebrewu.FullName recompile --rpx $rpx --output $generated --manifest $SelectedManifest
    Check-Exit 'Recompiling Gambit.rpx'
    if (-not (Complete-GeneratedModule $generated)) { Fail 'The recompiler did not produce the complete supported-game function table. Installation stopped before building an incomplete game.' }
    Set-Content -LiteralPath $stamp -Value $current -Encoding ASCII
}
Good 'Game code recompiled (stays on your PC, inside build\generated).'
$script:StageTimings['6. RPX Decompilation to C++'] = [math]::Round($script:StepSw.Elapsed.TotalSeconds, 2)

# ---------------------------------------------------------------------------
Step 'Building the game (this is the long part: 15-60 minutes)'
$script:StepSw = [System.Diagnostics.Stopwatch]::StartNew()
$gameBuild = Join-Path $Work 'game'
Reset-StaleCMakeCache $gameBuild
$gameCmakeArgs = @(
    '-S', (Join-Path $Recompiler 'port'),
    '-B', $gameBuild,
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    '-DVCPKG_TARGET_TRIPLET=x64-windows',
    '-DRBREW_GRAPHICS_BACKEND=Vulkan',
    "-DRBREW_RPX=$rpx",
    "-DGAMBIT_GENERATED_DIR=$generated",
    "-DSDL2_DIR=$(Split-Path $sdlMarker)",
    "-Dglslang_DIR=$(Split-Path $glslangMarker)",
    '-DRBREW_BUILD_GAME=ON',
    '-DRBREW_ENABLE_COVERAGE=OFF',
    '-DRBREW_ENABLE_PPC_FALLBACK=OFF',
    '-DRBREW_DIAGNOSTIC_RUNTIME=OFF',
    '-DRBREW_TEXTURE_DIAGNOSTICS=OFF'
)
if ($ninja) {
    $gameCmakeArgs += @('-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja", '-DCMAKE_BUILD_TYPE=Release')
} else {
    $gameCmakeArgs += @('-G', $generator, '-A', 'x64')
}
& $cmake @gameCmakeArgs
Check-Exit 'Configuring the game'
if ($ninja) {
    $workerCount = [Environment]::ProcessorCount
    $gameBuildArgs = @('--build', $gameBuild, '--target', 'Gambit', '--', '-j', "$workerCount")
} else {
    $gameBuildArgs = @('--build', $gameBuild, '--config', 'Release', '--target', 'Gambit', '--parallel')
    if ($generator -match 'Visual Studio') { $gameBuildArgs += @('--', '/p:TrackFileAccess=false') }
}
& $cmake @gameBuildArgs
Check-Exit 'Building the game'
$exe = Get-ChildItem -LiteralPath $gameBuild -Recurse -Filter Splatoon.exe | Select-Object -First 1
if (-not $exe) { Fail 'Splatoon.exe was not produced.' }
Good "Built: $($exe.FullName)"
$script:StageTimings['7. Game Compilation & Linking'] = [math]::Round($script:StepSw.Elapsed.TotalSeconds, 2)

# ---------------------------------------------------------------------------
Step 'Creating the game folder'
$script:StepSw = [System.Diagnostics.Stopwatch]::StartNew()
Ensure-Folder $Output
Initialize-WiiUFolders $Output
Copy-Item -LiteralPath $exe.FullName -Destination (Join-Path $Output 'Splatoon.exe') -Force
# Any library DLLs the build placed next to the executable (normally none).
Get-ChildItem -LiteralPath $exe.DirectoryName -Filter *.dll | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $Output -Force }
# Microsoft C++ runtime, copied from your Visual Studio install.
$crt = Get-ChildItem -LiteralPath (Join-Path $vs.installationPath 'VC\Redist\MSVC') -Recurse -Directory -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' -and $_.FullName -notmatch 'onecore|debug' } | Sort-Object FullName -Descending | Select-Object -First 1
if ($crt) {
    foreach ($dll in @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        $source = Join-Path $crt.FullName $dll
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $Output -Force }
    }
} else { Warn 'Could not find the Visual C++ runtime DLLs; install the "Microsoft Visual C++ Redistributable (x64)" if the game does not start.' }
Info 'Copying game content (about 1.7 GB) ...'
Copy-Tree (Join-Path $GameDump 'content') (Join-Path $Output 'content')
Copy-Tree (Join-Path $GameDump 'meta') (Join-Path $Output 'meta')
Good 'Wii U folders ready. No existing game or Mii saves were imported.'
Good 'A new installation starts fresh; progress already in this output folder is kept on updates.'
$script:StageTimings['8. Game Folder Packaging'] = [math]::Round($script:StepSw.Elapsed.TotalSeconds, 2)

# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '============================================================' -ForegroundColor Green
Write-Host '   Setup complete!' -ForegroundColor Green
Write-Host "   Play: $(Join-Path $Output 'Splatoon.exe')" -ForegroundColor Green
Write-Host '============================================================' -ForegroundColor Green
if (Ask-YesNo 'Create a desktop shortcut?') {
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Splatoon PC Port.lnk'))
    $shortcut.TargetPath = Join-Path $Output 'Splatoon.exe'
    $shortcut.WorkingDirectory = $Output
    $shortcut.Save()
    Good 'Shortcut created on your desktop.'
}

# ---------------------------------------------------------------------------
$totalElapsed = [math]::Round($script:TotalWatch.Elapsed.TotalSeconds, 2)
Write-Host ''
Write-Host '------------------------------------------------------------' -ForegroundColor DarkCyan
Write-Host "   BUILD PERFORMANCE REPORT (Total: ${totalElapsed}s)" -ForegroundColor DarkCyan
Write-Host '------------------------------------------------------------' -ForegroundColor DarkCyan
foreach ($entry in $script:StageTimings.GetEnumerator()) {
    Write-Host ("   {0,-42} : {1,8} s" -f $entry.Key, $entry.Value) -ForegroundColor Gray
}
Write-Host '------------------------------------------------------------' -ForegroundColor DarkCyan
Remove-BuildDrive
