# SPDX-License-Identifier: MIT
"""Inspect the original as DATA. Emit only patch metadata, not original code/weights.
The GFX12 scheduling bit is validated against llvm-mc before deriving edits.
"""
import contextlib,hashlib,io,json,os,runpy,struct,subprocess,tempfile
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'reports';OUT.mkdir(exist_ok=True)
SHA='dd38e6ede167c7a5886ae5d079022f63065b6775384d7588f185036b55877afb'
MASK=1<<21
# Directly check the compiler's documented GFX12 directive, avoiding a bit
# copied from older GPUs where bit 21 has different floating-point semantics.
def assembler_descriptor(flag,d):
    src=Path(d)/f'rr{flag}.s';obj=Path(d)/f'rr{flag}.o'
    src.write_text('.text\n.p2align 8\n.globl probe\n.type probe,@function\nprobe:\n s_endpgm\n.size probe, .-probe\n.rodata\n.p2align 6\n.amdhsa_kernel probe\n .amdhsa_group_segment_fixed_size 0\n .amdhsa_private_segment_fixed_size 0\n .amdhsa_kernarg_size 0\n .amdhsa_next_free_vgpr 1\n .amdhsa_next_free_sgpr 2\n .amdhsa_round_robin_scheduling '+str(flag)+'\n.end_amdhsa_kernel\n')
    subprocess.run(['llvm-mc-19','-triple=amdgcn-amd-amdhsa','-mcpu=gfx1201','-filetype=obj',str(src),'-o',str(obj)],check=True)
    elf=ELFFile(io.BytesIO(obj.read_bytes()));raw=elf.get_section_by_name('.rodata').data();assert len(raw)==64
    return raw
with tempfile.TemporaryDirectory() as d:
    zero,one=assembler_descriptor(0,d),assembler_descriptor(1,d)
    assert [i for i,(a,b) in enumerate(zip(zero,one)) if a!=b]==[50]
    assert (struct.unpack_from('<I',zero,48)[0]^struct.unpack_from('<I',one,48)[0])==MASK
with contextlib.redirect_stdout(io.StringIO()):
    ns=runpy.run_path(str(ROOT.parent/'autolab/inspect_boundary.py'))
code=ns['code'];meta=ns['meta'];assert hashlib.sha256(code).hexdigest()==SHA
elf=ELFFile(io.BytesIO(code));patched=bytearray(code)
expected={f'_Z10k_swin_varILi{c}ELb{special}EEv9VarParams' for c,special in [(32,0),(32,1),(64,0),(128,0),(256,0)]}
kernels=[k for k in meta['amdhsa.kernels'] if k['.name'] in expected]
assert len(kernels)==5
rows=[]
for kernel in sorted(kernels,key=lambda k:k['.name']):
    symname=kernel['.symbol'];matches=[]
    for section in elf.iter_sections():
        if isinstance(section,SymbolTableSection):
            for sym in section.iter_symbols():
                if sym.name==symname and isinstance(sym['st_shndx'],int):
                    target=elf.get_section(sym['st_shndx']);delta=sym['st_value']-target['sh_addr']
                    assert 0<=delta and delta+64<=target['sh_size'] and sym['st_size']==64
                    assert target['sh_flags']&2 and not target['sh_flags']&4
                    matches.append((target['sh_offset']+delta,sym['st_value']))
    assert matches and len(set(matches))==1,(symname,matches)
    descriptor,address=matches[0];offset=descriptor+48
    before=struct.unpack_from('<I',code,offset)[0];after=before^MASK
    struct.pack_into('<I',patched,offset,after)
    # No relocation may overwrite the program-setting word on module load.
    for section in elf.iter_sections():
        if section['sh_type'] in ('SHT_RELA','SHT_REL'):
            for rel in section.iter_relocations():
                assert not (address+40<=rel['r_offset']<address+52),'Relocation overlaps scheduler field'
    rows.append({'symbol':kernel['.name'],'descriptor_file_offset':descriptor,'word_file_offset':offset,
        'before_u32':before,'after_u32':after,'before_round_robin':bool(before&MASK),'after_round_robin':bool(after&MASK),
        'group_segment_bytes':kernel.get('.group_segment_fixed_size'),'private_segment_bytes':kernel.get('.private_segment_fixed_size'),
        'vgprs':kernel.get('.vgpr_count'),'sgprs':kernel.get('.sgpr_count')})
