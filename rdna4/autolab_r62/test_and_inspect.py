# SPDX-License-Identifier: MIT
import contextlib,hashlib,io,json,os,re,runpy,struct,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
out=root/'reports';out.mkdir(exist_ok=True)
# Reuse the previously reviewed r52 SPARSE fixture, not an unverified long-hex
# transcription. Production separately verifies the real selected plan's hash
# and its byte136 value before executing the control experiment.
fixture_source=(root.parent/'forward_r5/plan_regression_r52.hpp').read_text().replace('\r\n','\n')
fields=re.findall(r'last\.replace\((\d+)\*2,8,"([0-9a-f]{8})"\);',fixture_source)
assert fields==[('24','80070000'),('28','80040000'),('40','20000000'),('144','01000000')]
raw=bytearray(168)
for at,value in fields:raw[int(at):int(at)+4]=bytes.fromhex(value)
assert struct.unpack_from('<I',raw,136)[0]==0
assert struct.unpack_from('<I',raw,140)[0]==0
assert struct.unpack_from('<I',raw,144)[0]==1
fixture={'provenance':'Sparse terminal-argument regression fixture constructed from the reported fields already stored in plan_regression_r52.hpp. NOT raw checkpoint bytes read in this CI run.',
 'fixture_source_sha256':hashlib.sha256(fixture_source.encode()).hexdigest(),
 'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'command_index':157,
 'scalars':[{ 'offset':p,'u32':struct.unpack_from('<I',raw,p)[0],'f32':struct.unpack_from('<f',raw,p)[0]} for p in (136,140,144)],
 'complete_checkpoint_read':False,'gpu_executed':False,'intended_game_control_value_verified':False}
(out/'reported-terminal-fixture.json').write_text(json.dumps(fixture,indent=2)+'\n')
print('SPARSE REGRESSION FIXTURE',json.dumps(fixture),flush=True)
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
 'gpu_executed':False,'argument_136_f32_in_sparse_fixture':0.0,'actual_selected_plan_control_read_in_ci':False}
(out/'control136-static-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2));print(text)
