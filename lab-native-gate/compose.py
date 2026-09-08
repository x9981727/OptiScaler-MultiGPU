from pathlib import Path
import sys,shutil
root=Path(sys.argv[1]);p=root/'buffered_output.h';s=p.read_text(encoding='utf-8-sig')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('Composition anchor: '+a[:100])
 s=s.replace(a,b)
once('#include <d3d12sdklayers.h>','#include <d3d12sdklayers.h>\n#include "composition_output.h"')
once(' bool copyEngine=false;', ' bool copyEngine=false;\n std::unique_ptr<CompositionOutputLab::Renderer> composition;')
once('  texture.Flags=D3D12_RESOURCE_FLAG_NONE;', '''  if(CompositionOutputLab::Enabled()){
   if(directNativeInput||copyEngine||scheduleOutputGate)throw std::runtime_error("Composition mode conflicts with other output experiments");
   HWND hwnd=nullptr;Check(chain->GetHwnd(&hwnd));
   composition=std::make_unique<CompositionOutputLab::Renderer>();
   composition->Initialize(device.Get(),hwnd,desc.Width,desc.Height,desc.Format,static_cast<UINT>(Slots));
  }
  texture.Flags=D3D12_RESOURCE_FLAG_NONE;''')
once('   Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&snapshots[i])));', '   if(composition)snapshots[i]=composition->textures12.at(i);\n   else Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&snapshots[i])));')
once('   if(directNativeInput){', '''   if(composition){
    // The old same-slot present has reached the OS before this ring laps.
    // Release the FIFO mutex while waiting so later queued presents can retire it.
    l.unlock();composition->WaitAvailable(slot);l.lock();
    if(failed||stop)return failed?failure.load():E_ABORT;
   }
   if(directNativeInput){''')
once('   const auto begin=Clock::now();const double beginQpc=LabQpcMs();', '''   if(composition){
    const auto beginQpc=LabQpcMs();
    Check(captured->SetEventOnCompletion(job.id,completionEvent));
    if(WaitForSingleObject(completionEvent,3000)!=WAIT_OBJECT_0)throw std::runtime_error("Displayable buffer GPU completion timeout");
    if(captured->GetCompletedValue()==UINT64_MAX)throw std::runtime_error("Displayable producer device lost");
    const auto readyQpc=LabQpcMs();composition->Present(job.slot,job.id);
    if(outputRecords.size()<100000)outputRecords.push_back({static_cast<double>(job.id),beginQpc,readyQpc,readyQpc,LabQpcMs()});
    completed=job.id;cv.notify_all();continue;
   }
   const auto begin=Clock::now();const double beginQpc=LabQpcMs();''')
once('  if(!safe)Fail(DXGI_ERROR_DEVICE_HUNG);', '  if(!safe)Fail(DXGI_ERROR_DEVICE_HUNG);\n  if(composition){try{composition->Shutdown();}catch(const std::exception& e){std::fprintf(stderr,"COMPOSITION_SHUTDOWN %s\\n",e.what());Fail(E_FAIL);}}')
p.write_text(s,encoding='utf-8')
header=Path(__file__).with_name('composition_output.h');shutil.copyfile(header,root/'composition_output.h')
with (root/'CMakeLists.txt').open('a',encoding='utf-8') as f:f.write('\ntarget_link_libraries(basic_xess_fg_sample PRIVATE d3d11 dcomp)\n')
review=Path(__file__).resolve().parents[1]/'reference-build/source-review';review.mkdir(parents=True,exist_ok=True)
shutil.copyfile(header,review/'composition_output.h')
print('OS target-time composition output prepared; still requires hardware timing and pixels validation.')
