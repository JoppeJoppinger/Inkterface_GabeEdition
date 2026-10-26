@echo off
rem Double-click installer for Windows. Passes all arguments to install.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
