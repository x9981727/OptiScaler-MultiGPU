$ErrorActionPreference = 'Stop'

$d3dPath = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\hooks\D3D12_Hooks.cpp'
$xefgPath = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\framegen\xefg\XeFG_Dx12.cpp'

# v7 bypassed the entire post-v3 adapter-override block. That left AMD's internal
# D3D12CreateDevice(nullptr) on the default adapter. Remove the early v7 bypass and
# reinsert a passthrough only AFTER v3 has resolved nullptr to the selected FG adapter.
$d = [IO.File]::ReadAllText($d3dPath)
$earlyPattern = '(?ms)^    if \(State::multiGpuXeFGInit\)\r?\n    \{\r?\n        LOG_TRACE\("MultiGPU v7: passthrough D3D12CreateDevice during XeFG internal initialization"\);\r?\n        _creatingD3D12Device = true;\r?\n        auto result = o_D3D12CreateDevice\(pAdapter, MinimumFeatureLevel, riid, ppDevice\);\r?\n        _creatingD3D12Device = false;\r?\n        return result;\r?\n    \}\r?\n\r?\n'
$updated = [regex]::Replace($d, $earlyPattern, '', 1)
if ($updated -eq $d) { throw 'v8: could not remove early v7 D3D12 passthrough block' }
$d = $updated

$fnPos = $d.IndexOf('static HRESULT hkD3D12CreateDevice')
if ($fnPos -lt 0) { throw 'v8: hkD3D12CreateDevice function not found' }
$overridePos = $d.IndexOf('const uint64_t forcedPacked = _forceMultiGPUFGLuidPacked.load(std::memory_order_acquire);', $fnPos)
if ($overridePos -lt 0) { throw 'v8: v3 adapter override block not found' }
$debugPos = $d.IndexOf('#ifdef ENABLE_DEBUG_LAYER_DX12', $overridePos)
if ($debugPos -lt 0) { throw 'v8: post-v3 insertion point not found' }

$v8D3D = @(
    '    if (State::multiGpuXeFGInit)',
    '    {',
    '        LOG_TRACE("MultiGPU v7: passthrough D3D12CreateDevice during XeFG internal initialization");',
    '        LOG_TRACE("MultiGPU v8: passthrough D3D12CreateDevice after FG adapter override");',
    '        _creatingD3D12Device = true;',
    '        auto result = o_D3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);',
    '        _creatingD3D12Device = false;',
    '        return result;',
    '    }',
    '',
    ''
) -join "`n"
$d = $d.Insert($debugPos, $v8D3D)
[IO.File]::WriteAllText($d3dPath, $d, [Text.UTF8Encoding]::new($false))

# XeFG did not arm the v3 LUID override at all. Arm it specifically around Intel's
# D3D12InitFromSwapChainDesc using the already-created secondary FG device.
$x = [IO.File]::ReadAllText($xefgPath)
$includeNeedle = '#include "XeFG_Dx12.h"'
if (-not $x.Contains('#include <hooks/D3D12_Hooks.h>')) {
    if (-not $x.Contains($includeNeedle)) { throw 'v8: XeFG include insertion point not found' }
    $x = $x.Replace($includeNeedle, "$includeNeedle`n#include <hooks/D3D12_Hooks.h>")
}

$isolateNeedle = '        ScopedMultiGpuXeFGInit isolateXeFGInit {};'
$positions = @()
$offset = 0
while ($true) {
    $p = $x.IndexOf($isolateNeedle, $offset)
    if ($p -lt 0) { break }
    $positions += $p
    $offset = $p + $isolateNeedle.Length
}
if ($positions.Count -ne 2) { throw "v8: expected 2 XeFG isolation blocks, found $($positions.Count)" }

for ($i = $positions.Count - 1; $i -ge 0; --$i) {
    $p = $positions[$i]
    $callPos = $x.IndexOf('XeFGProxy::D3D12InitFromSwapChainDesc()', $p)
    if ($callPos -lt 0) { throw 'v8: XeFG init call not found after isolation marker' }
    $semi = $x.IndexOf(';', $callPos)
    if ($semi -lt 0) { throw 'v8: XeFG init call terminator not found' }

    $disarm = @(
        '',
        '        D3D12Hooks::SetMultiGPUFGAdapterOverride(nullptr);',
        '        LOG_INFO("MultiGPU v8: disarmed XeFG internal D3D12 adapter override");'
    ) -join "`n"
    $x = $x.Insert($semi + 1, $disarm)

    $arm = @(
        '        D3D12Hooks::SetMultiGPUFGAdapterOverride(_multiGpuRuntime->FGDevice());',
        '        LOG_INFO("MultiGPU v8: armed XeFG internal D3D12 adapter override for secondary GPU");',
        ''
    ) -join "`n"
    $x = $x.Insert($p, $arm)
}

[IO.File]::WriteAllText($xefgPath, $x, [Text.UTF8Encoding]::new($false))

# Fail early unless the exact architecture landed.
$d = [IO.File]::ReadAllText($d3dPath)
$x = [IO.File]::ReadAllText($xefgPath)
if (-not $d.Contains('MultiGPU v7: passthrough D3D12CreateDevice during XeFG internal initialization')) {
    throw 'v8: compatibility v7 D3D12 marker missing'
}
if (-not $d.Contains('MultiGPU v8: passthrough D3D12CreateDevice after FG adapter override')) {
    throw 'v8: post-override D3D12 passthrough marker missing'
}
$armMarker = 'MultiGPU v8: armed XeFG internal D3D12 adapter override for secondary GPU'
$disarmMarker = 'MultiGPU v8: disarmed XeFG internal D3D12 adapter override'
if ([regex]::Matches($x, [regex]::Escape($armMarker)).Count -ne 2) {
    throw 'v8: XeFG arm markers missing'
}
if ([regex]::Matches($x, [regex]::Escape($disarmMarker)).Count -ne 2) {
    throw 'v8: XeFG disarm markers missing'
}
if (-not $x.Contains('SetMultiGPUFGAdapterOverride(_multiGpuRuntime->FGDevice())')) {
    throw 'v8: XeFG selected-device override call missing'
}

Write-Host 'v8 XeFG selected-adapter initialization isolation injected successfully.'
