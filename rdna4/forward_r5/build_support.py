# SPDX-License-Identifier: MIT
"""Build-time support tests. Does not have the user's original plan or a GPU."""
from pathlib import Path
import hashlib,json,subprocess,urllib.request,zipfile
ROOT=Path(__file__).resolve().parent
DEPS=ROOT/'deps';DEPS.mkdir(exist_ok=True)
OUT=ROOT/'build';OUT.mkdir(exist_ok=True)
JSON_REV='55f93686c01528224f448c19128836e7df245f72'
sources=[
 ('json.hpp',f'https://raw.githubusercontent.com/nlohmann/json/{JSON_REV}/single_include/nlohmann/json.hpp',None),
 ('JSON_LICENSE.MIT',f'https://raw.githubusercontent.com/nlohmann/json/{JSON_REV}/LICENSE.MIT',None),
 ('puff.c','https://raw.githubusercontent.com/madler/zlib/v1.3.1/contrib/puff/puff.c','d759825ab1d79fc67b453944931f87d14b6f591e'),
 ('puff.h','https://raw.githubusercontent.com/madler/zlib/v1.3.1/contrib/puff/puff.h','e23a2454316cfa0c05e5e01cd6e1d548b4f2d44e'),
]
records=[]
for name,url,git_sha in sources:
    with urllib.request.urlopen(url,timeout=60) as f:data=f.read(2*1024*1024)
    assert 0<len(data)<2*1024*1024
    blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
    if git_sha:assert blob==git_sha,(name,blob)
    (DEPS/name).write_bytes(data)
    records.append({'file':name,'url':url,'sha256':hashlib.sha256(data).hexdigest(),'git_blob_sha1':blob})
fixture={'allocations':[{'id':0,'size':32},{'id':1,'size':32},{'id':2,'size':32}],
 'events':[{'op':'launch','kernel':'_Z_fixture','grid':[1,1,1],'block':[32,1,1],'shared':0,
 'args':[{'bytes':'0'*48,'relocations':[{'at':0,'allocation':1,'offset':0},{'at':8,'allocation':2,'offset':0},{'at':16,'allocation':0,'offset':0}]}]}]}
for mode,compression in [('deflated',zipfile.ZIP_DEFLATED),('stored',zipfile.ZIP_STORED)]:
    with zipfile.ZipFile(OUT/f'fixture-{mode}.zip','w',compression=compression) as z:
        z.writestr('checkpoint/plans/1080p.json',json.dumps(fixture))
        z.writestr('../must-not-extract.txt','This entry must NEVER be extracted to disk.')
main=OUT/'cpu.cpp'
main.write_text(r'''#include "plan_core.hpp"
#include <fstream>
#include <iostream>
std::vector<unsigned char> read(const char* p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char** argv){try{
 if(argc!=3)return 3;
 auto n=r5::self_test();
 for(int i=1;i<3;++i){auto v=read(argv[i]);auto raw=r5::plan_bytes(v);r5::need(r5::J::parse(raw.begin(),raw.end())==r5::fixture(),"ZIP content differs");++n;
   auto bad=v; // Damage the first central-directory CRC and require rejection.
   for(size_t p=0;p+46<bad.size();++p)if(r5::le<r5::U>(bad,p)==0x02014b50u){bad[p+16]^=1;break;}
   bool rejected=false;try{r5::plan_bytes(bad);}catch(...){rejected=true;}r5::need(rejected,"Corrupt CRC accepted");++n;
 }
 std::cout<<n<<" support checks passed; original plan and GPU NOT executed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
''')
subprocess.run(['clang-19','-O2','-c',str(DEPS/'puff.c'),'-o',str(OUT/'puff.o')],check=True)
subprocess.run(['clang++-19','-std=c++20','-O2','-I',str(ROOT),str(main),str(OUT/'puff.o'),'-o',str(OUT/'cpu-test')],check=True)
text=subprocess.check_output([str(OUT/'cpu-test'),str(OUT/'fixture-deflated.zip'),str(OUT/'fixture-stored.zip')],text=True)
assert not (Path.cwd().parent/'must-not-extract.txt').exists()
report={'support_cpu_tests':text.strip(),'original_plan_read_in_ci':False,'original_plan_sha256_expected':'0867c68b824b3a6e4a8e7b1681efc80a1ab8251a50384bf005f600a80fe89a93',
        'gpu_tested':False,'full_forward_tested':False,'dependencies':records}
(OUT/'support-build-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2),flush=True)
