@echo off
setlocal
cd /d "%~dp0"
echo RDNA4 r6 AutoLab - validation environment, NOT a final game DLL.
echo Close games before the GPU suite. Do not change overclock settings for this test.
echo Existing checkpoint, original runtime DLL and weights are read-only.
echo No Python, administrator rights or registry changes are required.
if not exist "rdna4-autolab.exe" (
  echo ERROR: Extract the complete ZIP first.
  pause
  exit /b 2
)
"%~dp0rdna4-autolab.exe" --output "%~dp0rdna4-r6-result.json" %*
set "rc=%errorlevel%"
echo.
echo Exit code: %rc%
echo Reports: rdna4-r6-result.json and rdna4-r6-console.txt
echo An output-sensitivity block is NOT release approval or convergence.
pause
exit /b %rc%
