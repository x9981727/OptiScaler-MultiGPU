param([Parameter(Mandatory=$true)][string]$MagpieRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$kitRoot = $PSScriptRoot
$reports = Join-Path $kitRoot 'reports'
New-Item -ItemType Directory -Force $reports | Out-Null
$context = Join-Path $reports 'upstream-context.txt'
$inspection = @'
import pathlib,sys
root=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2])
targets={
 'src/Magpie.Core/Renderer.cpp':['_frameGuidance.BeginFrame','_frameGuidance.Initialize','GraphicsCaptureFrameSource','_frameSource->GetOutput','_CompleteBackendFrame'],
 'src/Magpie.Core/FrameSourceBase.cpp':['FrameSourceBase::Initialize','FrameSourceBase::Update'],
 'src/Magpie.Core/ScalingWindow.cpp':['GraphicsCaptureFrameSource','captureMethod'],
}
with out.open('w',encoding='utf-8') as f:
 for name,needles in targets.items():
  p=root/name
  if not p.exists():continue
  lines=p.read_text(encoding='utf-8-sig').splitlines()
  indices=set()
  for n,line in enumerate(lines):
   if any(needle in line for needle in needles):indices.update(range(max(0,n-12),min(len(lines),n+24)))
  f.write('\nFILE '+name+'\n')
  for n in sorted(indices):f.write(f'{n+1}: {lines[n]}\n')
print(out.read_text(encoding='utf-8'))
'@
$inspectPath = Join-Path $reports 'inspect.py'
[IO.File]::WriteAllText($inspectPath, $inspection)
python $inspectPath $MagpieRoot $context
if ($LASTEXITCODE) { throw 'Source inspection failed' }
$baseZip = Join-Path $kitRoot 'r1-core.zip'
if (!(Test-Path $baseZip)) {
  'PREPARE_ONLY: source archive not yet added; no runtime binary claimed.'
  exit 0
}
$source = Join-Path $kitRoot 'source'
Expand-Archive -LiteralPath $baseZip -DestinationPath $source -Force
if (Test-Path (Join-Path $kitRoot 'overlay')) {
  Copy-Item (Join-Path $kitRoot 'overlay\*') $source -Recurse -Force
}
cmake -S $source -B (Join-Path $kitRoot 'build') -A x64
if ($LASTEXITCODE) { throw 'CMake configuration failed' }
cmake --build (Join-Path $kitRoot 'build') --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Windows native bridge compilation failed' }
ctest --test-dir (Join-Path $kitRoot 'build') -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Windows tests failed' }
