@echo off
setlocal
cd /d "%~dp0"
echo RDNA4 r6.2 - Neural-Contribution Lab. NOT a game DLL.
echo Close games first. Original files are READ ONLY.
echo Select the existing checkpoint ZIP, AMD NR DLL and weights BIN.
echo This test explicitly varies ONE final scalar in process memory, then restores it.
echo It does NOT install, enable or change game settings.
if not exist "%~dp0rdna4-r62-test.exe" (
 echo ERROR: Extract the complete ZIP to a new writable folder.
 pause
 exit /b 2
)
"%~dp0rdna4-r62-test.exe" --output "%~dp0rdna4-r62-result.json"
set "code=%errorlevel%"
echo.
echo Exit code: %code%
echo Keep rdna4-r62-result.json and rdna4-r62-console.txt, including on failure.
pause
exit /b %code%
