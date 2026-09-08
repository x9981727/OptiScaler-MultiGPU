# SPDX-License-Identifier: MIT
"""Run after clean r52, r6 and r61 preparation. Does not execute a GPU."""
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parent
forward=root.parent/'forward_r5'
s=(forward/'forward_r61.cpp').read_text(encoding='utf-8').replace('\r\n','\n')
expected='ace61c06324c8e0d72816f84ebbfb88f1718a356ece5c79c6584b7429f684f20'
assert hashlib.sha256(s.encode()).hexdigest()==expected,'Unexpected generated r6.1 source'
def once(old,new):
    global s
    assert s.count(old)==1,'Nonunique r6.2 patch target: '+old[:100]
    s=s.replace(old,new)
once('#include "../autolab_r61/diagnostics.hpp"','#include "../autolab_r61/diagnostics.hpp"\n#include "../autolab_r62/control_probe.hpp"')
once('report["negative_control"]=r6_trace(replay,input,state,model.packed,out,report);',
     'report["negative_control"]=r62_control_probes(replay,input,state,model.packed,out,report);')
once('report["boundary_probe"]=r61_boundary_probes(replay,input,state,model.packed,out,report);',
     '// r6.2 uses fresh-prefix terminal-control experiments instead of repeating the r6.1 buffer sweep.')
once('report["r61_measurement_and_probe_checks"]=r61::self_test();',
     'report["r61_measurement_and_probe_checks"]=r61::self_test();\n        report["r62_control_restoration_checks"]=r62::self_test();')
once('report["status"]="autolab_blocked_output_sensitivity";',
     'report["status"]="r62_diagnostics_complete_original_gate_closed";')
s=s.replace('r6.1','r6.2').replace('rdna4-r61-result.json','rdna4-r62-result.json').replace('rdna4-r61-console.txt','rdna4-r62-console.txt')
s=s.replace('r6.2 diagnostics completed: original output-sensitivity gate remains closed; boundary probes and raw timestamps saved',
            'r6.2 completed: original gate remains closed; explicit control experiments and profile saved separately')
(forward/'forward_r62.cpp').write_text(s,encoding='utf-8',newline='\n')
out=root/'reports';out.mkdir(exist_ok=True)
report={'revision':'r6.2','source_r61_sha256':expected,'generated_source_sha256':hashlib.sha256(s.encode()).hexdigest(),
 'original_control_unchanged_in_baseline':True,'experimental_changed_bytes':[136,137,138,139],
 'experimental_float_values':[0.125,1.0],'game_setting_values_verified':False,
 'gpu_code_changes':False,'gpu_executed_in_build':False,'game_release_approved':False,
 'new_features':['Explicit zero/nonzero output-control experiments','Two repeated Head perturbations per control',
 'Two output coverage sentinels per control','Whole-allocation candidate comparison under sensitive control 1',
 'RAII restoration of argument storage without pointer invalidation','Separate original gate and experimental results']}
(out/'r62-source-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
