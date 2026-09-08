from pathlib import Path
import hashlib, subprocess, sys, tempfile

if len(sys.argv) != 2:
    raise SystemExit('usage: apply_xessfg_native_r3.py <MagpieRoot>')
root = Path(sys.argv[1]).resolve()
core = root / 'src' / 'Magpie.Core'
expected = {
    'PresenterBase.h': '083eb14537af02a5664fff4e2409f0e71993c72e',
    'XeSSFGPresenter.h': '4d2354ca35825d9a3c526d934daad5cd48d0183f',
    'XeSSFGPresenter.cpp': 'fac3c8d0d0ada389c52e0e536044341817743894',
    'Renderer.h': '9341988ba69ef9b4c804aaafac3604cb2f21a4fb',
    'Renderer.cpp': 'dc1dc5b421f1ad79d020e32e01b64b6b5c91270d',
}

def blob_sha(data: bytes) -> str:
    return hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()

for name, sha in expected.items():
    path = core / name
    data = path.read_bytes()
    got = blob_sha(data)
    if got != sha:
        raise SystemExit(f'{name}: expected post-r2 blob {sha}, got {got}')
    text = data.decode('utf-8-sig').replace('\r\n', '\n')
    path.write_text(text, encoding='utf-8', newline='')

parts = sorted(Path(__file__).parent.glob('xessfg-native-r3.patch.part*'))
if len(parts) != 6:
    raise SystemExit(f'expected 6 patch parts, found {len(parts)}')
patch = b''.join(p.read_bytes() for p in parts)
with tempfile.NamedTemporaryFile(delete=False, suffix='.patch') as f:
    f.write(patch)
    patch_path = Path(f.name)
try:
    # The r3 patch was assembled from reviewed chunks and some hunk line counts
    # became stale while the contents remained intact. --recount recomputes only
    # those counts; --check still requires every context line to match the pinned
    # post-r2 source before anything is written.
    common = ['git', '-C', str(root), 'apply', '--recount', '--whitespace=nowarn']
    subprocess.run([*common, '--check', str(patch_path)], check=True)
    subprocess.run([*common, str(patch_path)], check=True)
finally:
    patch_path.unlink(missing_ok=True)

checks = {
    'PresenterBase.h': 'SetNativeFrameGuidance',
    'XeSSFGPresenter.cpp': 'RecreateProxyForNativeFlags',
    'Renderer.cpp': '_sharedNativeGuidanceValid',
}
for name, marker in checks.items():
    if marker not in (core / name).read_text(encoding='utf-8'):
        raise SystemExit(f'{name}: missing marker {marker}')
print('XeSSFG NativeBridge r3 patch applied: native depth + raw MV scale + camera + inverted-depth reinit')
