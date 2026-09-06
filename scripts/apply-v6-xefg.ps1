$ErrorActionPreference = 'Stop'

$patch = Join-Path $env:GITHUB_WORKSPACE 'v6\MultiGPU-v6-XeFG-virtual-swapchain.patch'
if (-not (Test-Path $patch)) { throw "v6 XeFG patch not found: $patch" }

git -C upstream apply --check $patch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git -C upstream apply $patch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$checks = @(
    'upstream\OptiScaler\framegen\xefg\XeFG_Dx12.cpp',
    'upstream\OptiScaler\wrapped\wrapped_swapchain.cpp',
    'upstream\OptiScaler\hooks\FG_Hooks.cpp',
    'upstream\OptiScaler\framegen\MultiGPU_Dx12.cpp'
)
foreach ($relative in $checks) {
    $path = Join-Path $env:GITHUB_WORKSPACE $relative
    if (-not (Test-Path $path)) { throw "v6 source missing: $relative" }
}

$xefg = [IO.File]::ReadAllText((Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\framegen\xefg\XeFG_Dx12.cpp'))
$wrapped = [IO.File]::ReadAllText((Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\wrapped\wrapped_swapchain.cpp'))
$hooks = [IO.File]::ReadAllText((Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\hooks\FG_Hooks.cpp'))
if (-not $xefg.Contains('MultiGPU v6: XeFG context created on secondary GPU')) { throw 'v6 XeFG source marker missing' }
if (-not $wrapped.Contains('MultiGPU v6: virtual backbuffer initialization')) { throw 'v6 virtual backbuffer source marker missing' }
if (-not $hooks.Contains('MultiGPU v6: XeFG proxy swapchain virtualized')) { throw 'v6 FG hook source marker missing' }

Write-Host 'v6 XeFG MultiGPU virtual-swapchain patch applied successfully.'
