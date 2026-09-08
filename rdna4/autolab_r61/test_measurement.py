# SPDX-License-Identifier: MIT
import json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
build=root/'build';build.mkdir(exist_ok=True)
source=build/'measurement_test.cpp'
source.write_text(r'''#include "measurement_policy.hpp"
#include <iostream>
int main(){
 try {
  auto n=r61::self_test();
  std::vector<std::vector<float>> samples(158,std::vector<float>(7,0.002f));
  samples[101][0]=0.0f;
  std::size_t resolved=0,unresolved=0;
  for(const auto& s:samples){if(r61::strict_median(s))++resolved;else ++unresolved;}
  if(resolved!=157 || unresolved!=1)throw std::runtime_error("Censored command must not be ranked as fastest");
  // This is fault injection, not a claim that the user's missing raw value was zero.
  std::cout<<"{\"checks\":"<<n<<",\"injected_command\":101,\"resolved\":"<<resolved
           <<",\"unresolved\":"<<unresolved<<",\"gpu_executed\":false}\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
''',encoding='utf-8')
rows=[]
for name,flags in [('debug',['-O0','-g']),('asan-ubsan',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']),('optimized',['-O3'])]:
    exe=build/('measurement-'+name)
    cmd=['clang++-19','-std=c++20','-ffp-contract=off',*flags,'-I',str(root),str(source),'-o',str(exe)]
    subprocess.run(cmd,check=True)
    result=json.loads(subprocess.check_output([str(exe)],text=True))
    assert result['checks']>=100 and result['unresolved']==1 and not result['gpu_executed']
    rows.append({'configuration':name,'command':cmd,'result':result})
    print(name,json.dumps(result),flush=True)
out=root/'reports';out.mkdir(exist_ok=True)
(out/'measurement-regressions.json').write_text(json.dumps({'all_passed':True,'scope':'Production policy and synthetic fault-injection tests; not a reproduction of the unknown actual GPU timestamp','tests':rows},indent=2)+'\n')
