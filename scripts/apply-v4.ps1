$ErrorActionPreference = 'Stop'

# Keep all v3 behavior first.
& "$env:GITHUB_WORKSPACE\scripts\apply-v3.ps1"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$fsr4H = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\fsr4\FSR4Upgrade.h'
$fsr4Cpp = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\fsr4\FSR4Upgrade.cpp'
$fsrfg = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\framegen\ffx\FSRFG_Dx12.cpp'

# Public scoped bypass used only while the secondary FG context is created.
$h = [IO.File]::ReadAllText($fsr4H)
$declNeedle = 'void InitFSR4Update();'
if (-not $h.Contains($declNeedle)) { throw 'FSR4Upgrade.h insertion point not found' }
if (-not $h.Contains('SetMultiGPUSecondaryFGProviderBypass')) {
  $h = $h.Replace($declNeedle, "$declNeedle`nvoid SetMultiGPUSecondaryFGProviderBypass(bool enable);")
}
[IO.File]::WriteAllText($fsr4H, $h, [Text.UTF8Encoding]::new($false))

$cpp = [IO.File]::ReadAllText($fsr4Cpp)
if (-not $cpp.Contains('#include <atomic>')) {
  $includeNeedle = '#include <magic_enum.hpp>'
  if (-not $cpp.Contains($includeNeedle)) { throw 'FSR4Upgrade.cpp include insertion point not found' }
  $cpp = $cpp.Replace($includeNeedle, "$includeNeedle`n#include <atomic>")
}

$stateNeedle = 'static HMODULE moduleAmdxcffx64 = nullptr;'
if (-not $cpp.Contains($stateNeedle)) { throw 'FSR4Upgrade.cpp state insertion point not found' }
if (-not $cpp.Contains('_multiGPUSecondaryFGProviderBypass')) {
  $cpp = $cpp.Replace($stateNeedle, "$stateNeedle`nstatic std::atomic_bool _multiGPUSecondaryFGProviderBypass { false };")
}

$funcNeedle = 'std::vector<std::filesystem::path> GetDriverStore()'
if (-not $cpp.Contains($funcNeedle)) { throw 'FSR4Upgrade.cpp function insertion point not found' }
if (-not $cpp.Contains('void SetMultiGPUSecondaryFGProviderBypass(bool enable)')) {
  $setter = @(
    'void SetMultiGPUSecondaryFGProviderBypass(bool enable)',
    '{',
    '    _multiGPUSecondaryFGProviderBypass.store(enable, std::memory_order_release);',
    '    LOG_DEBUG("MultiGPU: secondary FG driver provider bypass: {}", enable);',
    '}',
    ''
  ) -join "`n"
  $cpp = $cpp.Replace($funcNeedle, $setter + "`n" + $funcNeedle)
}

# Return E_NOINTERFACE for FG only while the secondary context is being created.
# This tells FidelityFX that the AMD driver provider does not provide FG, so the
# bundled amd_fidelityfx_framegeneration_dx12.dll provider remains in use.
$effectNeedle = 'auto effectType = FfxApiProxy::GetType(reinterpret_cast<ExternalProviderData*>(pData)->descType);'
$effectPos = $cpp.IndexOf($effectNeedle)
if ($effectPos -lt 0) { throw 'UpdateFfxApiProvider effectType line not found' }
if (-not $cpp.Contains('bypassing AMD driver FFX provider for secondary FG context')) {
  $insertPos = $effectPos + $effectNeedle.Length
  $guard = @(
    '',
    '',
    '        if (effectType == FFXStructType::FG &&',
    '            _multiGPUSecondaryFGProviderBypass.load(std::memory_order_acquire))',
    '        {',
    '            LOG_INFO("MultiGPU: bypassing AMD driver FFX provider for secondary FG context");',
    '            return E_NOINTERFACE;',
    '        }'
  ) -join "`n"
  $cpp = $cpp.Insert($insertPos, $guard)
}
[IO.File]::WriteAllText($fsr4Cpp, $cpp, [Text.UTF8Encoding]::new($false))

# Scope the provider bypass to exactly the same secondary-context creation window
# as the v3 adapter override.
$f = [IO.File]::ReadAllText($fsrfg)
$firstInclude = '#include "FSRFG_Dx12.h"'
if (-not $f.Contains($firstInclude)) { throw 'FSRFG_Dx12 include insertion point not found' }
if (-not $f.Contains('#include <fsr4/FSR4Upgrade.h>')) {
  $f = $f.Replace($firstInclude, "$firstInclude`n#include <fsr4/FSR4Upgrade.h>")
}

$armNeedle = 'D3D12Hooks::SetMultiGPUFGAdapterOverride(computeDevice);'
if (-not $f.Contains($armNeedle)) { throw 'v3 adapter arm line not found' }
if (-not $f.Contains('SetMultiGPUSecondaryFGProviderBypass(true);')) {
  $f = $f.Replace($armNeedle, "$armNeedle`n        SetMultiGPUSecondaryFGProviderBypass(true);")
}

$disarmNeedle = 'D3D12Hooks::SetMultiGPUFGAdapterOverride(nullptr);'
if (-not $f.Contains($disarmNeedle)) { throw 'v3 adapter disarm line not found' }
if (-not $f.Contains('SetMultiGPUSecondaryFGProviderBypass(false);')) {
  $f = $f.Replace($disarmNeedle, "SetMultiGPUSecondaryFGProviderBypass(false);`n        $disarmNeedle")
}
[IO.File]::WriteAllText($fsrfg, $f, [Text.UTF8Encoding]::new($false))

$checkH = [IO.File]::ReadAllText($fsr4H)
$checkCpp = [IO.File]::ReadAllText($fsr4Cpp)
$checkF = [IO.File]::ReadAllText($fsrfg)
if (-not $checkH.Contains('SetMultiGPUSecondaryFGProviderBypass(bool enable)')) { throw 'v4 header injection failed' }
if (-not $checkCpp.Contains('bypassing AMD driver FFX provider for secondary FG context')) { throw 'v4 provider guard injection failed' }
if (-not $checkF.Contains('SetMultiGPUSecondaryFGProviderBypass(true);')) { throw 'v4 provider bypass arm injection failed' }
if (-not $checkF.Contains('SetMultiGPUSecondaryFGProviderBypass(false);')) { throw 'v4 provider bypass disarm injection failed' }

Write-Host 'v4 secondary FG AMD driver-provider bypass injected successfully.'
