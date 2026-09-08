param([Parameter(Mandatory=$true)][string]$MagpieRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=$PSScriptRoot
$reports=Join-Path $root 'reports'
$dist=Join-Path $root 'dist'
New-Item -ItemType Directory -Force $reports,$dist | Out-Null
python (Join-Path $root 'unpack.py')
if ($LASTEXITCODE) {throw 'Source unpack failed'}
$source=Join-Path $root 'source'
if(Test-Path (Join-Path $root 'overlay')) {Copy-Item (Join-Path $root 'overlay/*') $source -Recurse -Force}
# Make the previously implicit standard-header dependencies explicit.
foreach($relative in @('tests/core_tests.cpp','tests/magpie_guidance_tests.cpp','tests/streamline_camera_tests.cpp','tests/wire_tests.cpp','windows/ipc_selftest.cpp')) {
 $p=Join-Path $source $relative
 $s=[IO.File]::ReadAllText($p)
 if(!$s.Contains('#include <string>')) {[IO.File]::WriteAllText($p,"#include <string>`n"+$s)}
}
$codeExport=@'
import sys,pathlib,zipfile
root=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2])
allowed={'.cpp','.c','.h','.hpp','.inl','.idl','.xaml','.props','.targets','.vcxproj','.py','.ps1','.json','.md','.yml','.hlsl','.txt','.slnx'}
with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
 for p in root.rglob('*'):
  if p.is_file() and '.git' not in p.parts and p.suffix.lower() in allowed:z.write(p,p.relative_to(root))
'@
$exporter=Join-Path $reports 'export-code.py'
[IO.File]::WriteAllText($exporter,$codeExport)
python $exporter $source (Join-Path $dist 'expanded-NativeBridge-source-for-review.zip')
if($LASTEXITCODE){throw 'NativeBridge source export failed'}
python $exporter $MagpieRoot (Join-Path $dist 'pinned-Magpie-code-for-review.zip')
if($LASTEXITCODE){throw 'Source export failed'}
$op=Join-Path $root '_optiscaler'
git clone --no-checkout https://github.com/optiscaler/OptiScaler.git $op
if(!$LASTEXITCODE){
 git -C $op checkout da70e61e1542a0b99adcb24168ff941e42109567
 if(!$LASTEXITCODE){python $exporter $op (Join-Path $dist 'pinned-OptiScaler-code-for-review.zip')}
}
$build=Join-Path $root 'build'
cmake -S $source -B $build -A x64 '-DCMAKE_CXX_FLAGS=/utf-8 /EHsc' 2>&1 | Tee-Object (Join-Path $reports 'configure.txt')
if ($LASTEXITCODE) {throw 'CMake configuration failed'}
cmake --build $build --config Release --parallel 2 2>&1 | Tee-Object (Join-Path $reports 'windows-build.txt')
$buildCode=$LASTEXITCODE
if(Test-Path (Join-Path $build 'Release')) {
 Get-ChildItem (Join-Path $build 'Release') -File | Where-Object {$_.Extension -in @('.exe','.lib','.dll')} | Copy-Item -Destination $dist
}
'Status: compiled components and diagnostics only; no complete game runtime claimed.' | Set-Content (Join-Path $dist 'STATUS.txt')
if ($buildCode) {throw 'Windows build failed'}
ctest --test-dir $build -C Release --output-on-failure 2>&1 | Tee-Object (Join-Path $reports 'windows-tests.txt')
$testCode=$LASTEXITCODE
& (Join-Path $build 'Release/native_bridge_adapters.exe') 2>&1 | Tee-Object (Join-Path $reports 'runner-adapters.txt')
python (Join-Path $source 'tools/apply_magpie.py') $MagpieRoot --apply 2>&1 | Tee-Object (Join-Path $reports 'magpie-patch.txt')
$patchCode=$LASTEXITCODE
if($testCode){throw 'Windows tests failed'}
if($patchCode){throw 'Pinned Magpie patch failed'}
if(Test-Path (Join-Path $root 'build-magpie.ps1')) {& (Join-Path $root 'build-magpie.ps1') -MagpieRoot $MagpieRoot; if($LASTEXITCODE){throw 'Magpie build failed'}}
