@echo off
setlocal
cd /d "%~dp0"
echo RDNA4 r5: fixed full Forward plan differential test. NOT a game DLL.
echo Close games and other GPU-heavy programs first.
echo File dialogs may ask for:
echo   1. DLSSNR-rewrite-source-checkpoint.zip, or its plans/1080p.json.
echo   2. Existing dlssnr_amd_pass1.dll or original version.dll.
echo   3. Existing dlssnr_on_amd_weights.bin.
echo The checkpoint is NOT the r4 test ZIP. No Python is required.
echo Original files are read only. Do not extract this test into a game folder.
"%~dp0rdna4-r5-test.exe" --output "%~dp0rdna4-r5-result.json" > "%~dp0rdna4-r5-console.txt" 2>&1
set "code=%errorlevel%"
type "%~dp0rdna4-r5-console.txt"
echo.
echo Exit code: %code%
echo Keep rdna4-r5-result.json and rdna4-r5-console.txt, including on failure.
echo No game installation or settings were changed.
pause
exit /b %code%
