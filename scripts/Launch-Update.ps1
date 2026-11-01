param()
$ErrorActionPreference='Stop'
$Root=Split-Path -Parent $PSScriptRoot
$Game=Join-Path $Root 'Game'
$Executable=Join-Path $Game 'Splatoon-v288.exe'
if(-not(Test-Path -LiteralPath $Executable)){throw 'Experimental update executable has not been built yet.'}
$LogDirectory=Join-Path $Root 'build\diagnostics'
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
# Language: settings.ini language= (F2 menu), English by default (main.cpp).
$RunName=Get-Date -Format 'yyyyMMdd-HHmmss'
$env:RBREW_STUB_REPORT=Join-Path $LogDirectory ($RunName+'-stubs.txt')
$Process=Start-Process -FilePath $Executable -WorkingDirectory $Game -WindowStyle Normal -RedirectStandardOutput (Join-Path $LogDirectory ($RunName+'-stdout.log')) -RedirectStandardError (Join-Path $LogDirectory ($RunName+'-stderr.log')) -PassThru -Wait
$Process.ExitCode | Set-Content -LiteralPath (Join-Path $LogDirectory ($RunName+'-exit.txt'))