changed=[i for i,(a,b) in enumerate(zip(code,patched)) if a!=b]
assert set(changed)=={r['word_file_offset']+2 for r in rows} and len(changed)==5
for i in changed:assert code[i]^patched[i]==0x20
for section in elf.iter_sections():
    if section['sh_flags']&4:
        at=section['sh_offset'];n=section['sh_size'];assert code[at:at+n]==patched[at:at+n]
patched_sha=hashlib.sha256(patched).hexdigest()
text=elf.get_section_by_name('.text').data()
report={'revision':'r6.3','source_code_sha256':SHA,'candidate_code_sha256':patched_sha,
        'altered_bytes':changed,'changed_byte_count':5,'descriptor_field':'COMPUTE_PGM_RSRC1.WG_RR_EN',
        'descriptor_word_byte_offset':48,'word_bit':21,'mask':MASK,'gfx1201_assembler_encoding_checked':True,
        'executable_sections_byte_identical':True,'all_other_bytes_identical':True,
        'original_text_sha256':hashlib.sha256(text).hexdigest(),'edits':rows,
        'original_download_executed_as_host_dll':False,'gpu_executed':False,'speedup_measured':False,
        'scope':'Only the five Swin kernel descriptors are toggled; no FP modes, barriers, arithmetic, ABI, grid, weights or model work is changed.',
        'documentation':'https://releases.llvm.org/19.1.0/docs/AMDGPUUsage.html#compute-pgm-rsrc1-for-gfx6-gfx12'}
(OUT/'scheduling-manifest.json').write_text(json.dumps(report,indent=2)+'\n')
header='// SPDX-License-Identifier: MIT\n// Generated by build_manifest.py from a SHA-pinned original ELF.\n#pragma once\n#include "schedule_policy.hpp"\nnamespace r63 {\n'
header+=f'inline constexpr char original_sha[]="{SHA}";\ninline constexpr char patched_sha[]="{patched_sha}";\n'
header+='inline const std::vector<Patch> patches={\n'
for r in rows:header+='{'+json.dumps(r['symbol'])+f",{r['word_file_offset']}u,{r['before_u32']}u,{r['after_u32']}u"+'},\n'
header+='};\n}\n';(ROOT/'patch_manifest.hpp').write_text(header)
# Compile and execute the exact portable policy also included by the Windows EXE.
source=OUT/'policy_test.cpp';source.write_text('#include "../schedule_policy.hpp"\n#include <iostream>\nint main(){try{std::cout<<r63::self_test()<<std::endl;return 0;}catch(const std::exception& e){std::cerr<<e.what();return 1;}}\n')
tests={}
for label,flags in [('debug',['-O0']),('asan-ubsan',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']),('optimized',['-O2'])]:
    exe=OUT/('policy-'+label)
    subprocess.run(['clang++-19','-std=c++20','-Wall','-Wextra','-Werror',*flags,str(source),'-o',str(exe)],check=True)
    p=subprocess.run([str(exe)],check=True,capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1',UBSAN_OPTIONS='halt_on_error=1'))
    checks=int(p.stdout.strip());assert checks>250;tests[label]={'passed':True,'checks':checks,'gpu_executed':False}
(OUT/'policy-tests.json').write_text(json.dumps(tests,indent=2)+'\n')
print(json.dumps(report,indent=2));print(json.dumps(tests,indent=2))
