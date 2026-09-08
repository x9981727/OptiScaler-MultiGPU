from pathlib import Path
import hashlib, json, shutil
kit=Path(__file__).resolve().parents[1]
root=kit/'upstream/OptiScaler'
p=root/'framegen/xefg/GameOutputPacing.h'
s=p.read_text(encoding='utf-8')
old_hash=hashlib.sha256(p.read_bytes()).hexdigest()
(kit/'v26/GameOutputPacing-before-rc3.h').write_text(s,encoding='utf-8')
def once(a,b):
    global s
    if s.count(a)!=1:raise RuntimeError('RC3 anchor not unique: '+a[:140])
    s=s.replace(a,b)
once('#include "PhaseOnlyDeadline.h"','#include "PhaseOnlyDeadline.h"\n#include "WindowThreadWait.h"')
once('inline HRESULT WaitIdle(ID3D12CommandQueue* q)', 'inline HRESULT WaitIdle(ID3D12CommandQueue* q,HWND window=nullptr)')
once('WaitForSingleObject(e,2000)','WindowHandleWait(window,e,2000)')
once('std::recursive_mutex submitMutex;','WindowSubmissionMutex submitMutex;')
s=s.replace('std::lock_guard<std::recursive_mutex>','std::lock_guard<WindowSubmissionMutex>')
once('HANDLE readyEvent=nullptr,copyEvent=nullptr,timer=nullptr,stopEvent=nullptr,workEvent=nullptr;',
     'HANDLE readyEvent=nullptr,copyEvent=nullptr,timer=nullptr,stopEvent=nullptr,workEvent=nullptr,progressEvent=nullptr,workerExitEvent=nullptr;')
once('hr=chain->GetHwnd(&hwnd);if(FAILED(hr))return hr;',
     'hr=chain->GetHwnd(&hwnd);if(FAILED(hr))return hr;submitMutex.SetWindow(hwnd);')
once('workEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);',
     'workEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);\n        progressEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);workerExitEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);')
once('if(!readyEvent||!copyEvent||!stopEvent||!timer||!workEvent)',
     'if(!readyEvent||!copyEvent||!stopEvent||!timer||!workEvent||!progressEvent||!workerExitEvent)')
once('return WaitForSingleObject(event,1500)==WAIT_OBJECT_0&&f->GetCompletedValue()>=ticket;',
     'return WindowHandleWait(hwnd,event,1500)==WAIT_OBJECT_0&&f->GetCompletedValue()>=ticket;')
for ms,predicate in [(2000,'inFlight==0||FAILED(fatal.load())'),(1500,'inFlight<leases.size()||FAILED(fatal.load())||stopping.load()')]:
    a=f'changed.wait_for(lock,std::chrono::milliseconds({ms}),[&]{{return {predicate};}})'
    b=f'WindowConditionWait(hwnd,progressEvent,changed,lock,{ms},[&]{{return {predicate};}})'
    once(a,b)
# Preserve the caller thread for sync/occluded/non-FG presentation. The worker
# is already drained here; do not enqueue and then block that same caller.
once('packets.push_back(p);++inFlight;++accepted;peak=(std::max)(peak,inFlight);',
     'if(!synchronous)packets.push_back(p);++inFlight;++accepted;peak=(std::max)(peak,inFlight);')
once('''        if(synchronous){
            lock.lock();
            if(!changed.wait_for(lock,std::chrono::milliseconds(2000),[&]{return p->done||FAILED(fatal.load());}))return Fail(DXGI_ERROR_DEVICE_HUNG);
            return p->done?p->result:fatal.load();
        }''','''        if(synchronous){
            Execute(p); // original calling thread, no additional thread handoff
            return p->result;
        }''')
# Extract the original exact copy+Present body; both execution paths use it.
a=s.index('            HRESULT hr=S_OK;\n            if(!FenceWait')
b=s.index('\n        }}catch(...){',a)
body=s[a:b]
body=body.replace('if(FAILED(hr)){Fail(hr);images->Fence()->Signal(serial);break;}',
                  'if(FAILED(hr)){Fail(hr);images->Fence()->Signal(serial);return;}')
start=s.index('    void Run()noexcept {')
end=s.index('    HRESULT Resize(',b)
s=s[:start]+'''    void Execute(const std::shared_ptr<Packet>& p) {
'''+body+'''
    }
    void Run()noexcept {
        SignalOnExit exited{workerExitEvent};
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
        try {for(;;){
            std::shared_ptr<Packet> p;
            {std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return stopping||!packets.empty();});
                if(packets.empty()){if(stopping)break;else continue;}p=packets.front();packets.pop_front();}
            Execute(p);
            if(FAILED(fatal.load()))break;
        }}catch(...){Fail(E_UNEXPECTED);if(images)images->Fence()->Signal(serial);}
    }
'''+s[end:]
s=s.replace('changed.notify_all();','SetEvent(progressEvent);changed.notify_all();')
s=s.replace('WaitIdle(producer.Get())','WaitIdle(producer.Get(),hwnd)').replace('WaitIdle(display.Get())','WaitIdle(display.Get(),hwnd)')
once('if(worker.joinable())worker.join();stopped=true;', '''if(worker.joinable()){
            if(WindowHandleWait(hwnd,workerExitEvent,2000)!=WAIT_OBJECT_0)
                return Fail(DXGI_ERROR_DEVICE_HUNG); // keep live worker allocations, never unbounded UI join
            worker.join();
        }
        stopped=true;''')
