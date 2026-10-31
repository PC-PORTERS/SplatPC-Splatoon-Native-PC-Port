# Guard tests for the update build in Install-Update.ps1: never copy Game\ onto itself, and skip the copy when a
# custom Setup output folder means there is no <DependencyRoot>\Game at all.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $PSScriptRoot
$buildUpdate = Join-Path $here 'Install-Update.ps1'
$failures = 0

function Expect([string]$name, [bool]$ok, [string]$details) {
  if ($ok) { Write-Host "PASS $name" } else { Write-Host "FAIL $name`: $details"; $script:failures++ }
}

$text = Get-Content -LiteralPath $buildUpdate -Raw
Expect 'guard variable exists' ($text -match '\$CopyDependencyGame=') 'no $CopyDependencyGame'
Expect 'title copy is guarded' ($text -match 'if\(\$CopyDependencyGame -and \(Test-Path -LiteralPath \$DependencyTitles') 'unguarded title copy'
Expect 'dll copy is guarded' ($text -match 'if\(\$CopyDependencyGame\)\{Get-ChildItem -LiteralPath \$DependencyGame') 'unguarded dll copy'
Expect 'unconditional Game copy removed' ($text -notmatch '(?m)^CopyTree \(Join-Path \$DependencyRoot ''Game\\mlc01') 'old copy still present'
Expect 'manifest is passed to the build' (($text -match '-DRBREW_MANIFEST=\$Manifest') -and ($text -match '--manifest \$Manifest')) 'manifest not passed'

# Same expression as the script.
function Test-Copy([string]$DependencyGame, [string]$Output) {
  return (Test-Path -LiteralPath $DependencyGame -PathType Container) -and -not ((Resolve-Path -LiteralPath $DependencyGame).Path.TrimEnd('\') -eq (Resolve-Path -LiteralPath $Output).Path.TrimEnd('\'))
}
$dir = Join-Path ([IO.Path]::GetTempPath()) ("splat-same-" + [guid]::NewGuid().ToString('N'))
$other = "$dir-other"
New-Item -ItemType Directory -Path $dir, $other -Force | Out-Null
Expect 'same folder is not copied' (-not (Test-Copy $dir $dir)) 'copy for identical paths'
Expect 'missing dependency Game is not copied' (-not (Test-Copy (Join-Path $dir 'absent') $other)) 'copy from a missing folder'
Expect 'separate folder is copied' (Test-Copy $dir $other) 'no copy for distinct folders'
Remove-Item -LiteralPath $dir, $other -Recurse -Force

if ($failures -gt 0) { Write-Host "$failures test(s) failed"; exit 1 }
Write-Host 'All Build-Update guard tests passed.'
exit 0
