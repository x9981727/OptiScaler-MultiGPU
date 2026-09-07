# SPDX-License-Identifier: MIT
"""Cross-compilation and CPU-only validation; no GPU execution claim."""
import collections,hashlib,io,json,re,subprocess
from pathlib import Path
import msgpack
from elftools.elf.elffile import ELFFile
root=Path(__file__).resolve().parent
out=root/'build';out.mkdir(exist_ok=True)
source=out/'cpu_main.cpp'
source.write_text('#include "head_contract.hpp"\n#include <iostream>\nint main(){try{auto n=head_contract::cpu_tests();std::cout<<n<<" checks passed; CPU only\\n";return 0;}catch(const std::exception& e){std::cerr<<e.what();return 1;}}\n')
subprocess.run(['clang++-19','-std=c++20','-O2','-ffp-contract=off','-I',str(root),str(source),'-o',str(out/'cpu_test')],check=True)
cpu=subprocess.check_output([str(out/'cpu_test')],text=True);print(cpu,flush=True)
cmd=['clang-19','-x','cl','-cl-std=CL2.0','--target=amdgcn-amd-amdhsa','-mcpu=gfx1201',
     '-mno-wavefrontsize64','-mcode-object-version=5','-nogpulib','-O3','-ffp-contract=off','-c',str(root/'head_direct.cl'),'-o',str(out/'head.o')]
subprocess.run(cmd,check=True)
code=out/'head_r4_gfx1201.hsaco'
subprocess.run(['ld.lld-19','-shared',str(out/'head.o'),'-o',str(code)],check=True)
text=subprocess.check_output(['llvm-objdump-19','-d','--mcpu=gfx1201',str(code)],text=True)
(out/'head_r4.isa.txt').write_text(text)
elf=ELFFile(io.BytesIO(code.read_bytes()));meta=None
for section in elf.iter_sections():
    if section.header.sh_type=='SHT_NOTE':
        for note in section.iter_notes():
            if note['n_name']=='AMDGPU':meta=msgpack.unpackb(note['n_desc'],raw=False,strict_map_key=False)
if meta is None:raise ValueError('GPU metadata absent')
assert meta['amdhsa.target']=='amdgcn-amd-amdhsa--gfx1201'
parts=re.split(r'^[0-9a-fA-F]+ <([^>]+)>:\s*$',text,flags=re.M);funcs=dict(zip(parts[1::2],parts[2::2]))
rows=[]
for k in meta['amdhsa.kernels']:
    name=k['.name'];body=funcs[name]
    args=[a for a in k['.args'] if not a['.value_kind'].startswith('hidden_')]
    assert len(args)==1 and args[0]['.offset']==0 and args[0]['.size']==24 and args[0]['.value_kind']=='by_value'
    assert k['.wavefront_size']==32 and k['.group_segment_fixed_size']==0
    assert k['.private_segment_fixed_size']==0 and k.get('.vgpr_spill_count',0)==0
    assert 'v_wmma_f32_16x16x16_fp8_fp8' in body
    assert 'v_cvt_f16_f32' in body and 'v_cvt_pk_fp8_f32' in body,'Rounding/FP8 conversion missing'
    row={'name':name,'vgpr':k['.vgpr_count'],'static_lds':k['.group_segment_fixed_size'],
         'scratch':k['.private_segment_fixed_size'],'original_explicit_abi_bytes':24,
         'fp8_wmma_static_count':body.count('v_wmma_f32_16x16x16_fp8_fp8')}
    print(json.dumps(row),flush=True);rows.append(row)
assert len(rows)==3
sha=hashlib.sha256(code.read_bytes()).hexdigest()
(root/'candidate_hash.hpp').write_text('#pragma once\ninline constexpr char CANDIDATE_CODE_SHA[]="'+sha+'";\n')
report={'scope':'Original-head contract candidate compilation, not full Swin or game',
        'cpu_self_test':cpu.strip(),'device_compilation_passed':True,'gpu_tested':False,
        'original_head_equivalence_tested_on_gpu':False,'full_model_tested':False,'game_tested':False,
        'candidate_sha256':sha,'kernels':rows,'metadata':meta,'command':cmd}
(out/'compile-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('COMPILED AND CPU TESTED; ORIGINAL GPU DIFFERENTIAL TEST STILL REQUIRED',flush=True)
