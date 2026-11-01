@echo off
title Splatoon 1 PC Port - Online
rem Starts the game without the shortcut: the sign-in screen (pseudo, language), then the online game.
rem Same thing the "Splatoon PC Port" shortcut does. Put it anywhere in this folder's reach; it finds its own files.
cd /d "%~dp0"
if not exist "Game\Splatoon-v288.exe" (
  echo The game is not installed yet: run Setup.bat first.
  echo Le jeu n'est pas encore installe : lance d'abord Setup.bat.
  pause
  exit /b 1
)
start "" /d "%~dp0Game" powershell -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0scripts\Splatoon-Login.ps1"
