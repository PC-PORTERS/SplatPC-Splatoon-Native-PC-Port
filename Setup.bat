@echo off
title Splatoon 1 PC Port - Setup
cd /d "%~dp0"
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0scripts\Setup.ps1" %*
echo.
pause
