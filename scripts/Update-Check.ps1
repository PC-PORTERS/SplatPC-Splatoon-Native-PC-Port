# Start-up update check, called by Splatoon-Login.ps1 before the sign-in screen.
# Asks GitHub (repository= in update.ini) for the newest tag online-vX.Y.Z; when it is newer than the
# installed version, offers "update now" (Update-Apply.ps1) or "later". No repository, no
# Internet or any error: returns at once, silently, and the game starts as usual.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

function Read-Repository {
  $file = Join-Path $Root 'update.ini'
  if (-not (Test-Path -LiteralPath $file)) { return '' }
  foreach ($line in Get-Content -LiteralPath $file) {
    if ($line -match '^\s*repository\s*=\s*([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)\s*$') { return $Matches[1] }
  }
  return ''
}
# The version that was last BUILT (an update whose build failed is offered again), else VERSION.
function Get-InstalledVersion {
  foreach ($file in @((Join-Path $Root 'build\installed-version.txt'), (Join-Path $Root 'VERSION'))) {
    if (Test-Path -LiteralPath $file) {
      $text = (Get-Content -LiteralPath $file -Raw).Trim().TrimStart('v')
      $parsed = $null
      if ([version]::TryParse($text, [ref]$parsed)) { return [version]::new($parsed.Major, $parsed.Minor, [Math]::Max($parsed.Build, 0)) }
    }
  }
  return [version]'0.0.0'
}

# No signing key shipped (scripts/allowed_signers): updates cannot be verified, so none is offered.
function Test-Signers {
  $file = Join-Path $PSScriptRoot 'allowed_signers'
  return (Test-Path -LiteralPath $file) -and [bool](Get-Content -LiteralPath $file | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') })
}

try {
  $repository = Read-Repository
  if (-not $repository -or -not (Test-Signers)) { exit 0 }
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  # The online edition lives on its own branch of a project that has releases of its own, so its
  # versions are the tags online-v1.2.3 (not "the latest release"); the newest one wins.
  # Windows PowerShell 5.1 hands a JSON array over as ONE object: unroll it, or the tags run together.
  $refs = @(Invoke-RestMethod -Uri "https://api.github.com/repos/$repository/git/matching-refs/tags/online-v" `
    -Headers @{ 'User-Agent' = 'splatoon-pc-launcher' } -TimeoutSec 3 | ForEach-Object { $_ })
  $tag = ''
  $latest = $null
  foreach ($ref in $refs) {
    $name = ([string]$ref.ref) -replace '^refs/tags/', ''
    $parsed = $null
    if ($name -match '^online-v(\d+\.\d+(\.\d+)?)$' -and [version]::TryParse($Matches[1], [ref]$parsed)) {
      $parsed = [version]::new($parsed.Major, $parsed.Minor, [Math]::Max($parsed.Build, 0))
      if (-not $latest -or $parsed -gt $latest) { $latest = $parsed; $tag = $name }
    }
  }
  if (-not $latest) { exit 0 }
  $installed = Get-InstalledVersion
  if ($latest -le $installed) { exit 0 }

  Add-Type -AssemblyName System.Windows.Forms
  $answer = [System.Windows.Forms.MessageBox]::Show(
    "Version $latest is available (you have $installed).`n`nUpdate now? The game is rebuilt on this PC: a few minutes for a small update, longer for a big one. Your save and settings are kept.",
    'Splatoon - update', 'YesNo', 'Information')
  if ($answer -ne 'Yes') { exit 0 }
  $apply = Join-Path $PSScriptRoot 'Update-Apply.ps1'
  $process = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -Wait -PassThru `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $apply, '-Repository', $repository, '-Tag', $tag)
  exit $process.ExitCode
} catch {
  exit 0   # never block the game on a failed check
}
