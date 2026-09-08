@echo off
setlocal
cd /d "%~dp0"
echo RDNA4 r6.2 - Explicit terminal-control experiments. NOT a game DLL.
echo Close games and other GPU-heavy programs first.
echo Select your existing checkpoint ZIP, runtime DLL, and weights BIN.
echo Original files are read-only. The nonzero control values are diagnostic only.
if not exist "%~dp0rdna4-autolab.exe" (
 echo ERROR: Extract the entire ZIP to a writable folder first.
 pause
 exit /b 2
)
"%~dp0rdna4-autolab.exe" --output "%~dp0rdna4-r62-result.json"
set "code=%errorlevel%"
echo.
echo Exit code: %code%
echo Keep rdna4-r62-result.json and rdna4-r62-console.txt, including on failure.
echo A completed diagnostic does NOT approve game deployment.
pause
exit /b %code%
