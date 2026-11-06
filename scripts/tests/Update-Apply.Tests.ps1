# Tests for Update-Apply.ps1 (fake folders only, no network, no game data).
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $PSScriptRoot
$apply = Join-Path $here 'Update-Apply.ps1'
$failures = 0
function Expect([string]$name, [bool]$ok, [string]$details) {
  if ($ok) { Write-Host "PASS $name" } else { Write-Host "FAIL $name`: $details"; $script:failures++ }
}

$text = Get-Content -LiteralPath $apply -Raw
Expect 'no bare folder names in /XD' ($text -notmatch '/XD build Game') 'robocopy /XD build Game skips recompiler\port\os\game too'
Expect 'excluded folders are full paths of the archive' ($text -match "\`$skip = @\('build', 'Game', '\.git'\) \| ForEach-Object \{ Join-Path \`$source\.FullName \`$_ \}") 'exclusion list changed'

# Same copy as the script, on a fake archive: nested "game" folders are copied, the top-level ones are not.
$base = Join-Path ([IO.Path]::GetTempPath()) ('splat-apply-' + [guid]::NewGuid().ToString('N'))
$source = New-Item -ItemType Directory -Path (Join-Path $base 'archive') -Force
$root = New-Item -ItemType Directory -Path (Join-Path $base 'install') -Force
foreach ($file in 'recompiler\port\os\game\hook.cpp', 'tools\build\note.txt', 'Game\should-not-copy.txt', 'build\should-not-copy.txt', 'VERSION') {
  $path = Join-Path $source.FullName $file
  New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
  Set-Content -LiteralPath $path -Value 'new'
}
New-Item -ItemType Directory -Path (Join-Path $root.FullName 'Game') -Force | Out-Null
Set-Content -LiteralPath (Join-Path $root.FullName 'Game\keep.txt') -Value 'player file'
$skip = @('build', 'Game', '.git') | ForEach-Object { Join-Path $source.FullName $_ }
& robocopy $source.FullName $root.FullName /E /XD @skip /NFL /NDL /NJH /NJS /NP | Out-Null
$code = $LASTEXITCODE
Expect 'robocopy succeeded' ($code -lt 8) "exit code $code"
Expect 'nested game folder is updated' (Test-Path -LiteralPath (Join-Path $root.FullName 'recompiler\port\os\game\hook.cpp')) 'os\game skipped'
Expect 'nested build folder is updated' (Test-Path -LiteralPath (Join-Path $root.FullName 'tools\build\note.txt')) 'tools\build skipped'
Expect 'top-level Game is not copied' (-not (Test-Path -LiteralPath (Join-Path $root.FullName 'Game\should-not-copy.txt'))) 'Game copied'
Expect 'top-level build is not copied' (-not (Test-Path -LiteralPath (Join-Path $root.FullName 'build\should-not-copy.txt'))) 'build copied'
Expect 'player files are kept' (Test-Path -LiteralPath (Join-Path $root.FullName 'Game\keep.txt')) 'Game\keep.txt lost'
Remove-Item -LiteralPath $base -Recurse -Force
$global:LASTEXITCODE = 0

if ($failures) { Write-Host "$failures test(s) failed"; exit 1 }
Write-Host 'All Update-Apply tests passed.'
exit 0
