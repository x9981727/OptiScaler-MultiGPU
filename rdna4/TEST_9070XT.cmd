@echo off
setlocal
cd /d "%~dp0"
echo DLSSNR-RDNA4-Core r1: standalone operator test, NOT a game DLL.
echo Please close games and other GPU-heavy programs before running.
echo No game files or registry settings will be modified.
"%~dp0rdna4-test.exe" --output "%~dp0rdna4-result.json" > "%~dp0rdna4-console.txt" 2>&1
set "code=%errorlevel%"
type "%~dp0rdna4-console.txt"
echo.
echo Exit code: %code%
echo Results: rdna4-result.json and rdna4-console.txt
echo These are OPERATOR results, not full-model or game performance.
pause
exit /b %code%
