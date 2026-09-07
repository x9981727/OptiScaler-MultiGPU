"""Apply the full-resolution, no-source-shedding v24 COPY prefetch experiment.

The payload is a unified source diff, compressed to match this build kit's format.
It is checksum-verified, written to v24/exact-2x.patch for review, then checked and
applied against the fully reconstructed v23 tree. No frame-time/quality changes.
"""
from pathlib import Path
import base64
import gzip
import hashlib
import subprocess

kit = Path(__file__).resolve().parents[1]
encoded = ''.join((kit / 'v24/exact-2x.patch.gz.b64').read_text().split())
payload = gzip.decompress(base64.b64decode(encoded, validate=True))
expected = '38becc44e0545b0fa467fb9fbd04bf9316be112ac2e1c9f9d5fcd439b06b8d43'
if hashlib.sha256(payload).hexdigest() != expected:
    raise RuntimeError('v24 source diff checksum mismatch')
patch = kit / 'v24/exact-2x.patch'
patch.write_bytes(payload)
command = ['git', '-C', str(kit / 'upstream'), 'apply', '--ignore-space-change']
subprocess.run(command + ['--check', str(patch)], check=True)
subprocess.run(command + [str(patch)], check=True)
print('v24 applied: dedicated COPY prefetch + exact post-completion destination lookup; source shedding disabled')
