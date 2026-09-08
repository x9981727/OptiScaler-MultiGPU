param([Parameter(Mandatory=$true)][string]$MagpieRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root=$PSScriptRoot
$reports=Join-Path $root 'reports'
New-Item -ItemType Directory -Force $reports | Out-Null
python (Join-Path $root 'unpack.py')
if ($LASTEXITCODE) {throw 'Source unpack failed'}
$source=Join-Path $root 'source'
$build=Join-Path $root 'build'
cmake -S $source -B $build -A x64 2>&1 | Tee-Object (Join-Path $reports 'configure.txt')
if ($LASTEXITCODE) {throw 'CMake configuration failed'}
cmake --build $build --config Release --parallel 2 2>&1 | Tee-Object (Join-Path $reports 'windows-build.txt')
if ($LASTEXITCODE) {throw 'Windows build failed'}
ctest --test-dir $build -C Release --output-on-failure 2>&1 | Tee-Object (Join-Path $reports 'windows-tests.txt')
if ($LASTEXITCODE) {throw 'Windows tests failed'}
& (Join-Path $build 'Release/native_bridge_adapters.exe') 2>&1 | Tee-Object (Join-Path $reports 'runner-adapters.txt')
python (Join-Path $source 'tools/apply_magpie.py') $MagpieRoot --apply 2>&1 | Tee-Object (Join-Path $reports 'magpie-patch.txt')
if ($LASTEXITCODE) {throw 'Pinned Magpie patch failed'}
$dist=Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item (Join-Path $build 'Release/*.exe') $dist
Copy-Item (Join-Path $build 'Release/*.lib') $dist
'Status: compiled core/transport/IPC diagnostics, NOT a complete game runtime.' | Set-Content (Join-Path $dist 'STATUS.txt')
