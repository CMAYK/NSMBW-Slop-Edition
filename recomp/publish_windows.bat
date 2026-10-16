@echo off
rem Publish the game in ..\game as an update (docs\UPDATES.md): makes the zipped build and puts it
rem on the server in update_url.txt. Raise the number in VERSION and build first.
rem   publish_windows.bat           publish (asks what changed and the copyparty password)
rem   publish_windows.bat keygen    make your signing key (once, before the first build)
cd /d "%~dp0"
set PY=
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY (
  echo Python 3.8 or newer is needed: https://www.python.org/downloads/
  goto end
)
if "%~1"=="" (%PY% tools\publish_update.py publish) else (%PY% tools\publish_update.py %*)
:end
echo.
pause
