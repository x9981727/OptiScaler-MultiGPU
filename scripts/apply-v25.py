"""Reconstruct the reviewed v25 delta against exact v24 sources.
No relaxed checksums, source-frame shedding, resolution change or fixed FPS cap.
"""
from pathlib import Path
import base64
import gzip
import hashlib
import subprocess
import sys

kit = Path(__file__).resolve().parents[1]
encoded = ''.join((kit / 'v25/consumer-pacing.patch.gz.b64').read_text().split())
# Correct two transcription errors in the transport text. This normalization is
# deterministic; the entire decoded diff must still match the reviewed SHA256.
encoded = encoded.replace('/DS1Vyby', '/DS1FVyby').replace('WS1a3fBj', 'WS1a3dBj')
payload = gzip.decompress(base64.b64decode(encoded, validate=True))
expected = 'f628acd2a4951ab859ea58b84a5243e6f077847d07070f66cf9815a14ebc2986'
actual = hashlib.sha256(payload).hexdigest()
if actual != expected:
    raise RuntimeError(f'v25 source checksum mismatch: {actual}')
patch = kit / 'v25/consumer-pacing.patch'
patch.write_bytes(payload)
print(f'v25 source SHA256 verified: {actual}; {len(payload)} bytes', flush=True)
if '--check-payload' in sys.argv:
    sys.exit(0)
command = ['git', '-C', str(kit / 'upstream'), 'apply', '--ignore-space-change']
subprocess.run(command + ['--check', str(patch)], check=True)
subprocess.run(command + [str(patch)], check=True)
print('v25 applied: consumer-owned XeLL stream, exact input ownership, MV flags and sampled diagnostics')
