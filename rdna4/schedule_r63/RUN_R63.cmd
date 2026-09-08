@echo off
setlocal
cd /d "%~dp0"
echo RDNA4 r6.3 - Swin Scheduling Candidate Test. NOT a game DLL.
echo Extract the whole ZIP to a new writable folder. Close games before testing.
echo Select your existing checkpoint ZIP, original AMD NR DLL, then weights BIN.
echo Original files are read-only. Experimental gain=1 is used equally in all timed modes.
echo This test does not install a game DLL or change clocks, voltages or driver settings.
if not exist "%~dp0rdna4-swin-r63.exe" (
  echo ERROR: rdna4-swin-r63.exe is missing. Extract the complete ZIP.
  pause
  exit /b 2
)
"%~dp0rdna4-swin-r63.exe" --output "%~dp0rdna4-r63-result.json" %*
set "rc=%errorlevel%"
echo.
echo Exit code: %rc%
echo Keep rdna4-r63-result.json and rdna4-r63-console.txt, also on failure.
echo A completed test does not mean a candidate is faster or approved for games.
pause
exit /b %rc%
