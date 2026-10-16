@echo off
rem Build the game (nsmbw.exe) and put it in ..\game. Double-click this file, or run it
rem from a command prompt (options: --clean, --jobs N; see build.py). It first asks for the
rem version number (Enter keeps it, + the next one); afterwards, when that is higher than the
rem update server's, it offers to publish the build (docs\UPDATES.md).
cd /d "%~dp0"
set PY=
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY goto nopython
%PY% -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul || goto nopython
%PY% -c "import ziglang" >nul 2>nul
if errorlevel 1 (
  echo Installing zig ^(the C compiler, as a Python package^)...
  %PY% -m pip install --user ziglang
  if errorlevel 1 goto end
)
%PY% -c "import PIL" >nul 2>nul || %PY% -m pip install --user pillow
%PY% build.py --ask-version --ask-publish %*
goto end
:nopython
echo Python 3.8 or newer is needed: https://www.python.org/downloads/
echo ^(in the installer, tick "Add python.exe to PATH"^), then run this again.
:end
echo.
pause
