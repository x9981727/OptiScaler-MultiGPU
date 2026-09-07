# SPDX-License-Identifier: MIT
"""CPU regression only: three reported calls, no full-plan or GPU execution claim."""
from pathlib import Path
import json,subprocess
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'build';OUT.mkdir(exist_ok=True)
main=OUT/'r52_regression.cpp'
main.write_text(r'''#ifdef R52_OLD
#include "r52_original_plan_core.hpp"
#else
#include "plan_core.hpp"
#endif
#include "plan_regression_r52.hpp"
#include <fstream>
#include <iostream>
int main(int argc,char** argv){try{
 auto fixture=r52::boundary_fixture();
 r5::need(r5::hex(fixture["events"][0]["args"][0]["bytes"].get<std::string>()).size()==168,"First reported argument must have 168 bytes");
 r5::need(r5::hex(fixture["events"][2]["args"][0]["bytes"].get<std::string>()).size()==168,"Last reported argument must have 168 bytes");
#ifdef R52_OLD
 bool reproduced=false;std::string failure;
 try{r5::Plan::parse(fixture,false);}catch(const std::exception& e){failure=e.what();reproduced=failure=="Required original pointer relocation absent";}
 r5::need(reproduced,"Old parser did not reproduce the reported exception");
 std::cout<<r5::J{{"old_error_reproduced",true},{"error",failure},{"gpu_executed",false}}.dump()<<'\n';
#else
 auto n=r52::regression_tests();auto regular=r5::self_test();auto plan=r5::Plan::parse(fixture,false);
 if(argc==2){std::ofstream f(argv[1]);f<<fixture.dump(2)<<'\n';f.close();r5::need(bool(f),"Cannot save three-call regression fixture");}
 std::cout<<r5::J{{"boundary_regression_checks",n},{"regular_parser_checks",regular},
   {"fixture_calls",plan.commands.size()},{"fixture_allocations",plan.allocations.size()},
   {"first_input_allocation",plan.firstInput.allocation},{"last_observed_allocation",plan.lastObserved.allocation},
   {"reported_head_groups",plan.commands[1].grid[0]},
   {"scope","Three reported call fragments; not the complete original plan"},{"gpu_executed",false}}.dump()<<'\n';
#endif
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
''',encoding='utf-8')
common=['clang++-19','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT),str(main),str(OUT/'puff.o')]
records={}
for mode in ['old','fixed']:
    exe=OUT/f'r52-{mode}-regression'
    subprocess.run(common+(['-DR52_OLD'] if mode=='old' else [])+['-o',str(exe)],check=True)
    command=[str(exe)]+([str(OUT/'reported-boundary-fixture.json')] if mode=='fixed' else [])
    text=subprocess.check_output(command,text=True)
    records[mode]=json.loads(text)
    print(mode,text,flush=True)
assert records['old']['old_error_reproduced']
assert records['fixed']['boundary_regression_checks']>=20
report={'revision':'r5.2','sanitizers':['address','undefined'],
        'old_parser_failure_reproduced':True,'patched_boundary_regression_passed':True,
        'results':records,'full_original_154_launch_plan_read_in_ci':False,
        'original_gpu_kernel_executed_in_ci':False,'game_tested':False}
(OUT/'r52-regression-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2),flush=True)
