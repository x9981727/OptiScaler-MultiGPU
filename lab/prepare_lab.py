"""Apply only the confirmed sequence fix to reconstructed v25; never hide failures."""
from pathlib import Path
import subprocess,hashlib,json
ROOT=Path(__file__).resolve().parents[1]
upstream=ROOT/'upstream'
patch=ROOT/'v25-sequence/sequence-only.patch'
cmd=['git','-C',str(upstream),'apply','--ignore-space-change']
subprocess.run(cmd+['--check',str(patch)],check=True)
subprocess.run(cmd+[str(patch)],check=True)
s=(upstream/'OptiScaler/framegen/xefg/XeFG_Dx12.cpp').read_text(encoding='utf-8-sig')
if 'const uint32_t compositorId = ++_managedSerial;' in s:raise RuntimeError('old pre-validation increment retained')
if s.count('MultiGPU::NextManagedXeFGId(_managedSerial, complete)')!=1:raise RuntimeError('production sequence fix not wired exactly once')
result=ROOT/'lab-results';result.mkdir(exist_ok=True)
manifest={'kind':'virtual_lab_source','candidate_hardware_accepted':False,'patch_sha256':hashlib.sha256(patch.read_bytes()).hexdigest(),'production_xefg_sha256':hashlib.sha256((upstream/'OptiScaler/framegen/xefg/XeFG_Dx12.cpp').read_bytes()).hexdigest()}
(result/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Applied confirmed sequence correction; performance/flicker acceptance remains blocked.')
