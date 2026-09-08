from pathlib import Path
import sys,hashlib,json,shutil
root=Path(sys.argv[1])/'samples/basic_sample_frame_generation'
p=root/'NativeGate.h';s=p.read_text(encoding='utf-8')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('priority scope anchor '+a[:100])
 s=s.replace(a,b)
once('#include "NativeFrameStage.h"','#include "NativeFrameStage.h"\n#include "SdkQueuePriorityScope.h"')
once('ComPtr<IDXGIFactory7> factory;void**old=nullptr;', 'std::unique_ptr<SdkQueuePriorityScope> sdkPriority;ComPtr<IDXGIFactory7> factory;void**old=nullptr;')
once('FactoryTap(IDXGIFactory*f,ID3D12CommandQueue*){', '''FactoryTap(IDXGIFactory*f,ID3D12CommandQueue* app){
  if(EnvFlag("XEFG_LAB_DEMOTE_SDK")) {
   ComPtr<ID3D12Device> d;
   if(!app||FAILED(app->GetDevice(IID_PPV_ARGS(&d))))throw std::runtime_error("priority device query");
   sdkPriority=std::make_unique<SdkQueuePriorityScope>(d.Get(),EnvPath());
  }
''')
once('hr=dev->CreateCommandQueue(&desc,IID_PPV_ARGS(&display));', '{SdkQueuePriorityScope::DisplayGuard preserve;hr=dev->CreateCommandQueue(&desc,IID_PPV_ARGS(&display));}')
p.write_text(s,encoding='utf-8')
shutil.copyfile(Path(__file__).parent/'SdkQueuePriorityScope.h',root/'SdkQueuePriorityScope.h')
base=Path(sys.argv[2]).resolve().as_posix()+'/src/'
files=['detours.cpp','modules.cpp','disasm.cpp','image.cpp','creatwth.cpp','disolx86.cpp','disolx64.cpp','disolia64.cpp','disolarm.cpp','disolarm64.cpp']
cm=root/'CMakeLists.txt';text=cm.read_text(encoding='utf-8')
text+='\nadd_library(lab_detours STATIC\n'+''.join('  "'+base+n+'"\n' for n in files)+')\n'
text+='target_include_directories(lab_detours PUBLIC "'+base+'")\n'
text+='target_compile_definitions(lab_detours PRIVATE WIN32_LEAN_AND_MEAN)\n'
text+='target_link_libraries(basic_xess_fg_sample PRIVATE lab_detours)\n'
cm.write_text(text,encoding='utf-8')
m=root/'NATIVE-GATE-MANIFEST.json';d=json.loads(m.read_text());d.update(sdk_high_to_normal_initialization_experiment_optional=True,priority_override_default=False,detours_commit='e4bfd6b03e50de46b47abfbd1e46b384f0c5f833',system_or_other_process_changes=False)
for n in ['NativeGate.h','SdkQueuePriorityScope.h']:d[n+'_sha256']=hashlib.sha256((root/n).read_bytes()).hexdigest()
m.write_text(json.dumps(d,indent=2),encoding='utf-8')
