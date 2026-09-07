# SPDX-License-Identifier: MIT
"""Compile r2 device code; this is not GPU execution or model validation."""
import collections,hashlib,io,json,re,subprocess
from pathlib import Path
import msgpack
from elftools.elf.elffile import ELFFile
root=Path(__file__).resolve().parents[1]
out=root/'build';out.mkdir(exist_ok=True)
source=root/'kernels/packed_r2.cl';obj=out/'packed_r2.o';code=out/'packed_r2_gfx1201.hsaco'
command=['clang-19','-x','cl','-cl-std=CL2.0','--target=amdgcn-amd-amdhsa','-mcpu=gfx1201',
 '-mno-wavefrontsize64','-mcode-object-version=5','-nogpulib','-O3','-ffp-contract=off','-c',str(source),'-o',str(obj)]
print(' '.join(command),flush=True)
subprocess.run(command,check=True)
subprocess.run(['ld.lld-19','-shared',str(obj),'-o',str(code)],check=True)
undefined=subprocess.check_output(['llvm-nm-19','--undefined-only',str(code)],text=True).strip()
assert not undefined,undefined
text=subprocess.check_output(['llvm-objdump-19','-d','--mcpu=gfx1201',str(code)],text=True)
(out/'packed_r2.isa.txt').write_text(text,encoding='utf-8')
elf=ELFFile(io.BytesIO(code.read_bytes()));meta=None
for section in elf.iter_sections():
 if section.header.sh_type=='SHT_NOTE':
  for note in section.iter_notes():
   if note['n_name']=='AMDGPU':meta=msgpack.unpackb(note['n_desc'],raw=False,strict_map_key=False)
assert meta is not None and meta['amdhsa.target']=='amdgcn-amd-amdhsa--gfx1201'
parts=re.split(r'^[0-9a-fA-F]+ <([^>]+)>:\s*$',text,flags=re.M)
functions=dict(zip(parts[1::2],parts[2::2]))
report={'compiled':True,'gpu_tested':False,'game_tested':False,'original_model_equivalence_tested':False,
 'command':command,'code_sha256':hashlib.sha256(code.read_bytes()).hexdigest(),
 'compiler':subprocess.check_output(['clang-19','--version'],text=True).splitlines()[0],
 'metadata':meta,'kernels':[]}
for k in meta['amdhsa.kernels']:
 name=k['.name'];body=functions[name]
 assert k['.wavefront_size']==32
 assert k['.private_segment_fixed_size']==0,(name,'scratch')
 assert k['.group_segment_fixed_size']==0,(name,'LDS')
 assert k.get('.vgpr_spill_count',0)==0,(name,'VGPR spills')
 explicit=[a['.offset'] for a in k['.args'] if not a['.value_kind'].startswith('hidden_')]
 assert explicit==([0,8,16,20] if name=='pack_kmajor_fp8' else [0,8,16,24,32,40,44,48,52,56]),(name,explicit)
 ops=collections.Counter(re.findall(r'\b(v_wmma_\w+)\b',body))
 loads=collections.Counter(re.findall(r'\b((?:global|buffer|flat)_load_\w+)\b',body))
 if name!='pack_kmajor_fp8':
  expected='v_wmma_f32_16x16x16_f16' if 'fp16' in name else 'v_wmma_f32_16x16x16_fp8_fp8'
  assert ops[expected]>0,(name,'Missing WMMA')
 if 'packed' in name:
  assert any(re.search(r'(?:b64|b128|dwordx2|dwordx4)',x) for x in loads),(name,'No vector load',loads)
 summary={'name':name,'vgpr':k['.vgpr_count'],'scratch_bytes':k['.private_segment_fixed_size'],
  'static_lds_bytes':k['.group_segment_fixed_size'],'matrix_static':dict(ops),'load_static':dict(loads)}
 report['kernels'].append(summary);print(json.dumps(summary))
assert len(report['kernels'])==7
(out/'r2-compile-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('Seven kernels compiled and statically inspected; GPU numerical/performance validation not performed here.')
