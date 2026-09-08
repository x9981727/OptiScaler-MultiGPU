from pathlib import Path
import sys,hashlib,json,shutil
root=Path(sys.argv[1])/'samples/basic_sample_frame_generation'
p=root/'NativeGate.h';s=p.read_text(encoding='utf-8')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('GPU phase anchor '+a[:100])
 s=s.replace(a,b)
once('#include "NativeFrameStage.h"','#include "NativeFrameStage.h"\n#include "NativeGpuReadyProbe.h"')
once('double submitted=0,ready=0,copied=0,deadline=0,presentEnter=0,presentReturn=0,period=0;',
     'double submitted=0,ready=0,copied=0,deadline=0,presentEnter=0,presentReturn=0,period=0,gpuReady=-1,appliedPhase=0;')
once('std::unique_ptr<NativeFrameStage> images;', '''std::unique_ptr<NativeFrameStage> images;
 std::unique_ptr<NativeGpuReadyProbe> gpuProbe;
 bool automaticPhase=EnvFlag("XEFG_LAB_AUTO_PHASE");
 double lastGpuReady=-1,phaseEstimate=0;UINT64 lastGpuId=0;unsigned phaseSamples=0,invalidPhaseSamples=0;''')
once('leases.resize(desc.BufferCount);cursor=chain->GetCurrentBackBufferIndex();records.reserve(200000);','''leases.resize(desc.BufferCount);cursor=chain->GetCurrentBackBufferIndex();records.reserve(200000);
  if(automaticPhase){
   gpuProbe=std::make_unique<NativeGpuReadyProbe>();
   if(FAILED(gpuProbe->Initialize(d.Get(),q,desc.BufferCount)))throw std::runtime_error("GPU readiness query unavailable");
  }''')
once('HRESULT hr=producer->Signal(readyFence.Get(),r.id);if(FAILED(hr))return Fail(hr);', '''HRESULT hr=S_OK;
  if(gpuProbe)hr=gpuProbe->Record(r.sourceIndex,r.id,readyFence.Get());
  if(FAILED(hr))return Fail(hr);
  hr=producer->Signal(readyFence.Get(),r.id);if(FAILED(hr))return Fail(hr);''')
once('r.ready=QpcMs()-epoch;r.actualIndex=getIndex(chain.Get());', '''r.ready=QpcMs()-epoch;r.actualIndex=getIndex(chain.Get());
   if(gpuProbe&&SUCCEEDED(hr)){
    r.gpuReady=gpuProbe->ReadMilliseconds(r.sourceIndex,r.id,readyFence.Get());
    if(r.gpuReady>=0&&lastGpuReady>=0&&r.id==lastGpuId+1&&r.paced){
     const double gap=r.gpuReady-lastGpuReady;
     if((r.id%2)==0&&gap>=0&&gap<r.period*1.5){
      const double bound=r.period*0.24;
      const double target=(std::max)(-bound,(std::min)(bound,r.period*0.5-gap));
      phaseEstimate=phaseSamples?phaseEstimate*0.9+target*0.1:target;
      ++phaseSamples;
     }
    }else if(r.gpuReady<0){++invalidPhaseSamples;}
    if(!r.paced){phaseEstimate=0;phaseSamples=0;}
    lastGpuReady=r.gpuReady;lastGpuId=r.id;
   }''')
once('const double adjusted=nextDeadline+PhaseOffset(r.id,r.period);r.deadline=adjusted-epoch;SleepUntil(adjusted);',
     'r.appliedPhase=automaticPhase?((r.id%2)==0?phaseEstimate:0.0):PhaseOffset(r.id,r.period);const double adjusted=nextDeadline+r.appliedPhase;r.deadline=adjusted-epoch;SleepUntil(adjusted);')
once('source_period_ms,paced,result\\n','source_period_ms,paced,result,gpu_ready_ms,phase_ms\\n')
once("<<static_cast<long>(r.result)<<'\\n';", "<<static_cast<long>(r.result)<<','<<r.gpuReady<<','<<r.appliedPhase<<'\\n';")
once('<<",\\\"native_frame_labels_may_be_unavailable\\\":true,\\\"game_modified\\\":false,\\\"resize_supported\\\":false}\\n";',
     '<<",\\\"automatic_phase\\\":"<<(automaticPhase?"true":"false")<<",\\\"phase_samples\\\":"<<phaseSamples<<",\\\"invalid_phase_samples\\\":"<<invalidPhaseSamples<<",\\\"final_phase_ms\\\":"<<phaseEstimate<<",\\\"native_frame_labels_may_be_unavailable\\\":true,\\\"game_modified\\\":false,\\\"resize_supported\\\":false}\\n";')
p.write_text(s,encoding='utf-8')
shutil.copyfile(Path(__file__).parent/'NativeGpuReadyProbe.h',root/'NativeGpuReadyProbe.h')
# Exercise the same timestamp helper in the existing pixel/lease test.
p=root/'stage-test.cpp';s=p.read_text(encoding='utf-8')
once('#include "NativeFrameStage.h"','#include "NativeFrameStage.h"\n#include "NativeGpuReadyProbe.h"')
once('NativeGateLab::NativeFrameStage stage;Hr(stage.Initialize(d,consumer.Get(),targets));',
     'NativeGateLab::NativeFrameStage stage;Hr(stage.Initialize(d,consumer.Get(),targets));\n NativeGateLab::NativeGpuReadyProbe probe;Hr(probe.Initialize(d,producer.Get(),3));double previousGpuTime=-1;')
once('producer->ExecuteCommandLists(1,xx);Hr(producer->Signal(ready.Get(),frame));',
     'producer->ExecuteCommandLists(1,xx);Hr(probe.Record(slot,frame,ready.Get()));Hr(producer->Signal(ready.Get(),frame));')
once('readback->Unmap(0,&none);','''readback->Unmap(0,&none);
  double gpuTime=probe.ReadMilliseconds(slot,frame,ready.Get());
  if(gpuTime<0||gpuTime<=previousGpuTime)throw std::runtime_error("GPU readiness timestamp not monotonic");
  if(probe.ReadMilliseconds(slot,frame+1,ready.Get())!=-1)throw std::runtime_error("Mismatched GPU ticket accepted");
  previousGpuTime=gpuTime;''')
p.write_text(s,encoding='utf-8')
m=root/'NATIVE-GATE-MANIFEST.json';d=json.loads(m.read_text());d.update(automatic_phase_optional=True,automatic_phase_source='producer GPU timestamp difference, not CPU ready observation',automatic_phase_changes_period=False,automatic_phase_default=False)
for n in ['NativeGate.h','NativeGpuReadyProbe.h','stage-test.cpp']:d[n+'_sha256']=hashlib.sha256((root/n).read_bytes()).hexdigest()
m.write_text(json.dumps(d,indent=2),encoding='utf-8')
