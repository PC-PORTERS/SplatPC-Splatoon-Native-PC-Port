@echo off
setlocal
title Splatoon 1 PC Port - Update
rem Manual update for players who cloned the repository: fetches the newest version of the branch
rem this folder is on, then runs Setup.bat. It never resets, cleans or stashes anything, and never
rem touches Game\save or the online identity: if the update cannot be applied cleanly it stops.
cd /d "%~dp0"
set "RC=0"

where git >nul 2>&1
if errorlevel 1 (
  echo Git is not installed or not in the PATH. Install it from https://git-scm.com/ and try again.
  set "RC=1"
  goto :end
)

set "PREFIX="
git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 goto :notrepo
for /f "delims=" %%i in ('git rev-parse --show-prefix') do set "PREFIX=%%i"
if defined PREFIX goto :notrepo

git symbolic-ref -q HEAD >nul 2>&1
if errorlevel 1 (
  echo This copy is not on a branch, so it cannot be updated automatically.
  set "RC=1"
  goto :end
)
set "UPSTREAM="
for /f "delims=" %%i in ('git rev-parse --abbrev-ref --symbolic-full-name @{u} 2^>nul') do set "UPSTREAM=%%i"
if not defined UPSTREAM (
  echo This branch does not follow an online branch, so it cannot be updated automatically.
  set "RC=1"
  goto :end
)

echo Checking for a new version of %UPSTREAM% ...
git fetch --quiet
if errorlevel 1 (
  echo Could not reach the server. Check your internet connection and try again.
  set "RC=1"
  goto :end
)

set "BEHIND=0"
for /f "delims=" %%i in ('git rev-list --count HEAD..@{u}') do set "BEHIND=%%i"
if "%BEHIND%"=="0" (
  echo Already up to date.
  goto :setup
)

for /f "delims=" %%i in ('git rev-parse --short HEAD') do set "OLD=%%i"
git -c advice.diverging=false merge --ff-only --quiet @{u}
if errorlevel 1 (
  echo.
  echo The update was NOT applied and nothing was changed or deleted.
  echo Either you changed files that the new version also changes, or your copy
  echo has commits that the online branch does not have.
  echo Move your changes elsewhere, or ask for help, then run update.bat again.
  set "RC=1"
  goto :end
)
for /f "delims=" %%i in ('git rev-parse --short HEAD') do set "NEW=%%i"
echo Updated %OLD% to %NEW%.

:setup
if not exist "%~dp0Setup.bat" (
  echo Setup.bat is missing from this folder.
  set "RC=1"
  goto :end
)
echo.
echo Running Setup.bat ...
call "%~dp0Setup.bat"
set "RC=%errorlevel%"
goto :end

:notrepo
echo This folder is not a git repository, so it cannot be updated here.
echo Clone the project with git, or download the new version again.
set "RC=1"

:end
echo.
if not "%RC%"=="0" echo Update stopped with code %RC%.
pause
exit /b %RC%