once('for(HANDLE h:{readyEvent,copyEvent,timer,stopEvent,workEvent})',
     'for(HANDLE h:{readyEvent,copyEvent,timer,stopEvent,workEvent,progressEvent,workerExitEvent})')
# ResizeTarget had not been serialized at all. Native fullscreen/mode work may
# complete only at a later Present, so it must obey the same drain contract.
once('using FullFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,BOOL,IDXGIOutput*);',
     'using FullFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,BOOL,IDXGIOutput*);\n    using TargetFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,const DXGI_MODE_DESC*);')
once('ColorFn color=nullptr;HdrFn hdr=nullptr;SizeFn sourceSize=nullptr;',
     'ColorFn color=nullptr;HdrFn hdr=nullptr;SizeFn sourceSize=nullptr;TargetFn resizeTarget=nullptr;')
once('fullscreen=reinterpret_cast<FullFn>(oldTable[10]);resize=',
     'resizeTarget=reinterpret_cast<TargetFn>(oldTable[14]);\n        fullscreen=reinterpret_cast<FullFn>(oldTable[10]);resize=')
once('inline HRESULT STDMETHODCALLTYPE HookFull(','''inline HRESULT STDMETHODCALLTYPE HookTarget(IDXGISwapChain*c,const DXGI_MODE_DESC*d){auto s=Find(c);if(!s)return E_UNEXPECTED;if(inNative)return s->resizeTarget(c,d);s->Log("resize-target-begin");auto hr=s->Control([&]{return s->resizeTarget(c,d);});s->Log("resize-target-end",hr);return hr;}
inline HRESULT STDMETHODCALLTYPE HookFull(''')
once('s->table[10]=reinterpret_cast<void*>(&HookFull);s->table[13]=reinterpret_cast<void*>(&HookResize);',
     's->table[10]=reinterpret_cast<void*>(&HookFull);s->table[13]=reinterpret_cast<void*>(&HookResize);\n            s->table[14]=reinterpret_cast<void*>(&HookTarget);')
once('''        auto hr=Drain();if(FAILED(hr))return hr;
        hr=WaitIdle''','''        Log("resize-begin");
        auto hr=Drain();if(FAILED(hr))return hr;
        hr=WaitIdle''')
once('<<" mode=phase-only fixedRateClock=removed pressureBypasses="',
     '<<" windowThread="<<GetWindowThreadProcessId(hwnd,nullptr)<<" callThread="<<GetCurrentThreadId()<<" sentMessageServices="<<sentMessageServiceCalls.load()\n            <<" mode=phase-only fixedRateClock=removed pressureBypasses="')
s=s.replace('v26 RC2','v26 RC3')
assert 'nextDeadline' not in s and 'fixedPeriodMs' not in s
assert 'std::condition_variable' in s
p.write_text(s,encoding='utf-8');(kit/'v26/GameOutputPacing-compiled.h').write_text(s,encoding='utf-8')
for directory in [p.parent,kit/'v26']:
    shutil.copyfile(kit/'v26rc3/WindowThreadWait.h',directory/'WindowThreadWait.h')
q=root/'framegen/xefg/XeFG_Dx12.cpp';text=q.read_text(encoding='utf-8');q.write_text(text.replace('v26 RC2','v26 RC3'),encoding='utf-8')
m=kit/'v26/INTEGRATION-MANIFEST.json';d=json.loads(m.read_text());d.update(kind='OptiScaler-v26-RC3-window-thread-game-candidate',candidate='v26 RC3 window-thread hotfix',actual_game_test_passed=False,changed_pacing_policy=False,fixed_fps_cap_added=False,window_sent_message_waits=True,synchronous_present_keeps_caller_thread=True,resize_target_serialized=True)
d['source_changes']={str(f.relative_to(root)).replace('\\','/'):hashlib.sha256(f.read_bytes()).hexdigest() for f in [p,q,p.parent/'PhaseOnlyDeadline.h',p.parent/'WindowThreadWait.h']}
d['window_hotfix']={'before_header_sha256':old_hash,'after_header_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'source':hashlib.sha256(q.read_bytes()).hexdigest()};m.write_text(json.dumps(d,indent=2),encoding='utf-8')
print(json.dumps(d),flush=True)
