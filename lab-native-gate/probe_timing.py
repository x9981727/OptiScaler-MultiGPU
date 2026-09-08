from pathlib import Path
import sys,subprocess,shutil
root=Path(sys.argv[1]); p=root/'basic_sample.cpp';s=p.read_text(encoding='utf-8-sig')
def replace(text,old,new):
 if text.count(old)!=1:raise RuntimeError('Timing probe anchor: '+old[:100])
 return text.replace(old,new)
s=replace(s,'extern "C" bool FastReferenceMode();','extern "C" bool FastReferenceMode();\nextern "C" float ReferenceFrameHint(float);\nextern "C" void RecordSourceStart(unsigned);\nextern "C" void RecordSourceEnd(unsigned,unsigned,int);')
s=replace(s,'FastReferenceMode() ? 3.574f : m_lastFrameTimeMS','ReferenceFrameHint(m_lastFrameTimeMS)')
s=replace(s,'void BasicSample::OnUpdate()\n{','void BasicSample::OnUpdate()\n{\n    RecordSourceStart(m_frameCounter);')
s=replace(s,'    if (m_lastPresentStatus.frameGenResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)','    RecordSourceEnd(m_frameCounter,m_lastPresentStatus.framesPresented,static_cast<int>(m_lastPresentStatus.frameGenResult));\n    if (m_lastPresentStatus.frameGenResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)')
s=replace(s,'#include "basic_sample.h"','#include "basic_sample.h"\n#include <cstdio>\n#include <d3d12sdklayers.h>')
p.write_text(s,encoding='utf-8')
n=root/'native_gate.cpp';c=n.read_text(encoding='utf-8-sig')
c=replace(c,'#include <algorithm>','#include <algorithm>\n#include <vector>\n#include <fstream>\n#include <iomanip>\n#include <cmath>')
c=replace(c,'extern "C" void FinishNativeGate() {BufferedOutputLab::Finish();gate.Finish();}','extern "C" void DumpSourceRecords();\nextern "C" void FinishNativeGate() {BufferedOutputLab::Finish();gate.Finish();DumpSourceRecords();}')
c+='''
namespace {
struct SourceRecord {unsigned id=0,frames=0;int result=-999;double start=0,end=0;};
std::vector<SourceRecord> sourceRecords;
double SourceQpcMs(){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return 1000.0*static_cast<double>(t.QuadPart)/static_cast<double>(f.QuadPart);}
}
extern "C" float ReferenceFrameHint(float actualMs){
 char b[64]{};
 if(GetEnvironmentVariableA("XEFG_FAST_HINT_MS",b,64)){
  char* end=nullptr;double value=std::strtod(b,&end);
  if(end!=b&&*end==0&&std::isfinite(value)&&value>=0&&value<=100)return static_cast<float>(value);
 }
 return FastReferenceMode()?3.574f:actualMs;
}
extern "C" void RecordSourceStart(unsigned id){
 if(sourceRecords.size()<100000)sourceRecords.push_back({id,0,-999,SourceQpcMs(),0});
}
extern "C" void RecordSourceEnd(unsigned id,unsigned frames,int result){
 if(!sourceRecords.empty()&&sourceRecords.back().id==id){auto& r=sourceRecords.back();r.frames=frames;r.result=result;r.end=SourceQpcMs();}
}
extern "C" void DumpSourceRecords(){
 char path[32768]{};DWORD n=GetEnvironmentVariableA("XEFG_LAB_TRACE_DIR",path,sizeof(path));
 if(!n||n>=sizeof(path))return;
 std::ofstream f(std::string(path)+"/source-cadence.csv");f<<"source_id,start_qpc_ms,end_qpc_ms,sdk_outputs,sdk_result\\n"<<std::fixed<<std::setprecision(5);
 for(const auto& r:sourceRecords)f<<r.id<<','<<r.start<<','<<r.end<<','<<r.frames<<','<<r.result<<'\\n';
 std::fprintf(stderr,"SOURCE_CADENCE_RECORDED count=%zu firstQpcMs=%.5f lastQpcMs=%.5f\\n",sourceRecords.size(),sourceRecords.empty()?0:sourceRecords.front().start,sourceRecords.empty()?0:sourceRecords.back().start);std::fflush(stderr);
}
'''
n.write_text(c,encoding='utf-8');print('Explicit source cadence records and frame-time hint control prepared.')
for step in ('precision.py','resource_gate.py','output_dma.py','queue_isolation.py','single_copy.py'):
 subprocess.run([sys.executable,str(Path(__file__).with_name(step)),str(root)],check=True)
probe=Path(__file__).with_name('composition_probe.cpp')
(root/'composition_probe.cpp').write_text('#include <initializer_list>\n'+probe.read_text(encoding='utf-8'),encoding='utf-8')
with (root/'CMakeLists.txt').open('a',encoding='utf-8') as f:
 f.write('\nadd_executable(composition_capability_probe composition_probe.cpp)\nset_property(TARGET composition_capability_probe PROPERTY CXX_STANDARD 17)\ntarget_link_libraries(composition_capability_probe PRIVATE d3d11 d3d12 dxgi dcomp)\n')
