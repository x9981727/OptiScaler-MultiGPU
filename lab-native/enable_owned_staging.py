from pathlib import Path
import sys,json,hashlib,shutil
root=Path(sys.argv[1])/'samples/basic_sample_frame_generation'
p=root/'NativeGate.h';s=p.read_text(encoding='utf-8')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('staging anchor: '+a[:120])
 s=s.replace(a,b)
once('#include <d3d12sdklayers.h>','#include <d3d12sdklayers.h>\n#include "NativeFrameStage.h"')
once('    ComPtr<ID3D12CommandQueue> queue, displayQueue;','''    ComPtr<ID3D12CommandQueue> queue, displayQueue;
    using GetBufferFn=HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,REFIID,void**);
    GetBufferFn originalGetBuffer=nullptr;
    std::unique_ptr<NativeFrameStage> staged;
    std::recursive_mutex presentCallMutex;
    HRESULT stageError=S_OK;''')
once('        readyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);','''        if(EnvFlag("XEFG_LAB_OWNED_OUTPUTS")) {
            if(display==q)throw std::runtime_error("Owned output experiment requires split display queue");
            DXGI_SWAP_CHAIN_DESC1 desc{};
            if(FAILED(chain->GetDesc1(&desc)))throw std::runtime_error("Native descriptor unavailable");
            std::vector<ComPtr<ID3D12Resource>> targets(desc.BufferCount);
            for(UINT i=0;i<desc.BufferCount;++i)
                if(FAILED(chain->GetBuffer(i,IID_PPV_ARGS(&targets[i]))))throw std::runtime_error("Native buffer unavailable");
            staged=std::make_unique<NativeFrameStage>();
            if(FAILED(staged->Initialize(dev.Get(),display,targets)))throw std::runtime_error("Owned output allocation failed");
        }
        readyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);''')
once('        originalPresent=reinterpret_cast<PresentFn>(originalVtable[8]);','''        originalPresent=reinterpret_cast<PresentFn>(originalVtable[8]);
        originalGetBuffer=reinterpret_cast<GetBufferFn>(originalVtable[9]);''')
once('        std::unique_lock<std::mutex> lock(mutex);\n        if(records.size()>=199990 || stop || failed) return SIZE_MAX;','''        stageError=S_OK;
        // Slot reclamation occurs outside the record mutex, so the deadline
        // worker remains able to retire prior tickets while a slot is busy.
        if(staged && FAILED(stageError=staged->Prepare(r.index)))return SIZE_MAX;
        std::unique_lock<std::mutex> lock(mutex);
        if(records.size()>=199990 || stop || failed) {stageError=E_ABORT;return SIZE_MAX;}''')
once('''        if(SUCCEEDED(a) && SUCCEEDED(b) && r.gate)
            b=displayQueue->Wait(releaseFence.Get(),r.id);''','''        if(SUCCEEDED(a) && SUCCEEDED(b) && staged) {
            b=staged->Copy(r.index,r.id);
            // Only protect the raw copy read, never wait for its display deadline.
            // This conservative dependency can be optimized after pixel validation.
            if(SUCCEEDED(b)) b=queue->Wait(staged->CopyFence(),r.id);
        }
        if(SUCCEEDED(a) && SUCCEEDED(b) && r.gate)
            b=displayQueue->Wait(releaseFence.Get(),r.id);''')
once('    void After(size_t i,HRESULT h) {','''    HRESULT SubmissionResult(size_t i) {
        if(i==SIZE_MAX)return FAILED(stageError)?stageError:E_ABORT;
        std::lock_guard<std::mutex> lock(mutex);
        return FAILED(records[i].signalHr)?records[i].signalHr:records[i].waitHr;
    }
    ID3D12Fence* DisplayReadyFence() {return staged?staged->CopyFence():readyFence.Get();}
    void After(size_t i,HRESULT h) {''')
# The worker observes final pixels ready after the copy, not merely SDK submission.
s=s.replace('readyFence->GetCompletedValue()', 'DisplayReadyFence()->GetCompletedValue()')
s=s.replace('readyFence->SetEventOnCompletion(r.id,readyEvent)', 'DisplayReadyFence()->SetEventOnCompletion(r.id,readyEvent)')
once('''                    if(ready>target+step || target>ready+r.period) target=ready;''','''                    if(staged) {
                        // Acquire enough initial phase to absorb alternating
                        // GPU readiness, without reducing the source rate target.
                        target=lastRelease>0 ? (std::max)(ready,lastRelease+step):ready+step;
                        if(target>ready+r.period)target=ready+step;
                    } else if(ready>target+step || target>ready+r.period) target=ready;''')
once('        if(worker.joinable())worker.join();','''        if(worker.joinable())worker.join();
        if(staged && FAILED(staged->Drain()))failed=true;''')
anchor='         <<",\\\"display_queue_separate_from_sdk\\\":"<<(displayQueue.Get()!=queue.Get()?"true":"false")'
once(anchor,anchor+'\n         <<",\\\"owned_output_textures\\\":"<<(staged?"true":"false")')
once('inline HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* sc,UINT interval,UINT flags) {','''inline HRESULT STDMETHODCALLTYPE OnGetBuffer(IDXGISwapChain* sc,UINT index,REFIID iid,void** result) {
    State* s=state.get();if(!s)return E_UNEXPECTED;
    return s->staged?s->staged->GetBuffer(index,iid,result):s->originalGetBuffer(sc,index,iid,result);
}
inline HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* sc,UINT interval,UINT flags) {''')
old='NativeScope scope;size_t i=s->Before(interval,flags);HRESULT hr=s->originalPresent(sc,interval,flags);s->After(i,hr);return hr;'
new='std::lock_guard<std::recursive_mutex> serial(s->presentCallMutex); NativeScope scope;size_t i=s->Before(interval,flags);HRESULT hr=s->SubmissionResult(i);if(SUCCEEDED(hr))hr=s->originalPresent(sc,interval,flags);s->After(i,hr);return hr;'
once(old,new)
old='NativeScope scope;size_t i=s->Before(interval,flags);HRESULT hr=s->originalPresent1(sc,interval,flags,p);s->After(i,hr);return hr;'
new='std::lock_guard<std::recursive_mutex> serial(s->presentCallMutex); NativeScope scope;size_t i=s->Before(interval,flags);HRESULT hr=s->SubmissionResult(i);if(SUCCEEDED(hr))hr=s->originalPresent1(sc,interval,flags,p);s->After(i,hr);return hr;'
once(old,new)
once('next->vtable[8]=reinterpret_cast<void*>(&OnPresent);next->vtable[22]=reinterpret_cast<void*>(&OnPresent1);','''next->vtable[8]=reinterpret_cast<void*>(&OnPresent);next->vtable[22]=reinterpret_cast<void*>(&OnPresent1);
                next->vtable[9]=reinterpret_cast<void*>(&OnGetBuffer);''')
p.write_text(s,encoding='utf-8')
shutil.copyfile(Path(__file__).parent/'NativeFrameStage.h',root/'NativeFrameStage.h')
m=root/'NATIVE-GATE-MANIFEST.json';d=json.loads(m.read_text());d['owned_output_textures_optional']=True
for n in ['NativeGate.h','NativeFrameStage.h','basic_sample.cpp']:d[n+'_sha256']=hashlib.sha256((root/n).read_bytes()).hexdigest()
d['prototype_resize_validation']='not_implemented_do_not_use_in_game';d['game_modified']=False
m.write_text(json.dumps(d,indent=2),encoding='utf-8')
