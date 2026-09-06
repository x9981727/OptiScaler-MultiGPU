$ErrorActionPreference = 'Stop'

$hooksH = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\hooks\D3D12_Hooks.h'
$hooksCpp = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\hooks\D3D12_Hooks.cpp'
$fsrfg = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\framegen\ffx\FSRFG_Dx12.cpp'

# Expose a very narrow adapter override API to FSRFG.
$h = [IO.File]::ReadAllText($hooksH)
$needle = '    static void Hook();'
if (-not $h.Contains($needle)) { throw 'D3D12_Hooks.h insertion point not found' }
$h = $h.Replace($needle, "    static void SetMultiGPUFGAdapterOverride(ID3D12Device* device);`n$needle")
[IO.File]::WriteAllText($hooksH, $h, [Text.UTF8Encoding]::new($false))

# Patch D3D12CreateDevice so AMD's internal nullptr/default adapter request is
# redirected to the active secondary FG device only while its context is created.
$cpp = [IO.File]::ReadAllText($hooksCpp)
$includeNeedle = '#include <Config.h>'
if (-not $cpp.Contains($includeNeedle)) { throw 'D3D12_Hooks.cpp include insertion point not found' }
$cpp = $cpp.Replace($includeNeedle, "$includeNeedle`n#include <framegen/MultiGPU_Dx12.h>`n#include <atomic>")

$stateNeedle = 'static LUID _lastAdapterLuid = {};'
if (-not $cpp.Contains($stateNeedle)) { throw 'D3D12_Hooks.cpp state insertion point not found' }
$cpp = $cpp.Replace($stateNeedle, "$stateNeedle`nstatic std::atomic<uint64_t> _forceMultiGPUFGLuidPacked { 0 };")

$fnNeedle = 'VALIDATE_HOOK(hkD3D12CreateDevice, D3d12Proxy::PFN_D3D12CreateDevice)'
if (-not $cpp.Contains($fnNeedle)) { throw 'hkD3D12CreateDevice marker not found' }
$setter = @(
  'void D3D12Hooks::SetMultiGPUFGAdapterOverride(ID3D12Device* device)',
  '{',
  '    uint64_t packed = 0;',
  '    if (device != nullptr)',
  '    {',
  '        const auto luid = device->GetAdapterLuid();',
  '        packed = (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) |',
  '                 static_cast<uint64_t>(luid.LowPart);',
  '    }',
  '    _forceMultiGPUFGLuidPacked.store(packed, std::memory_order_release);',
  '    LOG_DEBUG("MultiGPU: scoped internal D3D12 adapter override: {}, LUID={:016X}", packed != 0, packed);',
  '}',
  ''
) -join "`n"
$cpp = $cpp.Replace($fnNeedle, $setter + "`n" + $fnNeedle)

$fnPos = $cpp.IndexOf('static HRESULT hkD3D12CreateDevice')
if ($fnPos -lt 0) { throw 'hkD3D12CreateDevice function not found' }
$logNeedle = 'Util::WhoIsTheCaller(_ReturnAddress()));'
$logPos = $cpp.IndexOf($logNeedle, $fnPos)
if ($logPos -lt 0) { throw 'hkD3D12CreateDevice log insertion point not found' }
$insertPos = $logPos + $logNeedle.Length
$overrideCode = @(
  '',
  '',
  '    Microsoft::WRL::ComPtr<IDXGIAdapter1> forcedMultiGpuAdapter;',
  '    const uint64_t forcedPacked = _forceMultiGPUFGLuidPacked.load(std::memory_order_acquire);',
  '    if (forcedPacked != 0 && pAdapter == nullptr)',
  '    {',
  '        LUID forcedLuid {};',
  '        forcedLuid.LowPart = static_cast<DWORD>(forcedPacked & 0xFFFFFFFFull);',
  '        forcedLuid.HighPart = static_cast<LONG>((forcedPacked >> 32) & 0xFFFFFFFFull);',
  '        IDXGIAdapter1* rawAdapter = nullptr;',
  '        if (MultiGPU::TryGetAdapterByLuid(MultiGPU::LuidToString(forcedLuid), &rawAdapter))',
  '        {',
  '            forcedMultiGpuAdapter.Attach(rawAdapter);',
  '            pAdapter = forcedMultiGpuAdapter.Get();',
  '            DXGI_ADAPTER_DESC1 forcedDesc {};',
  '            forcedMultiGpuAdapter->GetDesc1(&forcedDesc);',
  '            LOG_INFO("MultiGPU: forcing internal D3D12CreateDevice(nullptr) to FG adapter {}",',
  '                     wstring_to_string(forcedDesc.Description));',
  '        }',
  '        else',
  '        {',
  '            LOG_ERROR("MultiGPU: failed to resolve active FG adapter for internal D3D12CreateDevice");',
  '        }',
  '    }'
) -join "`n"
$cpp = $cpp.Insert($insertPos, $overrideCode)
[IO.File]::WriteAllText($hooksCpp, $cpp, [Text.UTF8Encoding]::new($false))

