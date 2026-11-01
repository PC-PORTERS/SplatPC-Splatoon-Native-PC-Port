param(
  [Parameter(Mandatory = $true)][string]$Repository,   # owner/name on GitHub
  [Parameter(Mandatory = $true)][string]$Tag           # version tag of the online edition, for example online-v0.2.0
)
# Applies an update in THIS folder, so the build folders are kept and only changed files are
# recompiled. NOTHING from the new version is run or copied before its tag is verified: the tag
# must carry an SSH signature made by a key listed in scripts\allowed_signers of the version that
# is ALREADY installed (see docs\RELEASES.md). A git clone then checks out the tag; a ZIP install
# gets the tag's archive from GitHub (HTTPS, this repository only), checked against the verified
# commit, and copies it over without touching build\ and Game\. The game is then rebuilt with the
# dumps recorded at install time (Install-Update.ps1); the previous game stays in place until the
# new build succeeds.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
function Fail([string]$Message) {
  Write-Host ''
  Write-Host "UPDATE FAILED: $Message" -ForegroundColor Red
  Write-Host 'The previous version is still installed and playable.'
  Read-Host 'Press Enter to close'
  exit 2
}
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or $Tag -notmatch '^online-v\d+\.\d+(\.\d+)?$') { Fail 'Invalid repository or tag.' }
Write-Host "Splatoon PC - updating to $Tag" -ForegroundColor Cyan

# 1. Verify the signature, in a throw-away repository, before anything else.
$signers = Join-Path $PSScriptRoot 'allowed_signers'
$hasSigner = (Test-Path -LiteralPath $signers) -and [bool](Get-Content -LiteralPath $signers | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') })
if (-not $hasSigner) { Fail 'This version has no update signing key (scripts\allowed_signers): updates are off.' }
$git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $git) { Fail 'Git is required to check the signature of the update.' }
# Native git writes its verdicts to stderr, which PowerShell 5.1 would turn into a terminating error under
# 'Stop': every call below goes through this wrapper, which keeps stdout and the exit code only.
$script:GitExe = $git.Source
function git {
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & $script:GitExe @args 2>$null } finally { $ErrorActionPreference = $previous }
  $global:LASTEXITCODE = $LASTEXITCODE
}
$work = Join-Path $env:TEMP ('splatoon-update-' + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $work | Out-Null
$verify = Join-Path $work 'verify.git'
& git init --quiet --bare $verify
if ($LASTEXITCODE -ne 0) { Fail 'Could not prepare the signature check.' }
& git -C $verify fetch --quiet --depth 1 "https://github.com/$Repository.git" "refs/tags/${Tag}:refs/tags/$Tag"
if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue; Fail 'Could not download the update (git fetch).' }
& git -C $verify -c gpg.format=ssh -c "gpg.ssh.allowedSignersFile=$signers" verify-tag $Tag 2>$null
if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue; Fail "The tag $Tag is not signed by a trusted key. Nothing was installed." }
$commit = (& git -C $verify rev-parse "$Tag^{commit}").Trim()
Write-Host "Signature OK ($($commit.Substring(0, 10)))." -ForegroundColor Green

$needsBuild = $true
if ((Test-Path -LiteralPath (Join-Path $Root '.git')) -and $git) {
  & git -C $Root diff --quiet HEAD -- 2>$null
  if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue; Fail 'This folder has local changes (git). Commit or discard them first.' }
  $before = (& git -C $Root rev-parse HEAD).Trim()
  & git -C $Root fetch --quiet --tags "https://github.com/$Repository.git" "refs/tags/${Tag}:refs/tags/$Tag"
  if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue; Fail 'Could not download the update (git fetch).' }
  # The tag could have moved between the check and this download: it must be the verified commit.
  if ((& git -C $Root rev-parse "$Tag^{commit}").Trim() -ne $commit) {
    & git -C $Root tag -d $Tag 2>$null | Out-Null
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    Fail 'The tag changed during the update. Nothing was installed.'
  }
  & git -C $Root -c advice.detachedHead=false checkout --quiet $Tag
  if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue; Fail "Could not switch to $Tag." }
  # Only scripts or documents changed: nothing to rebuild.
  $changed = @(& git -C $Root diff --name-only $before $Tag)
  $needsBuild = [bool]($changed | Where-Object { $_ -match '^(recompiler/(port|runtime|src|include|cmake)/|recompiler/CMakeLists\.txt|profiles/|Manifest/)' })
} else {
  $zip = Join-Path $work 'update.zip'
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  try {
    Invoke-WebRequest -Uri "https://github.com/$Repository/archive/refs/tags/$Tag.zip" -OutFile $zip -UseBasicParsing -TimeoutSec 120
  } catch { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue; Fail "Could not download the update: $($_.Exception.Message)" }
  # GitHub writes the commit id in the archive comment: it must be the one whose signature was checked.
  $bytes = [IO.File]::ReadAllBytes($zip)
  $comment = ''
  for ($i = $bytes.Length - 22; $i -ge [Math]::Max(0, $bytes.Length - 70000); $i--) {
    if ($bytes[$i] -eq 0x50 -and $bytes[$i + 1] -eq 0x4B -and $bytes[$i + 2] -eq 5 -and $bytes[$i + 3] -eq 6) {
      $length = [BitConverter]::ToUInt16($bytes, $i + 20)
      if ($length -gt 0 -and $i + 22 + $length -le $bytes.Length) { $comment = [Text.Encoding]::ASCII.GetString($bytes, $i + 22, $length).Trim() }
      break
    }
  }
  if ($comment -ne $commit) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue; Fail 'The downloaded archive is not the signed version. Nothing was installed.' }
  $unpacked = Join-Path $work 'files'
  Expand-Archive -LiteralPath $zip -DestinationPath $unpacked -Force
  $source = Get-ChildItem -LiteralPath $unpacked -Directory | Select-Object -First 1
  if (-not $source -or -not (Test-Path -LiteralPath (Join-Path $source.FullName 'Setup.bat'))) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue; Fail 'The downloaded archive is not this project.' }
  & robocopy $source.FullName $Root /E /XD build Game .git /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue; Fail 'Could not copy the new files.' }
}
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue

if ($needsBuild) {
  Write-Host 'Rebuilding the game (only what changed).' -ForegroundColor Cyan
  & (Join-Path $PSScriptRoot 'Install-Update.ps1')
  if ($LASTEXITCODE -ne 0) { Fail "The game build failed (exit code $LASTEXITCODE)." }
}
New-Item -ItemType Directory -Path (Join-Path $Root 'build') -Force | Out-Null
Set-Content -LiteralPath (Join-Path $Root 'build\installed-version.txt') -Value ($Tag -replace '^online-v', '') -Encoding ASCII
Write-Host "Updated to $Tag." -ForegroundColor Green
exit 0
