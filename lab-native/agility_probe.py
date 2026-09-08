from pathlib import Path
import json,hashlib,urllib.request,zipfile,sys
root=Path(sys.argv[1])/'samples/basic_sample_frame_generation'
url='https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.619.5/microsoft.direct3d.d3d12.1.619.5.nupkg'
package=Path('agility-lab.nupkg')
with urllib.request.urlopen(url,timeout=60) as r:package.write_bytes(r.read())
out=Path('agility-lab')
with zipfile.ZipFile(package) as z:
 for info in z.infolist():
  name=info.filename
  if name.startswith('build/native/bin/x64/') and name.endswith(('D3D12Core.dll','d3d12SDKLayers.dll')) or name.lower().endswith(('license.txt','license.rtf','third-party-notices.txt','thirdpartynotices.txt')):
   target=out/Path(name).name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(z.read(info))
for name in ['D3D12Core.dll','d3d12SDKLayers.dll']:
 if not (out/name).is_file():raise RuntimeError('Missing app-local diagnostic runtime '+name)
cpp=root/'AgilityLab.cpp'
cpp.write_text('#include <windows.h>\nextern "C" {\n__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;\n__declspec(dllexport) extern const char* D3D12SDKPath = ".\\\\D3D12-Lab\\\\";\n}\n',encoding='utf-8')
cm=root/'CMakeLists.txt';cm.write_text(cm.read_text()+'\ntarget_sources(basic_xess_fg_sample PRIVATE AgilityLab.cpp)\n',encoding='utf-8')
(out/'AGILITY-LAB-MANIFEST.json').write_text(json.dumps({'kind':'isolated_debug_runtime_not_game_update','source':url,'package_sha256':hashlib.sha256(package.read_bytes()).hexdigest(),'version':'1.619.5','system_installed':False,'game_installed':False},indent=2))
print('App-local Agility diagnostic runtime prepared, no system changes.')
