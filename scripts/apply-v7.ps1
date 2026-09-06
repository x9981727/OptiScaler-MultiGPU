$ErrorActionPreference = 'Stop'

$patch = Join-Path $env:GITHUB_WORKSPACE 'v7\xefg-init-isolation.patch'
if (-not (Test-Path $patch)) { throw "Missing v7 patch: $patch" }
$sha = (Get-FileHash $patch -Algorithm SHA256).Hash.ToLowerInvariant()
if ($sha -ne '690e732e4076ed0e119d9fd43dd41623100b0a226f27a9169267f697a47b5e79') {
    throw "v7 patch SHA256 mismatch: $sha"
}

# D3D12_Hooks.cpp has already been modified by v3. Apply all other v7 hunks first.
git -C upstream apply --check --directory=OptiScaler --exclude=OptiScaler/hooks/D3D12_Hooks.cpp $patch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git -C upstream apply --directory=OptiScaler --exclude=OptiScaler/hooks/D3D12_Hooks.cpp $patch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Inject the D3D12 isolation block into the post-v3 source using v3's stable marker.
$d3dPath = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\hooks\D3D12_Hooks.cpp'
$d = [IO.File]::ReadAllText($d3dPath)
if (-not $d.Contains('MultiGPU v7: passthrough D3D12CreateDevice during XeFG internal initialization')) {
    $fnPos = $d.IndexOf('static HRESULT hkD3D12CreateDevice')
    if ($fnPos -lt 0) { throw 'v7: hkD3D12CreateDevice function not found' }

    $marker = '    Microsoft::WRL::ComPtr<IDXGIAdapter1> forcedMultiGpuAdapter;'
    $markerPos = $d.IndexOf($marker, $fnPos)
    if ($markerPos -lt 0) { throw 'v7: post-v3 forcedMultiGpuAdapter marker not found' }

    $block = @(
        '    if (State::multiGpuXeFGInit)',
        '    {',
        '        LOG_TRACE("MultiGPU v7: passthrough D3D12CreateDevice during XeFG internal initialization");',
        '        _creatingD3D12Device = true;',
        '        auto result = o_D3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);',
        '        _creatingD3D12Device = false;',
        '        return result;',
        '    }',
        '',
        ''
    ) -join "`n"

    $d = $d.Insert($markerPos, $block)
    [IO.File]::WriteAllText($d3dPath, $d, [Text.UTF8Encoding]::new($false))
}

$xefgPath = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\framegen\xefg\XeFG_Dx12.cpp'
$dxgiPath = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\hooks\DxgiFactory_Hooks.cpp'
$wrappedPath = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\hooks\DxgiFactory_WrappedCalls.cpp'
$statePath = Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\State.h'

$xefg = [IO.File]::ReadAllText($xefgPath)
$dxgi = [IO.File]::ReadAllText($dxgiPath)
$wrapped = [IO.File]::ReadAllText($wrappedPath)
$state = [IO.File]::ReadAllText($statePath)
$d = [IO.File]::ReadAllText($d3dPath)

$required = @(
    @($state, 'multiGpuXeFGInit'),
    @($state, 'ScopedMultiGpuXeFGInit'),
    @($xefg, 'MultiGPU v7: isolating XeFG internal DXGI/D3D12 initialization'),
    @($d, 'MultiGPU v7: passthrough D3D12CreateDevice during XeFG internal initialization'),
    @($dxgi, 'MultiGPU v7: passthrough XeFG internal CreateSwapChainForHwnd without Opti wrapping'),
    @($wrapped, 'MultiGPU v7: passthrough wrapped-factory XeFG internal CreateSwapChainForHwnd without Opti wrapping')
)
foreach ($item in $required) {
    if (-not $item[0].Contains($item[1])) { throw "v7 source marker missing: $($item[1])" }
}

Write-Host 'v7 XeFG initialization isolation injected successfully.'
