@echo off
setlocal
cd /d "%~dp0"
echo RDNA4 r4: ORIGINAL HEAD vs REWRITTEN HEAD. NOT a game DLL.
echo Close games and other GPU-heavy programs first.
echo Two file-selection windows may open:
echo   1. Select your existing dlssnr_amd_pass1.dll or original version.dll.
echo   2. Select your existing dlssnr_on_amd_weights.bin.
echo Files are read only. Nothing needs to be copied into a game folder.
"%~dp0rdna4-r4-test.exe" --output "%~dp0rdna4-r4-result.json" > "%~dp0rdna4-r4-console.txt" 2>&1
set "code=%errorlevel%"
type "%~dp0rdna4-r4-console.txt"
echo.
echo Exit code: %code%
echo Send rdna4-r4-result.json and rdna4-r4-console.txt, including on failure.
echo This is one internal layer, not full DLSS-NR or game performance.
pause
exit /b %code%
