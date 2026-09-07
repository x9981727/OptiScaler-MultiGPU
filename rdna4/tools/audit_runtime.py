"""Audit the pinned runtime without executing it or distributing model weights."""
import collections
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import subprocess
import urllib.request

import msgpack
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'audit'
OUT.mkdir(parents=True, exist_ok=True)
COMMIT = '04d8b83f26adcbdd489137ea27a3ce59fc75eab4'
URL = f'https://raw.githubusercontent.com/MatheusGViana/dlss-5-amd-project/{COMMIT}/version.dll'
data = urllib.request.urlopen(URL, timeout=60).read()
sha = hashlib.sha256(data).hexdigest()
assert sha == '106223723fd9266c44d38dc2fb77933948ab37803f46bfcea2bae3a0a474ac84', sha
report = {'upstream_commit': COMMIT, 'runtime_sha256': sha, 'hardware_tested': False, 'objects': []}
start = 0
while True:
    start = data.find(b'\x7fELF', start)
    if start < 0:
        break
    off = start
    start += 4
    try:
        if data[off+4:off+6] != b'\x02\x01':
            continue
        shoff = struct.unpack_from('<Q', data, off+40)[0]
        shentsize, shnum = struct.unpack_from('<HH', data, off+58)
        size = shoff + shentsize * shnum
        if size <= 64 or off + size > len(data):
            continue
        raw = data[off:off+size]
        elf = ELFFile(io.BytesIO(raw))
        if elf.header['e_machine'] != 'EM_AMDGPU':
            continue
        notes = []
        kernels = []
        target = ''
        for section in elf.iter_sections():
            if section.header.sh_type != 'SHT_NOTE':
                continue
            for note in section.iter_notes():
                if note['n_name'] == 'AMDGPU':
                    meta = msgpack.unpackb(note['n_desc'], raw=False, strict_map_key=False)
                    notes.append(meta)
                    target = meta.get('amdhsa.target', target)
                    kernels.extend(meta.get('amdhsa.kernels', []))
        item = {'offset': off, 'size': size, 'target': target, 'flags': elf.header['e_flags'], 'kernels': kernels}
        idx = len(report['objects'])
        objpath = OUT / f'original_{idx}.hsaco'
        objpath.write_bytes(raw)
        command = ['llvm-objdump-19', '-d', '--mcpu=gfx1201', str(objpath)]
        process = subprocess.run(command, capture_output=True, text=True)
        text = process.stdout
        (OUT / f'original_{idx}.isa.txt').write_text(text, encoding='utf-8')
        (OUT / f'original_{idx}.disasm-stderr.txt').write_text(process.stderr, encoding='utf-8')
        item['disassembler_exit_code'] = process.returncode
        chunks = re.split(r'^[0-9a-fA-F]+ <([^>]+)>:\s*$', text, flags=re.M)
        counts = []
        for i in range(1, len(chunks)-1, 2):
            name, body = chunks[i:i+2]
            ops = re.findall(r'\b(v_(?:swmmac|wmma|mfma)[a-zA-Z0-9_]*)\b', body)
            if 'k_swin_var' in name or ops:
                counts.append({'name': name, 'matrix_instruction_counts_static': dict(collections.Counter(ops)), 'disassembly_lines': len(body.splitlines())})
        item['matrix_instruction_inventory'] = counts
        report['objects'].append(item)
    except (ValueError, KeyError, struct.error) as exc:
        print('Rejected ELF candidate', off, type(exc).__name__)
(OUT / 'runtime-audit.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
assert report['objects'], 'No AMDGPU ELF objects recovered'
print('Runtime SHA256:', sha)
for obj in report['objects']:
    print('TARGET', obj['target'], 'FLAGS', obj['flags'], 'BYTES', obj['size'])
    for k in obj['kernels']:
        if 'k_swin_var' in k.get('.name', ''):
            print('KERNEL', json.dumps(k, sort_keys=True))
    for k in obj['matrix_instruction_inventory']:
        print('MATRIX', json.dumps(k, sort_keys=True))
print('NOTE: static instruction counts are not measured utilization or performance.')
