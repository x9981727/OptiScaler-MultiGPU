@echo off
setlocal
cd /d "%~dp0"
echo RDNA4 r5.1 - Full-Plan Differential Test. NOT a game DLL.
echo Close games and other GPU-heavy programs first.
echo Select the existing CHECKPOINT ZIP, then AMD NR runtime DLL, then weights BIN.
echo All three original files are read-only. Do not overwrite game files.
echo Progress will appear here. Results are saved automatically.
if not exist "%~dp0rdna4-r5-test.exe" (
  echo ERROR: Extract the complete ZIP before running TEST_R5.cmd.
  pause
  exit /b 2
)
"%~dp0rdna4-r5-test.exe" --output "%~dp0rdna4-r5-result.json"
set "result=%errorlevel%"
echo.
echo Exit code: %result%
echo Keep rdna4-r5-result.json and rdna4-r5-console.txt, including on failure.
echo This test does not install or enable a replacement game DLL.
pause
exit /b %result%