# Arm the override before the *whole* computeCreateResult assignment, not in the
# middle of its multiline expression. This avoids turning computeCreateResult into void.
$f = [IO.File]::ReadAllText($fsrfg)
$firstInclude = '#include "FSRFG_Dx12.h"'
if (-not $f.Contains($firstInclude)) { throw 'FSRFG_Dx12 include insertion point not found' }
$f = $f.Replace($firstInclude, "$firstInclude`n#include <hooks/D3D12_Hooks.h>")

$ctxNeedle = 'FfxApiProxy::D3D12_CreateContext(&_multiGpuComputeContext'
$ctxPos = $f.IndexOf($ctxNeedle)
if ($ctxPos -lt 0) { throw 'secondary compute context creation call not found' }
$assignPos = $f.LastIndexOf('auto computeCreateResult', $ctxPos)
if ($assignPos -lt 0 -or ($ctxPos - $assignPos) -gt 512) {
  throw 'computeCreateResult assignment not found near secondary context creation'
}
$windowStart = [Math]::Max(0, $assignPos - 1200)
$window = $f.Substring($windowStart, $ctxPos - $windowStart)
if (-not $window.Contains('computeDevice')) { throw 'computeDevice not found near secondary context creation' }

$lineStart = $f.LastIndexOf("`n", $assignPos)
if ($lineStart -lt 0) { $lineStart = 0 } else { $lineStart++ }
$semi = $f.IndexOf(';', $ctxPos)
if ($semi -lt 0) { throw 'secondary compute context statement terminator not found' }
$assignLine = $f.Substring($lineStart, [Math]::Min(200, $f.Length - $lineStart))
$indent = [regex]::Match($assignLine, '^\s*').Value

# Insert the trailing disarm first because its position is later in the string.
$f = $f.Insert($semi + 1, "`n${indent}D3D12Hooks::SetMultiGPUFGAdapterOverride(nullptr);")
$f = $f.Insert($lineStart, "${indent}D3D12Hooks::SetMultiGPUFGAdapterOverride(computeDevice);`n")
[IO.File]::WriteAllText($fsrfg, $f, [Text.UTF8Encoding]::new($false))

# Fail the build early if injection did not land as intended.
$checkH = [IO.File]::ReadAllText($hooksH)
$checkCpp = [IO.File]::ReadAllText($hooksCpp)
$checkF = [IO.File]::ReadAllText($fsrfg)
if (-not $checkH.Contains('SetMultiGPUFGAdapterOverride(ID3D12Device* device)')) { throw 'v3 header injection failed' }
if (-not $checkCpp.Contains('forcing internal D3D12CreateDevice(nullptr)')) { throw 'v3 D3D12 hook injection failed' }
if (-not $checkCpp.Contains('_forceMultiGPUFGLuidPacked')) { throw 'v3 atomic LUID state injection failed' }
if (-not $checkF.Contains('SetMultiGPUFGAdapterOverride(computeDevice)')) { throw 'v3 FSRFG arm injection failed' }
if (-not $checkF.Contains('SetMultiGPUFGAdapterOverride(nullptr)')) { throw 'v3 FSRFG disarm injection failed' }

Write-Host 'v3 scoped AMD adapter override injected successfully.'
