$ErrorActionPreference = 'Stop'

# Keep the v3 adapter override, but do NOT use the broken v4 cross-TU bypass.
& "$env:GITHUB_WORKSPACE\scripts\apply-v3.ps1"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$fsr4Cpp = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\fsr4\FSR4Upgrade.cpp'
$fsrfg = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\framegen\ffx\FSRFG_Dx12.cpp'

# 1) When Fsr4Update is temporarily disabled during MultiGPU secondary FG context creation,
#    block IAmdExtFfxApi instead of forwarding to the globally hooked RDNA4 driver entry point.
$cpp = [IO.File]::ReadAllText($fsr4Cpp)
$pattern = 'if \(!Config::Instance\(\)->Fsr4Update\.value_or_default\(\) && o_AmdExtD3DCreateInterface != nullptr\)\s*return o_AmdExtD3DCreateInterface\(pOuter, riid, ppvObject\);'
$replacement = @'
if (!Config::Instance()->Fsr4Update.value_or_default() && o_AmdExtD3DCreateInterface != nullptr)
    {
        if (Config::Instance()->FGMultiGPUEnabled.value_or_default() && riid == __uuidof(IAmdExtFfxApi))
        {
            LOG_INFO("MultiGPU v5: blocking IAmdExtFfxApi while Fsr4Update is temporarily disabled");
            return E_NOINTERFACE;
        }

        return o_AmdExtD3DCreateInterface(pOuter, riid, ppvObject);
    }
'@
$newCpp = [regex]::Replace($cpp, $pattern, $replacement, 1)
if ($newCpp -eq $cpp) { throw 'v5 FSR4Upgrade hook replacement did not match' }
[IO.File]::WriteAllText($fsr4Cpp, $newCpp, [Text.UTF8Encoding]::new($false))

# 2) Scope Fsr4Update=false to only the secondary compute-context create call.
$f = [IO.File]::ReadAllText($fsrfg)
if (-not $f.Contains('#include <Config.h>')) {
    $firstInclude = '#include "FSRFG_Dx12.h"'
    if (-not $f.Contains($firstInclude)) { throw 'FSRFG_Dx12 include insertion point not found' }
    $f = $f.Replace($firstInclude, "$firstInclude`n#include <Config.h>")
}

$armNeedle = 'D3D12Hooks::SetMultiGPUFGAdapterOverride(computeDevice);'
$disarmNeedle = 'D3D12Hooks::SetMultiGPUFGAdapterOverride(nullptr);'
if (-not $f.Contains($armNeedle)) { throw 'v3 adapter arm line not found' }
if (-not $f.Contains($disarmNeedle)) { throw 'v3 adapter disarm line not found' }

if (-not $f.Contains('MultiGPU v5: temporarily disabling Fsr4Update')) {
    $armReplacement = @(
        'const bool multiGpuFsr4Restore = Config::Instance()->Fsr4Update.value_or_default();',
        '        Config::Instance()->Fsr4Update.set_volatile_value(false);',
        '        LOG_INFO("MultiGPU v5: temporarily disabling Fsr4Update for secondary FG context");',
        '        D3D12Hooks::SetMultiGPUFGAdapterOverride(computeDevice);'
    ) -join "`n"
    $f = $f.Replace($armNeedle, $armReplacement)
}

if (-not $f.Contains('MultiGPU v5: restored Fsr4Update')) {
    $disarmReplacement = @(
        'D3D12Hooks::SetMultiGPUFGAdapterOverride(nullptr);',
        '        Config::Instance()->Fsr4Update.set_volatile_value(multiGpuFsr4Restore);',
        '        LOG_INFO("MultiGPU v5: restored Fsr4Update to {}", multiGpuFsr4Restore);'
    ) -join "`n"
    $f = $f.Replace($disarmNeedle, $disarmReplacement)
}
[IO.File]::WriteAllText($fsrfg, $f, [Text.UTF8Encoding]::new($false))

# Source-level validation.
$checkCpp = [IO.File]::ReadAllText($fsr4Cpp)
$checkF = [IO.File]::ReadAllText($fsrfg)
if (-not $checkCpp.Contains('MultiGPU v5: blocking IAmdExtFfxApi')) { throw 'v5 FSR4 hook marker missing' }
if (-not $checkF.Contains('MultiGPU v5: temporarily disabling Fsr4Update')) { throw 'v5 FSRFG disable marker missing' }
if (-not $checkF.Contains('MultiGPU v5: restored Fsr4Update')) { throw 'v5 FSRFG restore marker missing' }

Write-Host 'v5 scoped FSR4 provider isolation injected successfully.'
