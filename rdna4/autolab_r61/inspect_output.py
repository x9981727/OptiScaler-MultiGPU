# SPDX-License-Identifier: MIT
"""Static evidence only. Does not execute or alter original GPU code."""
import contextlib,io,json,runpy
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with contextlib.redirect_stdout(io.StringIO()):
    ns=runpy.run_path(str(root/'autolab/inspect_boundary.py'))
lines=ns['lines'];out=Path(__file__).resolve().parent/'reports';out.mkdir(exist_ok=True)
loads=[]
for i,line in enumerate(lines):
    if 's_load' in line:
        loads.append({'line':i+1,'isa':line})
print('=== ALL SCALAR LOADS ===')
for item in loads:print(item['line'],item['isa'])
print('=== OUTPUT EPILOGUE ===')
for i in range(4097,min(4430,len(lines))):print(i+1,lines[i])
report={'original_code_sha256':ns['report']['original_code_sha256'],'scope':'Static parameter loads, not dynamic data-flow verification','gpu_executed':False,'scalar_loads':loads}
(out/'output-parameter-loads.json').write_text(json.dumps(report,indent=2)+'\n')
(out/'output-epilogue.txt').write_text('\n'.join(f'{i+1} {lines[i]}' for i in range(4046,min(4430,len(lines))))+'\n')
