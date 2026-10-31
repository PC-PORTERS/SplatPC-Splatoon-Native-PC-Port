# Validation tests for Install-Update.ps1 (fake folders only, no game data).
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $PSScriptRoot
$installer = Join-Path $here 'Install-Update.ps1'
$manifests = Join-Path (Split-Path -Parent $here) 'Manifest'
$readme = Join-Path (Split-Path -Parent $here) 'README.md'
$failures = 0

function New-FakeRoot {
  $root = Join-Path ([IO.Path]::GetTempPath()) ("splat-install-" + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path (Join-Path $root 'scripts') -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $root 'Manifest') -Force | Out-Null
  Copy-Item -LiteralPath $installer -Destination (Join-Path $root 'scripts\Install-Update.ps1')
  foreach ($name in 'Gambit_v16.manifest', 'Gambit_v288.manifest') { Copy-Item -LiteralPath (Join-Path $manifests $name) -Destination (Join-Path $root (Join-Path 'Manifest' $name)) }
  return $root
}

function Add-BaseInstall([string]$root) {
  $game = Join-Path $root 'Game'
  New-Item -ItemType Directory -Path (Join-Path $game 'content') -Force | Out-Null
  Set-Content -LiteralPath (Join-Path $game 'Splatoon.exe') -Value 'fake' -Encoding ascii
}

function Add-UpdateDump([string]$root, [string]$version = '288') {
  $dump = Join-Path $root 'update dump [v288]'
  foreach ($part in 'code', 'content', 'meta') {
    New-Item -ItemType Directory -Path (Join-Path $dump $part) -Force | Out-Null
  }
  Set-Content -LiteralPath (Join-Path $dump 'code\Gambit.rpx') -Value 'fake rpx' -Encoding ascii
  Set-Content -LiteralPath (Join-Path $dump 'meta\meta.xml') -Value "<menu><title_version>$version</title_version></menu>" -Encoding ascii
  return $dump
}

function Add-Dependencies([string]$root) {
  $marker = Join-Path $root 'build\vcpkg\scripts\buildsystems\vcpkg.cmake'
  New-Item -ItemType Directory -Path (Split-Path -Parent $marker) -Force | Out-Null
  Set-Content -LiteralPath $marker -Value '# fake' -Encoding ascii
}

function Invoke-Installer([string]$root, [string[]]$extra) {
  $script = Join-Path $root 'scripts\Install-Update.ps1'
  $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $script @extra 2>&1 | Out-String
  return @{ Code = $LASTEXITCODE; Output = $output }
}

function Expect([string]$name, [bool]$ok, [string]$details) {
  if ($ok) { Write-Host "PASS $name" } else { Write-Host "FAIL $name`: $details"; $script:failures++ }
}

# Release message and README must point at the v288 executable, not the base one.
$text = Get-Content -LiteralPath $installer -Raw
Expect 'installer names the versioned exe' ($text.Contains('Splatoon-v') -and $text.Contains('.version')) 'missing versioned exe'
Expect 'installer does not name base exe' ($text -notmatch 'Start the game with Game\\Splatoon\.exe') 'old base exe message'
$readmeText = Get-Content -LiteralPath $readme -Raw
Expect 'README names Splatoon-v288.exe' ($readmeText -match 'Splatoon-v288\.exe') 'missing v288 exe'
Expect 'README does not name base exe' ($readmeText -notmatch 'Start the game with `Game\\Splatoon\.exe`') 'old base exe message'

$root = New-FakeRoot
$r = Invoke-Installer $root @('-CheckDependencies')
Expect 'dependencies: absent is refused' ($r.Code -eq 2 -and $r.Output -match 'Prepared dependencies not found') $r.Output

Add-Dependencies $root
$r = Invoke-Installer $root @('-CheckDependencies')
Expect 'dependencies: normal root is found' ($r.Code -eq 0 -and $r.Output -match 'Dependencies OK') $r.Output

$other = Join-Path $root 'prepared-upstream'
Add-Dependencies $other
$r = Invoke-Installer $root @('-CheckDependencies', '-DependencyRoot', $other)
Expect 'dependencies: explicit root is used' ($r.Code -eq 0 -and $r.Output -match 'Dependencies OK') $r.Output

Remove-Item -LiteralPath (Join-Path $root 'build') -Recurse -Force
$r = Invoke-Installer $root @('-CheckDependencies', '-DependencyRoot', $other)
Expect 'dependencies: explicit root works when local is absent' ($r.Code -eq 0 -and $r.Output -match 'Dependencies OK') $r.Output

$r = Invoke-Installer $root @('-UpdateDump', (Join-Path $root 'missing'), '-BaseDump', $root)
Expect 'missing base install is refused' ($r.Code -eq 2 -and $r.Output -match 'not built yet') $r.Output

Add-BaseInstall $root
$r = Invoke-Installer $root @('-UpdateDump', (Join-Path $root 'missing'), '-BaseDump', $root)
Expect 'missing update dump is refused' ($r.Code -eq 2 -and $r.Output -match 'not found') $r.Output

$dump = Add-UpdateDump $root
$r = Invoke-Installer $root @('-UpdateDump', $dump, '-BaseDump', (Join-Path $root 'Game'))
Expect 'wrong RPX hash is refused' ($r.Code -eq 2 -and $r.Output -match 'not a supported game update') $r.Output

$bad = Join-Path $root 'update-wrong-version'
foreach ($part in 'code', 'content', 'meta') {
  New-Item -ItemType Directory -Path (Join-Path $bad $part) -Force | Out-Null
}
Copy-Item -LiteralPath (Join-Path $dump 'code\Gambit.rpx') -Destination (Join-Path $bad 'code') -Force
Set-Content -LiteralPath (Join-Path $bad 'meta\meta.xml') -Value '<menu><title_version>16</title_version></menu>' -Encoding ascii
# Let the fake RPX pass the hash check so that the title version is what refuses it.
$fakeManifest = Join-Path $root 'Manifest\Gambit_v288.manifest'
$newHash = (Get-FileHash -LiteralPath (Join-Path $bad 'code\Gambit.rpx') -Algorithm SHA256).Hash.ToLower()
(Get-Content -LiteralPath $fakeManifest) -replace '^(image_sha256\s*=\s*)\w+', ('${1}' + $newHash) | Set-Content -LiteralPath $fakeManifest -Encoding ascii
$r = Invoke-Installer $root @('-UpdateDump', $bad, '-BaseDump', (Join-Path $root 'Game'))
Expect 'wrong title version is refused' ($r.Code -eq 2 -and $r.Output -match 'Expected title version 288') $r.Output

Remove-Item -LiteralPath $root -Recurse -Force
if ($failures -gt 0) { Write-Host "$failures test(s) failed"; exit 1 }
Write-Host 'All installer validation tests passed.'
exit 0
