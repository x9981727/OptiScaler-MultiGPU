from pathlib import Path
import hashlib, re, sys

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

texts = {}
for name, sha in expected.items():
    path = core / name
    data = path.read_bytes()
    got = blob_sha(data)
    if got != sha:
        raise SystemExit(f'{name}: expected post-r2 blob {sha}, got {got}')
    texts[name] = data.decode('utf-8-sig').replace('\r\n', '\n').replace('\r', '\n')

parts = sorted(Path(__file__).parent.glob('xessfg-native-r3.patch.part*'))
if len(parts) != 6:
    raise SystemExit(f'expected 6 patch parts, found {len(parts)}')
patch = b''.join(p.read_bytes() for p in parts).decode('utf-8').replace('\r\n', '\n').replace('\r', '\n')

file_re = re.compile(r'^--- a/src/Magpie\.Core/([^\n]+)\n\+\+\+ b/src/Magpie\.Core/\1\n', re.M)
hunk_re = re.compile(r'^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@[^\n]*\n', re.M)
file_matches = list(file_re.finditer(patch))
if len(file_matches) != len(expected):
    raise SystemExit(f'expected {len(expected)} file sections, found {len(file_matches)}')

for fi, fm in enumerate(file_matches):
    name = fm.group(1)
    if name not in texts:
        raise SystemExit(f'unexpected r3 target {name}')
    section_end = file_matches[fi + 1].start() if fi + 1 < len(file_matches) else len(patch)
    section = patch[fm.end():section_end]
    hunks = list(hunk_re.finditer(section))
    if not hunks:
        raise SystemExit(f'{name}: no hunks found')
    text = texts[name]
    applied = 0
    line_delta = 0
    for hi, hm in enumerate(hunks):
        old_start = int(hm.group(1))
        body_end = hunks[hi + 1].start() if hi + 1 < len(hunks) else len(section)
        body = section[hm.end():body_end]
        old_lines, new_lines = [], []
        for raw in body.splitlines(keepends=True):
            if raw.startswith('\\ No newline at end of file'):
                continue
            if not raw:
                continue
            tag = raw[0]
            payload = raw[1:]
            if tag in (' ', '-'):
                old_lines.append(payload)
            if tag in (' ', '+'):
                new_lines.append(payload)
            if tag not in (' ', '-', '+'):
                raise SystemExit(f'{name}: malformed hunk line {raw[:80]!r}')
        old = ''.join(old_lines)
        new = ''.join(new_lines)
        if not old:
            raise SystemExit(f'{name}: empty old block is not allowed')

        positions = []
        pos = text.find(old)
        while pos != -1:
            positions.append(pos)
            pos = text.find(old, pos + 1)
        if not positions:
            raise SystemExit(f'{name}: hunk {hi + 1} old block not found')
        if len(positions) == 1:
            chosen = positions[0]
        else:
            expected_line = old_start + line_delta
            ranked = sorted(
                (abs((text.count('\n', 0, p) + 1) - expected_line), p) for p in positions
            )
            if len(ranked) > 1 and ranked[0][0] == ranked[1][0]:
                raise SystemExit(
                    f'{name}: hunk {hi + 1} has ambiguous nearest matches at distance {ranked[0][0]}'
                )
            chosen = ranked[0][1]
            actual_line = text.count('\n', 0, chosen) + 1
            print(
                f'{name}: hunk {hi + 1} disambiguated {len(positions)} matches; '
                f'expected line {expected_line}, chose line {actual_line}'
            )
        text = text[:chosen] + new + text[chosen + len(old):]
        line_delta += len(new_lines) - len(old_lines)
        applied += 1
    texts[name] = text
    print(f'{name}: applied {applied} deterministic hunks')

for name, text in texts.items():
    (core / name).write_text(text, encoding='utf-8', newline='\n')

checks = {
    'PresenterBase.h': 'SetNativeFrameGuidance',
    'XeSSFGPresenter.cpp': 'RecreateProxyForNativeFlags',
    'Renderer.cpp': '_sharedNativeGuidanceValid',
}
for name, marker in checks.items():
    if marker not in texts[name]:
        raise SystemExit(f'{name}: missing marker {marker}')
print('XeSSFG NativeBridge r3 deterministic patch applied: native depth + raw MV scale + camera + inverted-depth reinit')
