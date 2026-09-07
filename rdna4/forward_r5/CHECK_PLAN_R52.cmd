@echo off
setlocal
cd /d "%~dp0"
echo RDNA4 r5.2 - OPTIONAL CPU-ONLY CHECKPOINT VALIDATION
echo This command does not initialize a GPU or read model weights.
echo Select your original checkpoint ZIP or its plans/1080p.json.
"%~dp0rdna4-r5-test.exe" --validate-plan-only --output "%~dp0rdna4-plan-check.json"
set "rc=%errorlevel%"
echo Exit code: %rc%
echo For the actual GPU comparison, run TEST_R5.cmd instead.
pause
exit /b %rc%
