@echo off
setlocal
cd /d "%~dp0"
"%~dp0tools\AutoPacing.exe" --seconds 180
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%
