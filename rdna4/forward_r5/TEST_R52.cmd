@echo off
setlocal
cd /d "%~dp0"
echo RDNA4 r5.2 - Special Swin Boundary Parser Fix
echo NOT a game DLL. Close games before running this independent test.
echo Use the SAME original checkpoint ZIP, runtime DLL and weights BIN.
echo Original files are read-only; no driver reinstall or Python required.
if not exist "%~dp0rdna4-r5-test.exe" (
  echo ERROR: Extract the complete r5.2 ZIP into a new writable folder.
  pause
  exit /b 2
)
"%~dp0rdna4-r5-test.exe" --output "%~dp0rdna4-r5-result.json"
set "rc=%errorlevel%"
echo.
echo Exit code: %rc%
echo Send rdna4-r5-result.json and rdna4-r5-console.txt, including on failure.
pause
exit /b %rc%
