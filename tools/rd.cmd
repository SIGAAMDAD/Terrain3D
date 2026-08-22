@echo off
setlocal
set "PYTHONUTF8=1"
if not defined RD_GODOT_ROOT if exist "D:\Godot_v4.7.1-stable_mono_win64" set "RD_GODOT_ROOT=D:\Godot_v4.7.1-stable_mono_win64"
where py >nul 2>nul
if %errorlevel%==0 (
  py -3 "%~dp0rd.py" %*
  exit /b %errorlevel%
)
where python >nul 2>nul
if %errorlevel%==0 (
  python "%~dp0rd.py" %*
  exit /b %errorlevel%
)
echo FAIL Python 3 not found. Install Python 3 or add py/python to PATH.
exit /b 1
