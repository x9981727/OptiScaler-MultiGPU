"""Patch only an isolated Intel reference; never the installed OptiScaler DLL."""
from pathlib import Path
import hashlib,json,sys
root=Path(sys.argv[1])/"samples/basic_sample_frame_generation"
p=root/"basic_sample.cpp";s=p.read_text(encoding="utf-8-sig")
before=hashlib.sha256(p.read_bytes()).hexdigest()
def replace_once(text,a,b):
    if text.count(a)!=1: raise RuntimeError("Source anchor not unique: "+a[:100])
    return text.replace(a,b)
def once(a,b):
    global s
    s=replace_once(s,a,b)
once('#include "basic_sample.h"', '#include "basic_sample.h"\n#include "NativeGate.h"')
once('    xellSleep(m_xellContext, m_frameCounter);',
     '    if (!NativeGateLab::EnvFlag("XEFG_LAB_SLEEP_BYPASS"))\n        xellSleep(m_xellContext, m_frameCounter);')
once('    // Describe and create the swap chain.',
     '    NativeGateLab::FactoryTap nativeDisplayTap(factory.Get(), m_commandQueue.Get());\n\n    // Describe and create the swap chain.')
once('void BasicSample::OnRender()\n{',
     'void BasicSample::OnRender()\n{\n    NativeGateLab::Source(m_lastFrameTimeMS, m_enableXeFG);')
once('            constData.frameRenderTime = m_lastFrameTimeMS;',
     '            constData.frameRenderTime = NativeGateLab::Hint(m_lastFrameTimeMS);')
once('void BasicSample::OnDestroy()\n{',
     'void BasicSample::OnDestroy()\n{\n    NativeGateLab::active.store(false);')
once('    ThrowIfFailed(xefgSwapChainDestroy(m_xefgSwapChain), "Failed to destroy XeSS-FG swap chain context");',
     '    ThrowIfFailed(xefgSwapChainDestroy(m_xefgSwapChain), "Failed to destroy XeSS-FG swap chain context");\n    NativeGateLab::Finish();')
p.write_text(s,encoding="utf-8")
h=(Path(__file__).parent/"NativeGate.h").read_text(encoding="utf-8")
def change(a,b):
    global h
    h=replace_once(h,a,b)
change('''                    target=std::max(ready,lastRelease+r.period*0.5);
                    if(target-ready>r.period) target=ready; // no accumulated latency debt''','''                    const double step=r.period*0.5;
                    target=lastRelease>0 ? lastRelease+step : ready;
                    // Absolute deadlines: timer overshoot must not accumulate.
                    if(ready>target+step || target>ready+r.period) target=ready;''')
change('lastRelease=released;','lastRelease=target;')
change('    ComPtr<ID3D12CommandQueue> queue;','    ComPtr<ID3D12CommandQueue> queue, displayQueue;')
change('void Initialize(IDXGISwapChain1* sc,ID3D12CommandQueue* q,ID3D12CommandQueue* app)',
       'void Initialize(IDXGISwapChain1* sc,ID3D12CommandQueue* q,ID3D12CommandQueue* app,ID3D12CommandQueue* display)')
change('        queue=q; sameQueue=q==app; requested=EnvFlag("XEFG_LAB_GATE");',
       '        queue=q; displayQueue=display; sameQueue=display==app; requested=EnvFlag("XEFG_LAB_GATE");')
change('''        HRESULT b=S_OK;
        if(SUCCEEDED(a) && r.gate) b=queue->Wait(releaseFence.Get(),r.id);''','''        HRESULT b=S_OK;
        // Only the swapchain's display queue waits. Do not block the SDK work
        // queue before its next interpolation dispatch. The producer still
        // provides one monotonically increasing ready signal per real Present.
        if(SUCCEEDED(a) && displayQueue.Get()!=queue.Get())
            b=displayQueue->Wait(readyFence.Get(),r.id);
        if(SUCCEEDED(a) && SUCCEEDED(b) && r.gate)
            b=displayQueue->Wait(releaseFence.Get(),r.id);''')
change('''        const HRESULT hr=t->original(f,d,w,sd,fd,restrictOutput,result);
        if(SUCCEEDED(hr) && result && *result && !state) {
            ComPtr<ID3D12CommandQueue> q;
            if(SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&q)))) {
                auto next=std::make_unique<State>();next->Initialize(*result,q.Get(),t->applicationQueue.Get());''','''        ComPtr<ID3D12CommandQueue> q,display;
        const bool isQueue=d && SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&q)));
        if(isQueue) display=q;
        if(isQueue && EnvFlag("XEFG_LAB_SPLIT_QUEUE") && !state) {
            ComPtr<ID3D12Device> dev;
            HRESULT create=q->GetDevice(IID_PPV_ARGS(&dev));
            if(FAILED(create)) return create;
            auto desc=q->GetDesc();
            create=dev->CreateCommandQueue(&desc,IID_PPV_ARGS(&display));
            if(FAILED(create)) return create;
            display->SetName(L"XeFG Lab isolated display-only queue");
        }
        const HRESULT hr=t->original(f,isQueue ? display.Get():d,w,sd,fd,restrictOutput,result);
        if(SUCCEEDED(hr) && result && *result && !state) {
            if(isQueue) {
                auto next=std::make_unique<State>();next->Initialize(*result,q.Get(),t->applicationQueue.Get(),display.Get());''')
# Add explicit experiment metadata, never infer split success from the request.
anchor='         <<",\\\"failed\\\":"<<(failed?"true":"false")'
h=replace_once(h,anchor,'         <<",\\\"display_queue_separate_from_sdk\\\":"<<(displayQueue.Get()!=queue.Get()?"true":"false")\n'+anchor)
(root/"NativeGate.h").write_text(h,encoding="utf-8")
cm=root/"CMakeLists.txt"
text=cm.read_text(encoding="utf-8-sig")+'\nset_property(TARGET basic_xess_fg_sample PROPERTY CXX_STANDARD 17)\n'
cm.write_text(text,encoding="utf-8")
(root/"NATIVE-GATE-MANIFEST.json").write_text(json.dumps({
 "kind":"isolated_native_queue_gate_probe_not_game_patch",
 "source_before":before,"source_after":hashlib.sha256(p.read_bytes()).hexdigest(),
 "header_sha256":hashlib.sha256((root/"NativeGate.h").read_bytes()).hexdigest(),
 "absolute_deadlines":True,"split_display_queue_optional":True,
 "no_resolution_or_shader_changes":True,"no_original_present_drops":True,
 "gate_default":False,"split_default":False,"sleep_bypass_default":False,"game_modified":False
},indent=2),encoding="utf-8")
print("Native queue probe built with optional separate display queue; not a verified game patch.")
