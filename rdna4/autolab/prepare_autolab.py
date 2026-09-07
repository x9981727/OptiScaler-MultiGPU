# SPDX-License-Identifier: MIT
"""Run once after prepare_r52.py on a clean checkout; does not execute a GPU."""
from pathlib import Path
import hashlib,json
ROOT=Path(__file__).resolve().parent
FORWARD=ROOT.parent/'forward_r5'
s=(FORWARD/'forward_delivery.cpp').read_text(encoding='utf-8')
source_hash=hashlib.sha256(s.encode()).hexdigest()
def change(old,new):
    global s
    if s.count(old)!=1:raise ValueError('Non-unique patch target: '+old[:100])
    s=s.replace(old,new)
change('#include "plan_regression_r52.hpp"','#include "plan_regression_r52.hpp"\n#include "../autolab/lab_policy.hpp"')
change('static void gpu_r5(J& report,','#include "../autolab/trace_support.hpp"\nstatic void gpu_r5(J& report,')
start=s.index('        arena.reset(input);std::vector<Byte> changed(size_t(head.grid[0])*16384);')
end=s.index('        r5::need(nan==0,"Full-plan Head contains FP8 NaNs;',start)
s=s[:start]+'''        report["negative_control"]=r6_trace(replay,input,state,model.packed,out,report);
        report["instrumented_profile"]=r6_profile(replay,input,original.start,original.stop);
        save_r5(out,report);
        if(!report["negative_control"].at("observed_region_sensitive").get<bool>()){
            report["synthetic_full_plan_head_substitution_byte_identical"]=true;
            report["status"]="autolab_blocked_output_sensitivity";
            report["release_gate"]={{"approved",false},{"reason","A verified Head perturbation did not reach the monitored output; further performance selection is stopped, not called converged."}};
            delivery_stage("r6 stopped automatically: output sensitivity gate failed; causal trace and per-command timings saved");
            save_r5(out,report);return;
        }
''' + s[end:]
change('for(U round=0;round<22;++round){','for(U round=0;round<66;++round){')
change('(round<15?train[mode]:hold[mode]).push_back(ms);','(round<45?train[mode]:hold[mode]).push_back(ms);')
change('{"required_rounds",22}','{"required_rounds",66}')
change('{"complete",round==21}','{"complete",round==65}')
change('"/22 complete; samples saved"','"/66 complete; samples saved"')
s=s.replace('15 training plus 7 holdout rounds','45 training plus 21 holdout rounds')
change('report["synthetic_full_plan_head_substitution_byte_identical"]=true;\n    bool sensitive=', '''report["synthetic_full_plan_head_substitution_byte_identical"]=true;
    report["candidate_screen"]=r6_screen_report(report.at("timing"),true,true);
    report["release_gate"]={{"approved",false},{"reason","Fixed synthetic plan only: full Swin rewrite, temporal/game correctness and sustained end-to-end improvement are not established."}};
    bool sensitive=''')
change('report["cpu_boundary_regression_checks"]=r52::regression_tests();', '''auto virtualTests=autolab::cpu_lab();
        report["virtual_contract_checks"]=virtualTests.numerical_checks;
        report["candidate_policy_checks"]=virtualTests.policy_checks;
        report["virtual_lab_scope"]="CPU recovered-Head arithmetic/layout models and acceptance logic; NOT an AMD GPU ISA emulator, full DLSSNR model or GPU speed prediction";
        report["cpu_boundary_regression_checks"]=r52::regression_tests();''')
s=s.replace('r5.2','r6.0').replace('rdna4-r5-result.json','rdna4-r6-result.json').replace('rdna4-r5-console.txt','rdna4-r6-console.txt')
s=s.replace('RDNA4 r6.0 Full-Plan Differential Test - not a game DLL','RDNA4 r6.0 AutoLab - not a final game DLL')
(FORWARD/'forward_autolab.cpp').write_text(s,encoding='utf-8',newline='\n')
(ROOT/'reports').mkdir(exist_ok=True)
report={'revision':'r6.0','input_generated_r52_source_sha256':source_hash,'compiled_source_sha256':hashlib.sha256(s.encode()).hexdigest(),
 'gpu_kernel_changes':False,'bounded_timing_rounds':66,'new_features':['Portable numerical-contract VM tests','Per-command causal fingerprints with original-repeat control','Instrumented profiling kept separate from full-graph timings','Fail-closed output sensitivity gate','Paired bootstrap candidate screen with at least one percent required gain'],
 'gpu_executed_by_build_script':False,'final_game_release_approved':False}
(ROOT/'reports'/'generated-source.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
