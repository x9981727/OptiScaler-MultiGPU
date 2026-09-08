# SPDX-License-Identifier: MIT
"""Run after prepare_r52.py and prepare_autolab.py on a clean checkout."""
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parent
forward=root.parent/'forward_r5'
s=(forward/'forward_autolab.cpp').read_text(encoding='utf-8').replace('\r\n','\n')
expected='f3748d147cb985091411b22b35527bd8fabeaa7633597ba1445c607fa6df4436'
assert hashlib.sha256(s.encode()).hexdigest()==expected,'Unexpected generated r6 source'
def once(a,b):
    global s
    assert s.count(a)==1,'Nonunique r6.1 patch target: '+a[:90]
    s=s.replace(a,b)
once('#include "../autolab/lab_policy.hpp"','#include "../autolab/lab_policy.hpp"\n#include "../autolab_r61/measurement_policy.hpp"')
once('#include "../autolab/trace_support.hpp"','#include "../autolab/trace_support.hpp"\n#include "../autolab_r61/diagnostics.hpp"')
once('report["instrumented_profile"]=r6_profile(replay,input,original.start,original.stop);', '''// Save the established sensitivity gate BEFORE optional timing diagnostics.
        report["release_gate"]={{"approved",false},{"reason","Experimental fixed-plan diagnostics; no game release is approved"}};
        save_r5(out,report);
        report["boundary_probe"]=r61_boundary_probes(replay,input,state,model.packed,out,report);
        report["instrumented_profile"]=r61_profile(replay,input,state,model.packed,out,report);''')
once('report["candidate_policy_checks"]=virtualTests.policy_checks;', 'report["candidate_policy_checks"]=virtualTests.policy_checks;\n        report["r61_measurement_and_probe_checks"]=r61::self_test();')
s=s.replace('r6.0','r6.1').replace('rdna4-r6-result.json','rdna4-r61-result.json').replace('rdna4-r6-console.txt','rdna4-r61-console.txt')
s=s.replace('r6 stopped automatically: output sensitivity gate failed; causal trace and per-command timings saved',
            'r6.1 diagnostics completed: original output-sensitivity gate remains closed; boundary probes and raw timestamps saved')
(forward/'forward_r61.cpp').write_text(s,encoding='utf-8',newline='\n')
out=root/'reports';out.mkdir(exist_ok=True)
report={'revision':'r6.1','input_r6_source_sha256':expected,'compiled_source_sha256':hashlib.sha256(s.encode()).hexdigest(),
        'gpu_kernel_changes':False,'original_argument_changes':False,
        'new_features':['Independent final-boundary buffer probes','Confirmed repeated probe outputs and restoration',
                        'Distinct queued timing events per boundary','Every raw timestamp retained including zero',
                        'No silent epsilon replacement or censored-sample win','Incremental diagnostics before failures'],
        'measurement_passes':7,'gpu_executed_by_build':False,'game_tested':False,'game_release_approved':False}
(out/'r61-source-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
