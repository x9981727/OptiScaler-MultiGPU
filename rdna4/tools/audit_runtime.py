# SPDX-License-Identifier: MIT
"""Static audit of a SHA-pinned binary; never runs the proprietary runtime."""
import collections, hashlib, io, json, re, struct, subprocess, tempfile, urllib.request
from pathlib import Path
import msgpack
from elftools.elf.elffile import ELFFile
from elftools.common.exceptions import ELFError

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'audit'; OUT.mkdir(parents=True,exist_ok=True)
COMMIT='04d8b83f26adcbdd489137ea27a3ce59fc75eab4'
URL=f'https://raw.githubusercontent.com/MatheusGViana/dlss-5-amd-project/{COMMIT}/version.dll'
data=urllib.request.urlopen(URL,timeout=60).read()
sha=hashlib.sha256(data).hexdigest()
assert sha=='106223723fd9266c44d38dc2fb77933948ab37803f46bfcea2bae3a0a474ac84',sha
report={'upstream_commit':COMMIT,'runtime_sha256':sha,'hardware_tested':False,'objects':[],'gfx1201_swin':[]}
start=0
while True:
    start=data.find(b'\x7fELF',start)
    if start<0: break
    off=start; start+=4
    try:
        if data[off+4:off+6]!=b'\x02\x01': continue
        shoff=struct.unpack_from('<Q',data,off+40)[0]
        shentsize,shnum=struct.unpack_from('<HH',data,off+58)
        size=shoff+shentsize*shnum
        if size<=64 or off+size>len(data): continue
        raw=data[off:off+size]; elf=ELFFile(io.BytesIO(raw))
        if elf.header['e_machine']!='EM_AMDGPU': continue
        kernels=[];target=''
        for section in elf.iter_sections():
            if section.header.sh_type!='SHT_NOTE': continue
            for note in section.iter_notes():
                if note['n_name']=='AMDGPU':
                    meta=msgpack.unpackb(note['n_desc'],raw=False,strict_map_key=False)
                    target=meta.get('amdhsa.target',target)
                    kernels.extend(meta.get('amdhsa.kernels',[]))
        item={'offset':off,'size':size,'target':target,'kernel_count':len(kernels)}
        report['objects'].append(item)
        if target!='amdgcn-amd-amdhsa--gfx1201': continue
        with tempfile.TemporaryDirectory() as td:
            p=Path(td)/'original.hsaco';p.write_bytes(raw)
            text=subprocess.check_output(['llvm-objdump-19','-d','--mcpu=gfx1201',str(p)],text=True)
        # No proprietary runtime, weight binary or full disassembly in the distribution.
        parts=re.split(r'^[0-9a-fA-F]+ <([^>]+)>:\s*$',text,flags=re.M)
        functions=dict(zip(parts[1::2],parts[2::2]))
        for kernel in kernels:
            name=kernel.get('.name','')
            if 'k_swin_var' not in name: continue
            body=functions[name]
            counts=dict(collections.Counter(re.findall(r'\b(v_(?:swmmac|wmma|mfma)[a-zA-Z0-9_]*)\b',body)))
            row={'name':name,'vgpr_count':kernel['.vgpr_count'],
                 'static_lds_bytes':kernel['.group_segment_fixed_size'],
                 'private_segment_bytes':kernel['.private_segment_fixed_size'],
                 'vgpr_spill_count':kernel.get('.vgpr_spill_count',0),
                 'wavefront_size':kernel['.wavefront_size'],
                 'by_value_argument_bytes':[a['.size'] for a in kernel['.args'] if a['.value_kind']=='by_value'],
                 'matrix_instructions_static':counts}
            assert counts.get('v_wmma_f32_16x16x16_fp8_fp8',0)>0
            report['gfx1201_swin'].append(row)
            print(json.dumps(row))
    except (ELFError,ValueError,KeyError,struct.error) as e:
        print('Rejected ELF candidate',off,type(e).__name__)
assert len(report['gfx1201_swin'])==5, 'Incomplete gfx1201 Swin audit'
report['limitations']=['Static instructions are not dynamic utilization.',
    'Static LDS is not the dynamic shared-memory allocation.',
    'This audit does not measure game performance or model equivalence.',
    'These findings apply only to the pinned runtime, not every version.']
(OUT/'runtime-audit.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
text=['# gfx1201 runtime audit','','Pinned runtime SHA256: `'+sha+'`',
      '','The existing runtime already has a gfx1201 code object and native FP8 WMMA in all five Swin variants.',
      '','| Swin symbol | VGPR | Static LDS bytes | VGPR spills | Static FP8 WMMA instructions |',
      '|---|---:|---:|---:|---:|']
for k in report['gfx1201_swin']:
    text.append('| '+k['name']+' | '+str(k['vgpr_count'])+' | '+str(k['static_lds_bytes'])+' | '+str(k['vgpr_spill_count'])+' | '+str(k['matrix_instructions_static']['v_wmma_f32_16x16x16_fp8_fp8'])+' |')
text.extend(['','Do not infer a 2x speedup just from selecting FP8 or gfx1201; both are present already.',
             'An independent linear prototype is NOT a full Swin replacement. GPU numerical validation and model mapping remain separate gates.'])
(OUT/'AUDIT.md').write_text('\n'.join(text)+'\n',encoding='utf-8')
