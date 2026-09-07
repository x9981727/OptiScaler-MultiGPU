# SPDX-License-Identifier: MIT
"""Development-only static inspection. Never execute the upstream DLL.
Do not use symbol names or header-looking numbers as proof of tensor semantics.
"""
import hashlib, io, json, re, struct, subprocess, urllib.parse, urllib.request
from pathlib import Path
import msgpack
from elftools.elf.elffile import ELFFile
from elftools.common.exceptions import ELFError

REPO='MatheusGViana/dlss-5-amd-project'
REV='04d8b83f26adcbdd489137ea27a3ce59fc75eab4'
DLL_SHA='106223723fd9266c44d38dc2fb77933948ab37803f46bfcea2bae3a0a474ac84'
WEIGHT_SHA='6bf8dc931ef3ccffe18c82de26ab374156e7f19539ffcf8eabaa25dca5cf15ab'
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'inspection';OUT.mkdir(parents=True,exist_ok=True)

def get(url):
    request=urllib.request.Request(url,headers={'User-Agent':'RDNA4-layer-contract-inspector'})
    with urllib.request.urlopen(request,timeout=120) as response: return response.read()

def run():
    dll=get(f'https://raw.githubusercontent.com/{REPO}/{REV}/version.dll')
    if hashlib.sha256(dll).hexdigest()!=DLL_SHA: raise ValueError('Wrong original runtime')
    selected=None;meta=None;start=0
    while True:
        off=dll.find(b'\x7fELF',start)
        if off<0: break
        start=off+4
        try:
            if dll[off+4:off+6]!=b'\x02\x01':continue
            shoff=struct.unpack_from('<Q',dll,off+40)[0]
            es,n=struct.unpack_from('<HH',dll,off+58)
            size=shoff+es*n
            if size<64 or off+size>len(dll):continue
            raw=dll[off:off+size];elf=ELFFile(io.BytesIO(raw))
            if elf.header['e_machine']!='EM_AMDGPU':continue
            for section in elf.iter_sections():
                if section.header.sh_type!='SHT_NOTE':continue
                for note in section.iter_notes():
                    if note['n_name']!='AMDGPU':continue
                    candidate=msgpack.unpackb(note['n_desc'],raw=False,strict_map_key=False)
                    if candidate.get('amdhsa.target')=='amdgcn-amd-amdhsa--gfx1201':
                        if selected is not None:raise RuntimeError('Duplicate target')
                        selected=raw;meta=candidate
        except (ELFError,ValueError,struct.error):continue
    if selected is None:raise RuntimeError('gfx1201 code object missing')
    obj=OUT/'original_gfx1201.hsaco';obj.write_bytes(selected)
    asm=subprocess.check_output(['llvm-objdump-19','-d','--mcpu=gfx1201',str(obj)],text=True)
    parts=re.split(r'^[0-9a-fA-F]+ <([^>]+)>:\s*$',asm,flags=re.M)
    funcs=dict(zip(parts[1::2],parts[2::2]))
    (OUT/'kernel-metadata.json').write_text(json.dumps(meta,indent=2))
    print('=== ORIGINAL LAYER METADATA ===',flush=True)
    for k in meta['amdhsa.kernels']:
        if any(s in k['.name'] for s in ['k_repack','k_final_head','k_swin_var']):print(json.dumps(k),flush=True)
    for name in ['_Z8k_repack12RepackParams','_Z12k_final_head10HeadParams','_Z10k_swin_varILi32ELb0EEv9VarParams']:
        text=funcs[name];(OUT/(name+'.isa.txt')).write_text(text)
        lines=text.splitlines()
        count=len(lines) if 'repack' in name or 'final_head' in name else 260
        print('=== ISA '+name+' total_lines='+str(len(lines))+' ===',flush=True)
        print('\n'.join(lines[:count]),flush=True)
    tree=json.loads(get(f'https://api.github.com/repos/{REPO}/git/trees/{REV}?recursive=1'))
    if tree.get('truncated'):raise RuntimeError('Incomplete upstream tree')
    paths=[e['path'] for e in tree['tree'] if e['path'].endswith('dlssnr_on_amd_weights.bin')]
    print('WEIGHT PATHS',json.dumps(paths),flush=True)
    weights=None;chosen=None
    for path in paths:
        url=f'https://raw.githubusercontent.com/{REPO}/{REV}/'+urllib.parse.quote(path)
        w=get(url)
        if w.startswith(b'version https://git-lfs.github.com/spec/v1'):
            w=get(f'https://media.githubusercontent.com/media/{REPO}/{REV}/'+urllib.parse.quote(path))
        if hashlib.sha256(w).hexdigest()==WEIGHT_SHA:weights=w;chosen=path;break
    if weights is None:raise RuntimeError('Checksum-pinned weights not found')
    if weights[:8]!=b'DLSSNRW1':raise ValueError('Archive magic')
    count,base=struct.unpack_from('<II',weights,8);pos=16;entries=[]
    for _ in range(count):
        n=weights[pos];pos+=1;name=weights[pos:pos+n].decode('ascii');pos+=n
        off,size=struct.unpack_from('<QQ',weights,pos);pos+=16
        if base+off+size>len(weights):raise ValueError('Entry range')
        entry={'name':name,'offset':off,'bytes':size};entries.append(entry)
        if name in ['block0.layer0.layer','block1.layer0.layer','block70.layer0.layer']:
            header=weights[base+off:base+off+min(size,256)]
            entry['initial_u32_uninterpreted']=list(struct.unpack('<'+'I'*(len(header)//4),header))
            entry['initial_f32_uninterpreted']=[repr(v) for v in struct.unpack('<'+'f'*(len(header)//4),header)]
    if pos!=base:raise ValueError('Directory mismatch')
    report={'scope':'Static header and ABI inspection only','runtime_sha256':DLL_SHA,
        'code_sha256':hashlib.sha256(selected).hexdigest(),'weights_sha256':WEIGHT_SHA,
        'weights_path':chosen,'weights_bytes':len(weights),'entries':entries,'gpu_executed':False}
    (OUT/'weight-structure.json').write_text(json.dumps(report,indent=2))
    print('=== WEIGHT DIRECTORY AND SELECTED RAW HEADER WORDS ===',flush=True)
    print(json.dumps(report,indent=2),flush=True)
    # Full bytes are transient CI inputs, not a distributable optimized runtime.
    obj.unlink()

if __name__=='__main__':run()
