# SPDX-License-Identifier: MIT
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parent
forward=root.parent/'forward_r5'
s=(forward/'forward_r61.cpp').read_text(encoding='utf-8').replace('\r\n','\n')
expected='ace61c06324c8e0d72816f84ebbfb88f1718a356ece5c79c6584b7429f684f20'
assert hashlib.sha256(s.encode()).hexdigest()==expected,'Unexpected r6.1 source'
def once(a,b):
    global s
    assert s.count(a)==1,'Nonunique r6.2 patch target: '+a[:100]
    s=s.replace(a,b)
once('#include "../autolab_r61/diagnostics.hpp"','#include "../autolab_r61/diagnostics.hpp"\n#include "../control_r62/experiment.hpp"')
start=s.index('    const char* namesCase[]={')
end=s.index('\nint r5_delivery_entry(',start)
assert s[start:end].rstrip().endswith('}')
s=s[:start]+'''    r62_experiments(replay,model.packed,original.start,original.stop,out,report);
}
'''+s[end:]
once('report["r61_measurement_and_probe_checks"]=r61::self_test();',
     'report["r61_measurement_and_probe_checks"]=r61::self_test();\n        report["r62_control_policy_checks"]=r62::self_test();')
s=s.replace('r6.1','r6.2').replace('rdna4-r61-result.json','rdna4-r62-result.json').replace('rdna4-r61-console.txt','rdna4-r62-console.txt')
s=s.replace('RDNA4 r6.2 AutoLab - not a final game DLL','RDNA4 r6.2 Neural-Contribution Lab - EXPLICIT CONTROL EXPERIMENT, not a game DLL')
# Defaults state what an actual GPU run will do, even if file selection fails first.
s=s.replace('Original scalar argument bytes are not edited.', 'Original scalar bytes are preserved in baseline; only final scalar136 is varied in explicitly labelled r6.2 experimental profiles.')
s=s.replace('All 154 launches and 4 original D2D copies are timed.', 'All 154 launches and 4 original D2D copies are timed ONLY in the predeclared gain=1/32 experiment after both sensitivity checks; all modes use identical controls.')
(forward/'forward_r62.cpp').write_text(s,encoding='utf-8',newline='\n')
report={'revision':'r6.2','source_r61_sha256':expected,'compiled_source_sha256':hashlib.sha256(s.encode()).hexdigest(),
        'changes_original_checkpoint_on_disk':False,'changes_gpu_kernel':False,
        'experimental_argument_change':{'command':157,'argument':0,'byte_offset':136,'bytes':4,'values':[0,0.03125,0.125,1],
                                       'pointers_changed':False,'restore_on_exit':True,'game_default_claimed':False},
        'scope':'Explicit controlled counterfactual with unchanged original baseline and exact original/replacement same-control comparisons',
        'gpu_executed':False,'game_release_approved':False}
out=root/'reports';out.mkdir(exist_ok=True)
(out/'r62-source-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
