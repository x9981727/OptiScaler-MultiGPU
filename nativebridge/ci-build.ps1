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
if(Test-Path (Join-Path $root 'integrations')) {Copy-Item (Join-Path $root 'integrations') $source -Recurse -Force}
foreach($relative in @('tests/core_tests.cpp','tests/magpie_guidance_tests.cpp','tests/streamline_camera_tests.cpp','tests/wire_tests.cpp','windows/ipc_selftest.cpp')) {
 $p=Join-Path $source $relative
 $s=[IO.File]::ReadAllText($p)
 if(!$s.Contains('#include <string>')) {[IO.File]::WriteAllText($p,"#include <string>`n"+$s)}
}
$codeExport=@'
import sys,pathlib,zipfile
root=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2])
allowed={'.cpp','.c','.h','.hpp','.inl','.idl','.xaml','.props','.targets','.vcxproj','.py','.ps1','.json','.md','.yml','.hlsl','.txt','.sln','.slnx'}
with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
 for p in root.rglob('*'):
  if p.is_file() and '.git' not in p.parts and p.suffix.lower() in allowed:z.write(p,p.relative_to(root))
'@
$exporter=Join-Path $reports 'export-code.py'
[IO.File]::WriteAllText($exporter,$codeExport)
python $exporter $source (Join-Path $dist 'expanded-NativeBridge-source-for-review.zip')
if($LASTEXITCODE){throw 'NativeBridge source export failed'}
python $exporter $MagpieRoot (Join-Path $dist 'pinned-Magpie-code-for-review.zip')
if($LASTEXITCODE){throw 'Magpie source export failed'}

$op=Join-Path $root '_optiscaler'
if(Test-Path $op){Remove-Item $op -Recurse -Force}
git clone --no-checkout https://github.com/optiscaler/OptiScaler.git $op
if($LASTEXITCODE){throw 'OptiScaler clone failed'}
git -C $op checkout da70e61e1542a0b99adcb24168ff941e42109567
if($LASTEXITCODE){throw 'Pinned OptiScaler checkout failed'}
python $exporter $op (Join-Path $dist 'pinned-OptiScaler-code-for-review.zip')
if($LASTEXITCODE){throw 'Pinned OptiScaler source export failed'}
python (Join-Path $source 'tools/apply_optiscaler_r2.py') $op --apply 2>&1 | Tee-Object (Join-Path $reports 'optiscaler-patch-r2.txt')
$opPatchCode=$LASTEXITCODE
if(!$opPatchCode){
 python $exporter $op (Join-Path $dist 'patched-OptiScaler-NativeBridge-code-for-review.zip')
 if($LASTEXITCODE){throw 'Patched OptiScaler source export failed'}
}

$build=Join-Path $root 'build'
cmake -S $source -B $build -A x64 '-DCMAKE_CXX_FLAGS=/utf-8 /EHsc' 2>&1 | Tee-Object (Join-Path $reports 'configure.txt')
if ($LASTEXITCODE) {throw 'CMake configuration failed'}
cmake --build $build --config Release --parallel 2 2>&1 | Tee-Object (Join-Path $reports 'windows-build.txt')
$buildCode=$LASTEXITCODE
if(Test-Path (Join-Path $build 'Release')) {
 Get-ChildItem (Join-Path $build 'Release') -File | Where-Object {$_.Extension -in @('.exe','.lib','.dll')} | Copy-Item -Destination $dist
}
'Status: NativeBridge transport/components compiled; patched Magpie and OptiScaler full builds follow.' | Set-Content (Join-Path $dist 'STATUS.txt')
if ($buildCode) {throw 'Windows build failed'}
ctest --test-dir $build -C Release --output-on-failure 2>&1 | Tee-Object (Join-Path $reports 'windows-tests.txt')
$testCode=$LASTEXITCODE
& (Join-Path $build 'Release/native_bridge_adapters.exe') 2>&1 | Tee-Object (Join-Path $reports 'runner-adapters.txt')

