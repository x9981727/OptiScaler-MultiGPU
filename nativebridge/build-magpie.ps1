param(
    [Parameter(Mandatory=$true)][string]$MagpieRoot,
    [Parameter(Mandatory=$true)][string]$XeSSSdkDir
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=$PSScriptRoot
$reports=Join-Path $root 'reports'
$dist=Join-Path $root 'dist'

$requiredSdk=@(
    (Join-Path $XeSSSdkDir 'inc/xess_fg/xefg_swapchain_d3d12.h'),
    (Join-Path $XeSSSdkDir 'lib/libxess_fg.lib'),
    (Join-Path $XeSSSdkDir 'lib/libxell.lib'),
    (Join-Path $XeSSSdkDir 'bin/libxess_fg.dll'),
    (Join-Path $XeSSSdkDir 'bin/libxell.dll')
)
foreach($p in $requiredSdk){if(!(Test-Path $p)){throw "Pinned XeSSFG SDK file missing: $p"}}

# Magpie defaults this experimental feature off. CI must explicitly compile the
# real XeSS-FG branch; a successful stub-only build is not acceptable evidence.
$sdkXml=[System.Security.SecurityElement]::Escape((Resolve-Path $XeSSSdkDir).Path)
$userProps=@"
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <EnableXeSSFrameGeneration>true</EnableXeSSFrameGeneration>
    <XeSSSdkDir>$sdkXml</XeSSSdkDir>
  </PropertyGroup>
</Project>
"@
[IO.File]::WriteAllText((Join-Path $MagpieRoot 'src/BuildOptions.props.user'),$userProps)

Push-Location $MagpieRoot
try {
    python scripts/publish.py --compiler=MSVC --platform=x64 2>&1 | Tee-Object (Join-Path $reports 'magpie-build.txt')
    if($LASTEXITCODE){throw "Patched Magpie XeSSFG-enabled publish build failed: $LASTEXITCODE"}
    $publish=Join-Path $MagpieRoot 'publish/x64'
    foreach($name in @('Magpie.exe','libxess.dll','libxess_fg.dll','libxell.dll')){
        if(!(Test-Path (Join-Path $publish $name))){throw "XeSSFG-enabled Magpie output missing: $name"}
    }

    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if(!(Test-Path $vswhere)){throw 'vswhere.exe not found for Magpie import verification'}
    $dumpbin=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
    if(!$dumpbin -or !(Test-Path $dumpbin)){throw 'dumpbin.exe not found for Magpie import verification'}
    $imports=(& $dumpbin /imports (Join-Path $publish 'Magpie.exe') 2>&1 | Out-String)
    $imports | Set-Content (Join-Path $reports 'magpie-imports.txt')
    if($imports -notmatch '(?i)libxess_fg\.dll'){throw 'Magpie.exe does not import libxess_fg.dll; XeSSFG branch was not linked'}
    if($imports -notmatch '(?i)libxell\.dll'){throw 'Magpie.exe does not import libxell.dll; XeLL branch was not linked'}

    $target=Join-Path $dist 'Magpie-NativeBridge-x64'
    if(Test-Path $target){Remove-Item $target -Recurse -Force}
    Copy-Item $publish $target -Recurse
    Get-FileHash (Join-Path $target 'Magpie.exe') -Algorithm SHA256 |
        Format-List | Out-String | Set-Content (Join-Path $reports 'magpie-exe-sha256.txt')
    @(
      'Magpie NativeBridge consumer: COMPILED on Windows CI',
      'Intel XeSS-FG SDK branch: COMPILED AND LINKED (imports verified)',
      'NativeBridge native Depth + Motion + Camera path: COMPILED',
      'Game-side patched OptiScaler producer: compiled later in the same CI gate',
      'AMD+NVIDIA dual-GPU game runtime: NOT YET TESTED'
    ) | Set-Content (Join-Path $target 'NATIVEBRIDGE_STATUS.txt')
} finally {
    Pop-Location
}
