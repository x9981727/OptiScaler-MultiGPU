$ErrorActionPreference = 'Stop'

$chunkNames = @(
    'v6\\xefg-v6.patch.gz.part00',
    'v6\\xefg-v6.patch.gz.part01'
)
$outGz = Join-Path $env:GITHUB_WORKSPACE 'v6\\MultiGPU-v6-XeFG.patch.gz'
$outPatch = Join-Path $env:GITHUB_WORKSPACE 'v6\\MultiGPU-v6-XeFG-virtual-swapchain.rebuilt.patch'

$stream = [IO.File]::Open($outGz, [IO.FileMode]::Create, [IO.FileAccess]::Write)
try {
    foreach ($relative in $chunkNames) {
        $path = Join-Path $env:GITHUB_WORKSPACE $relative
        if (-not (Test-Path $path)) { throw "Missing XeFG v6 gzip chunk: $relative" }
        $bytes = [IO.File]::ReadAllBytes($path)
        $stream.Write($bytes, 0, $bytes.Length)
    }
}
finally { $stream.Dispose() }

$gzSize = (Get-Item $outGz).Length
$gzSha = (Get-FileHash $outGz -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Reconstructed XeFG v6 gzip: $gzSize bytes, SHA256=$gzSha"
if ($gzSize -ne 7737) { throw "v6 XeFG gzip size mismatch: $gzSize" }
if ($gzSha -ne '7ca3cf3c23d81a17eb58ccaeecc8a0d1b2c65aa7011672e7bc9a76443282eece') { throw "v6 XeFG gzip SHA256 mismatch: $gzSha" }

$input = [IO.File]::OpenRead($outGz)
$output = [IO.File]::Create($outPatch)
try {
    $gzip = [IO.Compression.GZipStream]::new($input, [IO.Compression.CompressionMode]::Decompress)
    try { $gzip.CopyTo($output) } finally { $gzip.Dispose() }
}
finally { $output.Dispose(); $input.Dispose() }

$size = (Get-Item $outPatch).Length
$sha = (Get-FileHash $outPatch -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Decompressed XeFG v6 patch: $size bytes, SHA256=$sha"
if ($size -ne 37585) { throw "v6 XeFG patch size mismatch: $size" }
if ($sha -ne '63bf8df77d02b413fb59cc5b21df62b708bfe400abde921783e17d0869794f92') { throw "v6 XeFG patch SHA256 mismatch: $sha" }

git -C upstream apply --check --directory=OptiScaler $outPatch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git -C upstream apply --directory=OptiScaler $outPatch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$xefg = [IO.File]::ReadAllText((Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\framegen\xefg\XeFG_Dx12.cpp'))
$wrapped = [IO.File]::ReadAllText((Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\wrapped\wrapped_swapchain.cpp'))
$hooks = [IO.File]::ReadAllText((Join-Path $env:GITHUB_WORKSPACE 'upstream\OptiScaler\hooks\FG_Hooks.cpp'))
if (-not $xefg.Contains('MultiGPU v6: XeFG context created on secondary GPU')) { throw 'v6 XeFG source marker missing' }
if (-not $wrapped.Contains('MultiGPU v6: virtual backbuffer initialization')) { throw 'v6 virtual backbuffer source marker missing' }
if (-not $hooks.Contains('MultiGPU v6: XeFG proxy swapchain virtualized')) { throw 'v6 FG hook source marker missing' }
Write-Host 'v6 XeFG MultiGPU virtual-swapchain patch applied successfully.'
