@echo off
title Splatoon 1 PC Port - Setup
cd /d "%~dp0"
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0scripts\Setup.ps1" %*
rem Stop here when the base install failed or asked to be restarted (exit code 1 or 3).
if errorlevel 1 goto done
echo.
echo Base install complete. Choose the revision 288 update dump in the next window.
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0scripts\Install-Update.ps1"
:done
echo.
pause
