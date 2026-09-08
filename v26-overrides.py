from pathlib import Path
root=Path('v26')
p=root/'install_game.py';s=p.read_text(encoding='utf-8')
a='def install(target:Path, root:Path):\n';b='def restore(target:Path):\n'
assert s.count(a)==1 and s.count(b)==1
s=s.replace(a,a+'    target=target.resolve(); root=root.resolve()\n').replace(b,b+'    target=target.resolve()\n')
p.write_text(s,encoding='utf-8',newline='\n')
# Keep this build correction beside the emitted readable source.
(root/'build_corrections.py').write_text(Path(__file__).read_text(encoding='utf-8'),encoding='utf-8')
print('Installer paths canonicalized on both install and restore.')
