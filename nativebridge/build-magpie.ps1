param([Parameter(Mandatory=$true)][string]$MagpieRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=$PSScriptRoot
$reports=Join-Path $root 'reports'
$dist=Join-Path $root 'dist'
Push-Location $MagpieRoot
try {
    python scripts/publish.py --compiler=MSVC --platform=x64 2>&1 | Tee-Object (Join-Path $reports 'magpie-build.txt')
    if($LASTEXITCODE){throw "Patched Magpie publish build failed: $LASTEXITCODE"}
    $publish=Join-Path $MagpieRoot 'publish/x64'
    if(!(Test-Path (Join-Path $publish 'Magpie.exe'))){throw 'Patched Magpie.exe missing after successful build'}
    $target=Join-Path $dist 'Magpie-NativeBridge-x64'
    if(Test-Path $target){Remove-Item $target -Recurse -Force}
    Copy-Item $publish $target -Recurse
    Get-FileHash (Join-Path $target 'Magpie.exe') -Algorithm SHA256 |
        Format-List | Out-String | Set-Content (Join-Path $reports 'magpie-exe-sha256.txt')
    @(
      'Magpie NativeBridge consumer: COMPILED on Windows CI',
      'NativeBridge D3D transport/self-tests: see windows-tests.txt',
      'Game-side OptiScaler producer: NOT YET INSTALLED',
      'AMD+NVIDIA dual-GPU runtime: NOT YET TESTED'
    ) | Set-Content (Join-Path $target 'NATIVEBRIDGE_STATUS.txt')
} finally {
    Pop-Location
}