python (Join-Path $source 'tools/apply_magpie.py') $MagpieRoot --apply 2>&1 | Tee-Object (Join-Path $reports 'magpie-patch-r1.txt')
$r1PatchCode=$LASTEXITCODE
if(!$r1PatchCode){
 python (Join-Path $source 'tools/apply_magpie_r2.py') $MagpieRoot --apply 2>&1 | Tee-Object (Join-Path $reports 'magpie-patch-r2.txt')
 $r2PatchCode=$LASTEXITCODE
} else {$r2PatchCode=1}
if(!$r1PatchCode -and !$r2PatchCode){
 python (Join-Path $root 'integrations/magpie/apply_xessfg_native_r3.py') $MagpieRoot 2>&1 |
  Tee-Object (Join-Path $reports 'magpie-xessfg-native-r3.txt')
 $r3PatchCode=$LASTEXITCODE
} else {$r3PatchCode=1}
if(!$r1PatchCode -and !$r2PatchCode -and !$r3PatchCode){
 python (Join-Path $source 'tools/apply_magpie_dlssfg_r4.py') $MagpieRoot --apply 2>&1 |
  Tee-Object (Join-Path $reports 'magpie-dlssfg-native-r4.txt')
 $r4PatchCode=$LASTEXITCODE
} else {$r4PatchCode=1}
if(!$r1PatchCode -and !$r2PatchCode -and !$r3PatchCode -and !$r4PatchCode){
 python $exporter $MagpieRoot (Join-Path $dist 'patched-Magpie-NativeBridge-code-for-review.zip')
 if($LASTEXITCODE){throw 'Patched Magpie source export failed'}
}
if($testCode){throw 'Windows tests failed'}
if($opPatchCode){throw 'Pinned OptiScaler producer patch failed'}
if($r1PatchCode){throw 'Pinned Magpie guidance patch failed'}
if($r2PatchCode){throw 'Pinned Magpie runtime patch failed'}
if($r3PatchCode){throw 'Pinned Magpie XeSSFG native guidance patch failed'}
if($r4PatchCode){throw 'Pinned Magpie DLSSFG native constants patch failed'}

git -C $op submodule update --init --depth 1 external/xess 2>&1 | Tee-Object (Join-Path $reports 'magpie-xess-submodule.txt')
if($LASTEXITCODE){throw 'Pinned XeSS SDK submodule initialization failed'}
$xessSdk=Join-Path $op 'external/xess'
if(Test-Path (Join-Path $root 'build-magpie.ps1')) {
 & (Join-Path $root 'build-magpie.ps1') -MagpieRoot $MagpieRoot -XeSSSdkDir $xessSdk
 if($LASTEXITCODE){throw 'XeSSFG-enabled Magpie build failed'}
}

git -C $op submodule update --init --recursive --depth 1 2>&1 | Tee-Object (Join-Path $reports 'optiscaler-submodules.txt')
if($LASTEXITCODE){throw 'OptiScaler submodule initialization failed'}
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if(!(Test-Path $vswhere)){throw 'vswhere.exe not found'}
$vsPath=& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if(!$vsPath){throw 'Visual Studio installation with MSBuild not found'}
$msbuild=Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if(!(Test-Path $msbuild)){throw "MSBuild.exe not found at $msbuild"}
& $msbuild (Join-Path $op 'OptiScaler.sln') /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal 2>&1 |
 Tee-Object (Join-Path $reports 'optiscaler-build.txt')
if($LASTEXITCODE){throw 'OptiScaler NativeBridge build failed'}
$opOut=Join-Path $op 'x64/Release'
if(Test-Path $opOut){
 Get-ChildItem $opOut -Recurse -File | Where-Object {$_.Extension -in @('.dll','.pdb','.ini','.bat')} |
  Copy-Item -Destination $dist -Force
}
'PASS: NativeBridge 9/9 + Magpie real XeSSFG SDK compile/link + native Depth/Motion/Camera r3 + exact DLSSFG native constants r4 + patched OptiScaler producer build. Real AMD/NVIDIA dual-GPU game validation is separate.' |
 Set-Content (Join-Path $dist 'BUILD_STATUS.txt')