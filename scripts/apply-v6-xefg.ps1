$ErrorActionPreference = 'Stop'

$names = 0..7 | ForEach-Object { "v6\xefg-v6.patch.part{0:D2}" -f $_ }
$patch = Join-Path $env:GITHUB_WORKSPACE 'v6\MultiGPU-v6-XeFG-virtual-swapchain.patch'
$stream = [IO.File]::Open($patch, [IO.FileMode]::Create, [IO.FileAccess]::Write)
try {
    foreach ($name in $names) {
        $path = Join-Path $env:GITHUB_WORKSPACE $name
        if (-not (Test-Path $path)) { throw "Missing v6 XeFG patch fragment: $name" }
        $bytes = [IO.File]::ReadAllBytes($path)
        $stream.Write($bytes, 0, $bytes.Length)
    }
}
finally {
    $stream.Dispose()
}

$expectedSha = 'c13c47049a9d77bb8b3fbb50afcdb08d548da75eab5c8c85ca1c060f78fcbdc2'
$sha = (Get-FileHash $patch -Algorithm SHA256).Hash.ToLowerInvariant()
$size = (Get-Item $patch).Length
Write-Host "Reconstructed XeFG v6 patch: $size bytes, SHA256=$sha"
if ($size -ne 37893) { throw "v6 XeFG patch size mismatch: $size" }
if ($sha -ne $expectedSha) { throw "v6 XeFG patch SHA256 mismatch: $sha" }

git -C upstream apply --check --whitespace=error-all $patch
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
