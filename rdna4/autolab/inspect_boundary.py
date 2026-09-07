# SPDX-License-Identifier: MIT
"""Static boundary investigation; downloaded runtime is data and is never executed."""
import hashlib,io,json,platform,re,struct,subprocess,tempfile,urllib.request
from pathlib import Path
import msgpack
from elftools.elf.elffile import ELFFile
from elftools.common.exceptions import ELFError
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'reports';OUT.mkdir(parents=True,exist_ok=True)
URL='https://raw.githubusercontent.com/MatheusGViana/dlss-5-amd-project/04d8b83f26adcbdd489137ea27a3ce59fc75eab4/version.dll'
with urllib.request.urlopen(URL,timeout=90) as f:data=f.read(32*1024*1024)
assert hashlib.sha256(data).hexdigest()=='106223723fd9266c44d38dc2fb77933948ab37803f46bfcea2bae3a0a474ac84'
code=None;meta=None;start=0
while True:
    at=data.find(b'\x7fELF',start)
    if at<0:break
    start=at+4
    try:
        if data[at+4:at+6]!=b'\x02\x01':continue
        off=struct.unpack_from('<Q',data,at+40)[0];es,n=struct.unpack_from('<HH',data,at+58)
        size=off+es*n
        if es!=64 or n>4096 or size<64 or at+size>len(data):continue
        raw=data[at:at+size]
        if hashlib.sha256(raw).hexdigest()!='dd38e6ede167c7a5886ae5d079022f63065b6775384d7588f185036b55877afb':continue
        assert code is None
        code=raw
        elf=ELFFile(io.BytesIO(code))
        for section in elf.iter_sections():
            if section.header.sh_type=='SHT_NOTE':
                for note in section.iter_notes():
                    if note['n_name']=='AMDGPU':meta=msgpack.unpackb(note['n_desc'],raw=False,strict_map_key=False)
    except (ELFError,ValueError,struct.error):continue
assert code is not None and meta is not None
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'input.hsaco';p.write_bytes(code)
    text=subprocess.check_output(['llvm-objdump-19','-d','--mcpu=gfx1201',str(p)],text=True)
parts=re.split(r'^[0-9a-fA-F]+ <([^>]+)>:\s*$',text,flags=re.M)
functions=dict(zip(parts[1::2],parts[2::2]))
symbol='_Z10k_swin_varILi32ELb1EEv9VarParams';lines=functions[symbol].splitlines()
selected=set(range(min(160,len(lines))))
for i,line in enumerate(lines):
    if 'global_store' in line or 'buffer_store' in line or 'flat_store' in line:
        selected.update(range(max(0,i-26),min(len(lines),i+12)))
excerpt='\n'.join(f'{i+1:5d} {lines[i]}' for i in sorted(selected))
(OUT/'special-swin-store-paths.txt').write_text(excerpt+'\n')
report={'scope':'Static ISA/resource inspection; no full GPU instruction emulator or target performance model',
 'platform':platform.platform(),'kfd_present':Path('/dev/kfd').exists(),'dri_present':Path('/dev/dri').exists(),
 'gpu_executed':False,'original_code_sha256':hashlib.sha256(code).hexdigest(),'symbol':symbol,
 'disassembly_lines':len(lines),'metadata':next(k for k in meta['amdhsa.kernels'] if k['.name']==symbol)}
(OUT/'boundary-static.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='metadata'},indent=2),flush=True)
print('=== INITIAL SCALAR LOADS AND CONTROL FLOW ===',flush=True)
print('\n'.join(f'{i+1:5d} {s}' for i,s in enumerate(lines[:100])),flush=True)
print('=== FINAL OUTPUT REGION STORE PATHS (STATIC, NOT EXECUTED) ===',flush=True)
print('\n'.join(excerpt.splitlines()[-260:]),flush=True)
