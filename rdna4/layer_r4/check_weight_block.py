# SPDX-License-Identifier: MIT
"""Validate a pinned local-contract fixture, without publishing weights or implying full-forward binding."""
import hashlib,json,struct,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parent
url='https://media.githubusercontent.com/media/MatheusGViana/dlss-5-amd-project/04d8b83f26adcbdd489137ea27a3ce59fc75eab4/OptiScaler-AMD-PreSR-Multipass-v1.1/dlssnr_on_amd_weights.bin'
with urllib.request.urlopen(url,timeout=120) as f: data=f.read()
expected='6bf8dc931ef3ccffe18c82de26ab374156e7f19539ffcf8eabaa25dca5cf15ab'
assert hashlib.sha256(data).hexdigest()==expected
assert data[:8]==b'DLSSNRW1'
count,base=struct.unpack_from('<II',data,8);pos=16;chosen=None
for _ in range(count):
    n=data[pos];pos+=1;name=data[pos:pos+n].decode('ascii');pos+=n
    off,size=struct.unpack_from('<QQ',data,pos);pos+=16
    assert base+off+size<=len(data)
    if name=='block30.layer4.layer':chosen=data[base+off:base+off+size]
assert pos==base and count==153 and chosen is not None and len(chosen)==524304
payload=chosen[:524288]
nans=sum((b&127)==127 for b in payload)
assert nans==0,'Basis interpretation needs revision: FP8 NaN encodings in selected matrix'
report={'source_commit':'04d8b83f26adcbdd489137ea27a3ce59fc75eab4','archive_bytes':len(data),
        'archive_sha256':expected,'selected_block':'block30.layer4.layer','block_bytes':len(chosen),
        'block_sha256':hashlib.sha256(chosen).hexdigest(),'matrix_bytes':len(payload),
        'fp8_nan_encodings':nans,'nonzero_magnitude_bytes':sum((b&127)!=0 for b in payload),
        'full_forward_binding_verified':False,'gpu_tested':False,
        'scope':'Real-model payload used as differential-test fixture; host binding and full network not validated'}
(ROOT/'build'/'weight-fixture-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
