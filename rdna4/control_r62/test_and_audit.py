# SPDX-License-Identifier: MIT
"""Static evidence and CPU checks only. Never executes the upstream runtime."""
import contextlib,hashlib,io,json,re,runpy,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
out=root/'reports';out.mkdir(exist_ok=True)
source=out/'control_cpu.cpp'
source.write_text('#include "control_policy.hpp"\n#include <iostream>\nint main(){try{std::cout<<r62::self_test()<<" control checks passed; CPU only\\n";return 0;}catch(const std::exception& e){std::cerr<<e.what();return 1;}}\n')
rows=[]
for name,flags in [('debug',['-O0','-g']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']),('optimized',['-O2'])]:
    exe=out/('control-'+name)
    subprocess.run(['clang++-19','-std=c++20','-ffp-contract=off','-I',str(root),*flags,str(source),'-o',str(exe)],check=True)
    result=subprocess.check_output([str(exe)],text=True)
    rows.append({'build':name,'result':result.strip(),'gpu_executed':False});print(name,result.strip(),flush=True)
# Fix a reporting edge case in the generated experiment source. Partial original
# sensitivity must not be labelled a passed default gate.
exp=root/'experiment.hpp';text=exp.read_text()
a='report["original_default_output_sensitivity_passed"]=!zeroBlocked;'
b='report["original_default_output_sensitivity_passed"]=std::all_of(info["profiles"].begin(),info["profiles"].end(),[](const J& p){return p.at("scalar136").get<float>()!=0.0f || p.at("both_independent_head_perturbations_reach_output").get<bool>();});'
assert text.count(a)==1,'Unexpected experimental reporting source'
exp.write_text(text.replace(a,b),encoding='utf-8',newline='\n')
with contextlib.redirect_stdout(io.StringIO()):ns=runpy.run_path(str(root.parent/'autolab'/'inspect_boundary.py'))
lines=ns['lines'];joined='\n'.join(lines)
patterns={
 'load_final_rgb_and_output_pointers':r's_load_b128\s+s\[4:7\],\s*s\[0:1\],\s*0x70',
 'load_scalar136_into_s12':r's_load_b96\s+s\[12:14\],\s*s\[0:1\],\s*0x88',
 'rgb_encode':r'v_fmaak_f32\s+v2,\s*s30,\s*v2,\s*0xbd800000',
 'network_contribution_v2':r'v_fmac_f32_e32\s+v2,\s*s12,\s*v3',
 'network_contribution_v3':r'v_fmac_f32_e32\s+v3,\s*s12,\s*v11',
 'network_contribution_v4':r'v_fmac_f32_e32\s+v4,\s*s12,\s*v11',
 'output_decode_clamp':r'v_fma_f32\s+v2,\s*0x41000000,\s*v2,\s*0.5 clamp',
 'one_eighth_constant':r's_mov_b32\s+s30,\s*0x3e000000'}
evidence=[]
for label,pattern in patterns.items():
    found=[{'line':i+1,'isa':line.strip()} for i,line in enumerate(lines) if re.search(pattern,line)]
    assert found,'Missing fixed original ISA evidence: '+label
    evidence.append({'meaning_assigned_by_analysis':label,'matches':found})
report={'scope':'Opcode and load-address matches in a checksum-pinned original. Supports a falsifiable scalar136 contribution hypothesis; not complete control-flow proof, game-default discovery or GPU validation.',
        'original_code_sha256':ns['report']['original_code_sha256'],
        'candidate_r4_code_unchanged':True,'hypothesized_final_gain_byte_offset':136,
        'model_source_observation':'User r6.1 preserves scalar136=0, scalar140=0, scalar144=1; only source RGB buffer changes the final observer in recorded probes',
        'synthetic_control_values':[0,0.03125,0.125,1],'game_default_inferred':False,
        'static_matches':evidence,'cpu_tests':rows,'gpu_executed':False,
        'compiled_experiment_header_sha256':hashlib.sha256(exp.read_bytes()).hexdigest()}
(out/'control-static-and-cpu-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2),flush=True)
