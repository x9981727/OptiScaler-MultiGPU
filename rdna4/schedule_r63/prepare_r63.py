# SPDX-License-Identifier: MIT
"""Run after r52, r6, r61 and OUTPUT-CONTROL r62 generators on a clean checkout."""
from pathlib import Path
import hashlib,json
ROOT=Path(__file__).resolve().parent
F=ROOT.parent/'forward_r5'
s=(F/'forward_r62.cpp').read_text(encoding='utf-8').replace('\r\n','\n')
expected='70c5ccd81840ce5f8e95f6a02ea0103f262eeb1b8d997a5f8505acfcbff9c1a8'
assert hashlib.sha256(s.encode()).hexdigest()==expected,'Wrong r6.2 predecessor: expected Output-Control branch'
def once(a,b):
    global s
    assert s.count(a)==1,'Nonunique r6.3 generation target: '+a[:100]
    s=s.replace(a,b)
once('#include "../autolab_r62/control_probe.hpp"','#include "../autolab_r62/control_probe.hpp"\n#include "../schedule_r63/scheduler_test.hpp"')
start=s.index('    const char* namesCase[]=')
end=s.index('\n}\nint r5_delivery_entry(',start)
s=s[:start]+'    r63_run(replay,originalCode,model.packed,out,report);\n'+s[end:]
once('report["r62_control_restoration_checks"]=r62::self_test();',
     'report["r62_control_restoration_checks"]=r62::self_test();\n        report["r63_schedule_policy_checks"]=r63::self_test();\n        report["scope"]="r6.3 Swin scheduling descriptors and active-control full-plan comparison; no new Swin arithmetic or game hooks";')
s=s.replace('r6.2','r6.3').replace('rdna4-r62-result.json','rdna4-r63-result.json').replace('rdna4-r62-console.txt','rdna4-r63-console.txt')
s=s.replace('RDNA4 r6.3 AutoLab - not a final game DLL','RDNA4 r6.3 Swin Scheduling - not a game DLL')
s=s.replace('Usage: rdna4-r5-test.exe','Usage: rdna4-swin-r63.exe').replace('Usage: rdna4-autolab.exe','Usage: rdna4-swin-r63.exe')
(F/'forward_r63.cpp').write_text(s,encoding='utf-8',newline='\n')
(ROOT/'reports').mkdir(exist_ok=True)
report={'revision':'r6.3','predecessor':'Output-Control r6.2','predecessor_sha256':expected,
        'generated_source_sha256':hashlib.sha256(s.encode()).hexdigest(),
        'new_swin_math_kernel':False,'original_instruction_bytes_changed':False,'swin_scheduler_descriptor_changes':True,
        'original_head_all_timed_modes':True,'timing_control':1.0,'control_is_experimental_not_verified_game_setting':True,
        'training_rounds':36,'holdout_rounds':30,'original_files_modified':False,
        'gpu_executed_during_generation':False,'game_release_approved':False}
(ROOT/'reports'/'generated-source.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
