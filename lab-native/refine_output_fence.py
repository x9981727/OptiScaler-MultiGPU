from pathlib import Path
import sys,json,hashlib
root=Path(sys.argv[1])/'samples/basic_sample_frame_generation'
def replace(text,a,b):
 if text.count(a)!=1:raise RuntimeError('output fence anchor '+a[:120])
 return text.replace(a,b)
p=root/'NativeFrameStage.h';s=p.read_text(encoding='utf-8')
s=replace(s,'#include <utility>','#include <utility>\n#include <mutex>')
s=replace(s,'    UINT64 last=0;','    UINT64 last=0;\n    std::mutex leaseMutex;\n    UINT64 reuseWaitCount=0;')
s=replace(s,'        if(s.retired && copied->GetCompletedValue()<s.retired) {\n            HRESULT hr=copied->SetEventOnCompletion(s.retired,event);if(FAILED(hr))return hr;','''        UINT64 ticket=0;
        {std::lock_guard<std::mutex> lock(leaseMutex);ticket=s.retired;}
        if(ticket && copied->GetCompletedValue()<ticket) {
            HRESULT hr=copied->SetEventOnCompletion(ticket,event);if(FAILED(hr))return hr;''')
s=replace(s,'        s.retired=serial;last=serial;return S_OK;','''        {std::lock_guard<std::mutex> lock(leaseMutex);s.retired=serial;}
        last=serial;return S_OK;''')
s=replace(s,'    ID3D12Fence* CopyFence() const { return copied.Get(); }','''    HRESULT ProtectReuse(UINT index,ID3D12CommandQueue* producer) {
        if(index>=slots.size() || !producer)return E_INVALIDARG;
        std::lock_guard<std::mutex> lock(leaseMutex);
        const UINT64 ticket=slots[index].retired;
        if(!ticket || copied->GetCompletedValue()>=ticket)return S_OK;
        const HRESULT hr=producer->Wait(copied.Get(),ticket);
        if(SUCCEEDED(hr))++reuseWaitCount;
        return hr;
    }
    UINT64 ReuseWaits() {std::lock_guard<std::mutex> lock(leaseMutex);return reuseWaitCount;}
    ID3D12Fence* CopyFence() const { return copied.Get(); }''')
p.write_text(s,encoding='utf-8')
p=root/'NativeGate.h';s=p.read_text(encoding='utf-8')
s=replace(s,'    GetBufferFn originalGetBuffer=nullptr;','''    GetBufferFn originalGetBuffer=nullptr;
    using GetIndexFn=UINT (STDMETHODCALLTYPE*)(IDXGISwapChain3*);
    GetIndexFn originalGetIndex=nullptr;''')
s=replace(s,'        originalGetBuffer=reinterpret_cast<GetBufferFn>(originalVtable[9]);','''        originalGetBuffer=reinterpret_cast<GetBufferFn>(originalVtable[9]);
        originalGetIndex=reinterpret_cast<GetIndexFn>(originalVtable[36]);''')
s=replace(s,'''        if(SUCCEEDED(a) && SUCCEEDED(b) && staged) {
            b=staged->Copy(r.index,r.id);
            // Only protect the raw copy read, never wait for its display deadline.
            // This conservative dependency can be optimized after pixel validation.
            if(SUCCEEDED(b)) b=queue->Wait(staged->CopyFence(),r.id);
        }
        if(SUCCEEDED(a) && SUCCEEDED(b) && r.gate)
            b=displayQueue->Wait(releaseFence.Get(),r.id);''','''        if(SUCCEEDED(a) && SUCCEEDED(b) && r.gate)
            b=displayQueue->Wait(releaseFence.Get(),r.id);
        // The final pixel write must be after the release fence. A stand-alone
        // queue wait after a completed write does not certify flip timing.
        if(SUCCEEDED(a) && SUCCEEDED(b) && staged)
            b=staged->Copy(r.index,r.id);''')
s=replace(s,'    ID3D12Fence* DisplayReadyFence() {return staged?staged->CopyFence():readyFence.Get();}','''    ID3D12Fence* DisplayReadyFence() {return readyFence.Get();}
    HRESULT ProtectCurrentBuffer() {
        if(!staged)return S_OK;
        return staged->ProtectReuse(originalGetIndex(chain.Get()),queue.Get());
    }''')
s=replace(s,'inline HRESULT STDMETHODCALLTYPE OnGetBuffer(IDXGISwapChain* sc,UINT index,REFIID iid,void** result) {','''inline UINT STDMETHODCALLTYPE OnGetIndex(IDXGISwapChain3* sc) {
    State* s=state.get();if(!s)return 0;
    const UINT index=s->originalGetIndex(sc);
    if(!insideNative && s->staged && FAILED(s->staged->ProtectReuse(index,s->queue.Get())))s->failed=true;
    return index;
}
inline HRESULT STDMETHODCALLTYPE OnGetBuffer(IDXGISwapChain* sc,UINT index,REFIID iid,void** result) {''')
s=replace(s,'if(SUCCEEDED(hr))hr=s->originalPresent(sc,interval,flags);s->After(i,hr);return hr;',
 'if(SUCCEEDED(hr))hr=s->originalPresent(sc,interval,flags);if(SUCCEEDED(hr)){HRESULT reuse=s->ProtectCurrentBuffer();if(FAILED(reuse))hr=reuse;}s->After(i,hr);return hr;')
s=replace(s,'if(SUCCEEDED(hr))hr=s->originalPresent1(sc,interval,flags,p);s->After(i,hr);return hr;',
 'if(SUCCEEDED(hr))hr=s->originalPresent1(sc,interval,flags,p);if(SUCCEEDED(hr)){HRESULT reuse=s->ProtectCurrentBuffer();if(FAILED(reuse))hr=reuse;}s->After(i,hr);return hr;')
s=replace(s,'next->vtable[9]=reinterpret_cast<void*>(&OnGetBuffer);','next->vtable[9]=reinterpret_cast<void*>(&OnGetBuffer);\n                next->vtable[36]=reinterpret_cast<void*>(&OnGetIndex);')
a='         <<",\\\"owned_output_textures\\\":"<<(staged?"true":"false")'
s=replace(s,a,a+'\n         <<",\\\"reuse_only_waits\\\":"<<(staged?staged->ReuseWaits():0)<<",\\\"pixel_write_after_deadline\\\":true"')
p.write_text(s,encoding='utf-8')
m=root/'NATIVE-GATE-MANIFEST.json';d=json.loads(m.read_text())
for n in ['NativeGate.h','NativeFrameStage.h']:d[n+'_sha256']=hashlib.sha256((root/n).read_bytes()).hexdigest()
d['pixel_write_after_deadline']=True;d['source_waits_only_on_slot_reuse']=True
m.write_text(json.dumps(d,indent=2),encoding='utf-8')
