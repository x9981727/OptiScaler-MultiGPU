@echo off
setlocal
cd /d "%~dp0"
echo RDNA4 r6.1 - Timestamp repair and final-boundary probes.
echo Close games and other GPU-heavy applications first.
echo Select your EXISTING checkpoint ZIP, original runtime DLL, then weights BIN.
echo No Python, no administrator rights, and no game file replacement.
if not exist "%~dp0rdna4-autolab.exe" (
  echo ERROR: Extract the complete ZIP into a new writable folder first.
  pause
  exit /b 2
)
"%~dp0rdna4-autolab.exe" --output "%~dp0rdna4-r61-result.json"
set "result=%errorlevel%"
echo.
echo Exit code: %result%
echo Keep rdna4-r61-result.json and rdna4-r61-console.txt, even on failure.
echo This is a diagnostic test, NOT a replacement game DLL.
pause
exit /b %result%
