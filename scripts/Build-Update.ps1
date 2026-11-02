param(
 [Parameter(Mandatory=$true)][string]$BaseDump,
 [Parameter(Mandatory=$true)][string]$UpdateDump,
 [Parameter(Mandatory=$true)][string]$DependencyRoot,
 [string]$Output = '',
 [switch]$Diagnostics,
 [switch]$Optimize
)
$ErrorActionPreference='Stop'
$Root=Split-Path -Parent $PSScriptRoot
$Work=Join-Path $Root 'build'
if(-not $Output){$Output=Join-Path $Root 'Game'}
function Checked([string]$What){if($LASTEXITCODE -ne 0){throw "$What failed: $LASTEXITCODE"}}
function CopyTree([string]$From,[string]$To){& robocopy $From $To /E /NFL /NDL /NJH /NJS /NP | Out-Null; if($LASTEXITCODE -ge 8){throw "Copy failed: $From"};$global:LASTEXITCODE=0}
$RevisionProfile=Get-Content -LiteralPath (Join-Path $Root 'profiles\agmp-v288.json') -Raw | ConvertFrom-Json
foreach($InputCheck in @(@($BaseDump,$RevisionProfile.base_rpx_sha256),@($UpdateDump,$RevisionProfile.rpx_sha256))){
 $InputPath=$InputCheck[0]
 foreach($Directory in @('code','content','meta')){if(-not (Test-Path -LiteralPath (Join-Path $InputPath $Directory) -PathType Container)){throw "Missing $Directory in $InputPath"}}
 if((Get-FileHash -LiteralPath (Join-Path $InputPath 'code\Gambit.rpx') -Algorithm SHA256).Hash -ne $InputCheck[1]){throw "RPX revision mismatch: $InputPath"}
}
[xml]$UpdateMeta=Get-Content -LiteralPath (Join-Path $UpdateDump 'meta\meta.xml') -Raw
if($UpdateMeta.SelectSingleNode('/menu/title_version').InnerText -ne '288'){throw 'Expected title version 288'}
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
# Steps 1-3 are skipped when their inputs did not change since the last build (stamp files), so an
# update that only touches the runtime rebuilds in minutes instead of re-translating the whole game.
function Get-InputsStamp([string[]]$Paths,[string[]]$Extra){
 $Lines=New-Object System.Collections.Generic.List[string]
 foreach($Line in $Extra){$Lines.Add($Line)}
 foreach($Path in $Paths){
  if(Test-Path -LiteralPath $Path -PathType Container){$Files=Get-ChildItem -LiteralPath $Path -Recurse -File | Sort-Object FullName}
  elseif(Test-Path -LiteralPath $Path -PathType Leaf){$Files=@(Get-Item -LiteralPath $Path)}
  else{$Lines.Add("missing $Path");continue}
  foreach($File in $Files){$Lines.Add($File.FullName.Substring($Root.Length)+' '+(Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash)}
 }
 $Bytes=[Text.Encoding]::UTF8.GetBytes(($Lines -join "`n"))
 return [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($Bytes)).Replace('-','')
}
function Test-Stamp([string]$File,[string]$Value){(Test-Path -LiteralPath $File -PathType Leaf) -and ((Get-Content -LiteralPath $File -Raw).Trim() -eq $Value)}
$RecompilerBuild=Join-Path $Work 'rebrewu'
$Generated=Join-Path $Work 'generated-v288'
$RecompilerExe=Join-Path $RecompilerBuild 'bin\Release\rebrewu.exe'
# The import addresses of the 288 image come from its manifest.
$Manifest=Join-Path $Root 'Manifest\Gambit_v288.manifest'
if(-not(Test-Path -LiteralPath $Manifest -PathType Leaf)){throw "Missing manifest: $Manifest"}
$TranslateStamp=Join-Path $Generated '.inputs'
$TranslateInputs=Get-InputsStamp @((Join-Path $Root 'recompiler\src'),(Join-Path $Root 'recompiler\include'),(Join-Path $Root 'recompiler\cmake'),(Join-Path $Root 'recompiler\CMakeLists.txt'),$Manifest,(Join-Path $Root 'profiles\agmp-v288.entries'),(Join-Path $Root 'profiles\agmp-v288.hooks'),(Join-Path $Root 'profiles\agmp-v288.tables')) @("rpx $($RevisionProfile.rpx_sha256)")
if((Test-Stamp $TranslateStamp $TranslateInputs) -and (Test-Path -LiteralPath (Join-Path $Generated 'Gambit.h') -PathType Leaf)){
 Write-Output '[1/5] Recompiler unchanged: skipped'
 Write-Output '[2/5] Game translation unchanged: skipped'
}else{
 Write-Output '[1/5] Build revision-aware recompiler'
 & $Cmake -S (Join-Path $Root 'recompiler') -B $RecompilerBuild -G $Generator -A x64 "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" -DREBREWU_BUILD_TESTS=OFF
 Checked 'Configure recompiler'
 & $Cmake --build $RecompilerBuild --config Release --target rebrewu --parallel -- /p:TrackFileAccess=false
 Checked 'Build recompiler'
 Write-Output '[2/5] Translate update RPX with revision-specific candidates'
 if(Test-Path -LiteralPath $TranslateStamp){Remove-Item -LiteralPath $TranslateStamp -Force}
 & $RecompilerExe recompile --rpx (Join-Path $UpdateDump 'code\Gambit.rpx') --output $Generated --manifest $Manifest --analyzer-entries (Join-Path $Root 'profiles\agmp-v288.entries') --discover-referenced-entries --hooks (Join-Path $Root 'profiles\agmp-v288.hooks') --hints-file (Join-Path $Root 'profiles\agmp-v288.tables') -v
 Checked 'Translate update RPX'
 Set-Content -LiteralPath $TranslateStamp -Value $TranslateInputs -Encoding ASCII
}
$Merged=Join-Path $Work 'merged-v288'
$MergeStamp=Join-Path $Merged '.inputs'
$MergeInputs=Get-InputsStamp @((Join-Path $BaseDump 'meta\meta.xml'),(Join-Path $UpdateDump 'meta\meta.xml')) @("base $((Resolve-Path -LiteralPath $BaseDump).Path)","update $((Resolve-Path -LiteralPath $UpdateDump).Path)")
$MergeSkipped=(Test-Stamp $MergeStamp $MergeInputs) -and (Test-Path -LiteralPath (Join-Path $Merged 'content') -PathType Container)
if($MergeSkipped){
 Write-Output '[3/5] Game content unchanged: skipped'
}else{
 Write-Output '[3/5] Merge owned content into private staging folder'
 if(Test-Path -LiteralPath $MergeStamp){Remove-Item -LiteralPath $MergeStamp -Force}
 CopyTree $BaseDump $Merged
 CopyTree $UpdateDump $Merged
 Set-Content -LiteralPath $MergeStamp -Value $MergeInputs -Encoding ASCII
}
Write-Output '[4/5] Build experimental update runtime'
$GameBuild=Join-Path $Work $(if($Optimize){'game-v288-release'}else{'game-v288-diagnostic'})
$FastGuest=if($Optimize){'OFF'}else{'ON'}
$DiagnosticRuntime=if($Optimize -and -not $Diagnostics){'OFF'}else{'ON'}
& $Cmake -S (Join-Path $Root 'recompiler\port') -B $GameBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" "-DGAMBIT_GENERATED_DIR=$Generated" "-DRBREW_MANIFEST=$Manifest" "-DSDL2_DIR=$Sdl" "-Dglslang_DIR=$Glslang" -DRBREW_GAME_REVISION=288 -DRBREW_GRAPHICS_BACKEND=Vulkan -DRBREW_BUILD_GAME=ON "-DRBREW_DIAGNOSTIC_RUNTIME=$DiagnosticRuntime" "-DRBREW_FAST_DIAGNOSTIC_GUEST=$FastGuest" -DRBREW_ENABLE_PPC_FALLBACK=OFF
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
$OutputStamp=Join-Path $Output '.content-inputs'
if(-not($MergeSkipped -and (Test-Stamp $OutputStamp $MergeInputs))){
 CopyTree (Join-Path $Merged 'content') (Join-Path $Output 'content')
 CopyTree (Join-Path $Merged 'meta') (Join-Path $Output 'meta')
 Set-Content -LiteralPath $OutputStamp -Value $MergeInputs -Encoding ASCII
}
# The prepared dependencies usually live in this same checkout (DependencyRoot = repository root),
# so source and destination can be the same folder: there is nothing to copy then.
$DependencyGame=Join-Path $DependencyRoot 'Game'
# With a custom Setup output folder there is no <DependencyRoot>\Game: Setup already put the
# system titles and runtime DLLs into that folder, so there is nothing to copy either.
$CopyDependencyGame=(Test-Path -LiteralPath $DependencyGame -PathType Container) -and -not ((Resolve-Path -LiteralPath $DependencyGame).Path.TrimEnd('\') -eq (Resolve-Path -LiteralPath $Output).Path.TrimEnd('\'))
$DependencyTitles=Join-Path $DependencyGame 'mlc01\sys\title'
if($CopyDependencyGame -and (Test-Path -LiteralPath $DependencyTitles -PathType Container)){CopyTree $DependencyTitles (Join-Path $Output 'mlc01\sys\title')}
Copy-Item -LiteralPath (Join-Path $GameBuild 'Splatoon.exe') -Destination (Join-Path $Output 'Splatoon-v288.exe') -Force
Get-ChildItem -LiteralPath $GameBuild -Filter '*.dll' | Copy-Item -Destination $Output -Force
if($CopyDependencyGame){Get-ChildItem -LiteralPath $DependencyGame -Filter '*.dll' | Copy-Item -Destination $Output -Force}
$ZlibDll=Join-Path $Output 'z.dll'
if(-not(Test-Path -LiteralPath $ZlibDll)){
 $ZlibSource=Join-Path $DependencyRoot 'build\vcpkg\installed\x64-windows\bin\z.dll'
 if(-not(Test-Path -LiteralPath $ZlibSource)){throw "Missing required zlib runtime: $ZlibSource"}
 Copy-Item -LiteralPath $ZlibSource -Destination $ZlibDll
}
Write-Output "EXPERIMENTAL BUILD COMPLETE: $Output\Splatoon-v288.exe"

