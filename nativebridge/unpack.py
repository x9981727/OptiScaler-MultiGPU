"""Expand the audited source bundle, checking its exact digest and paths."""
from pathlib import Path, PurePosixPath
import hashlib,json,lzma
root=Path(__file__).resolve().parent
blob=b''.join((root/'bundle'/f'source.xz.{i}').read_bytes() for i in range(4))
if hashlib.sha256(blob).hexdigest()!='995e24de7f042cef38db5000c922cbe41b6744dc89bc3bbd11216bebc8bd058c':
    raise SystemExit('Source bundle checksum mismatch')
files=json.loads(lzma.decompress(blob))
for name,content in files.items():
    parts=PurePosixPath(name)
    if parts.is_absolute() or '..' in parts.parts or '\\' in name or ':' in name:
        raise SystemExit('Unsafe source path')
    target=root/'source'/name
    target.parent.mkdir(parents=True,exist_ok=True)
    target.write_text(content,encoding='utf-8',newline='\n')
print(f'Extracted {len(files)} verified source files')
