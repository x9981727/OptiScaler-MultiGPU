# SPDX-License-Identifier: MIT
import contextlib,hashlib,io,json,os,re,runpy,struct,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
out=root/'reports';out.mkdir(exist_ok=True)
# Exact normalized LAST command argument bytes from the user's r6 JSON.
# Relocated device pointers are zero in this representation; not real addresses.
reported='000000000000000000000000000000000000000000000000800700008004000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000'
raw=bytes.fromhex(reported)
assert len(raw)==168,(len(raw),'Reported terminal fixture copy is not 168 bytes')
assert struct.unpack_from('<I',raw,136)[0]==0
assert struct.unpack_from('<I',raw,140)[0]==0
assert struct.unpack_from('<I',raw,144)[0]==1
fixture={'provenance':'Exact normalized last command argument excerpt in user-supplied rdna4-r6-result.json; not the complete checkpoint',
 'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'command_index':157,
 'scalars':[{ 'offset':p,'u32':struct.unpack_from('<I',raw,p)[0],'f32':struct.unpack_from('<f',raw,p)[0]} for p in (136,140,144)],
 'gpu_executed':False,'intended_game_control_value_verified':False}
(out/'reported-terminal-fixture.json').write_text(json.dumps(fixture,indent=2)+'\n')
print('REPORTED ARGUMENT FIXTURE',json.dumps(fixture),flush=True)
source=root/'control_cpu.cpp'
source.write_text('#include "control_policy.hpp"\n#include <iostream>\nint main(){try{auto n=r62::self_test();std::cout<<"{\\"checks\\":"<<n<<",\\"gpu_executed\\":false}"<<std::endl;return 0;}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}\n')
results={}
for mode,flags in [('debug',['-O0']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']),('optimized',['-O2'])]:
    exe=out/('control-'+mode)
    subprocess.run(['clang++-19','-std=c++20','-Wall','-Wextra','-Werror',*flags,str(source),'-o',str(exe)],check=True)
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1',UBSAN_OPTIONS='halt_on_error=1')
    completed=subprocess.run([str(exe)],capture_output=True,text=True,check=True,env=env)
    result=json.loads(completed.stdout);assert result['checks']>100 and not result['gpu_executed']
    results[mode]=result;print(mode,result,flush=True)
(out/'control-policy-tests.json').write_text(json.dumps({'configurations':results,'target_gpu_executed':False},indent=2)+'\n')
with contextlib.redirect_stdout(io.StringIO()):
    ns=runpy.run_path(str(root.parent/'autolab/inspect_boundary.py'))
lines=ns['lines']
assert ns['report']['original_code_sha256']=='dd38e6ede167c7a5886ae5d079022f63065b6775384d7588f185036b55877afb'
loads=[(i,line) for i,line in enumerate(lines) if re.search(r's_load_b96\s+s\[12:14\],.*0x88\s',line)]
assert len(loads)==1,'Expected control-load instruction absent'
uses=[(i,line) for i,line in enumerate(lines) if re.search(r'v_(?:fmac_f32_e32|mul_f32_e32)\s+v[234],\s+s12,',line)]
assert len(uses)==6,'Expected output scale-use instructions differ'
interesting={}
for i,line in loads+uses:
    for j in range(max(0,i-3),min(len(lines),i+4)):interesting[j]=lines[j]
text='\n'.join(f'{j+1:5d} {line}' for j,line in sorted(interesting.items()))+'\n'
(out/'control136-isa-evidence.txt').write_text(text)
report={'original_code_sha256':ns['report']['original_code_sha256'],
 'symbol':'_Z10k_swin_varILi32ELb1EEv9VarParams','load_instruction_line':loads[0][0]+1,
 'observed_output_scale_uses':[{'line':i+1,'isa':s} for i,s in uses],
 'scope':'Static instruction matches support a byte136 output-scale hypothesis. This is not complete control-flow/taint proof and does not establish the intended host/game parameter value.',
 'gpu_executed':False,'original_argument_136_f32_from_reported_fixture':0.0}
(out/'control136-static-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2));print(text)
