#pragma once
// v26 RC1: experimental native XeFG presentation layer. Not a claim of perfect
// pacing or zero FPS cost. Only scoped secondary-XeFG creation installs this.
// Complete outputs are accepted into a bounded FIFO; actual Present errors are
// sticky and propagated to callers. Submission success is not scanout completion.
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace XeFGGamePacing {
using Microsoft::WRL::ComPtr;
inline double ClockMs() {
    static const double factor=[] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0/f.QuadPart; }();
    LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart*factor;
}
struct Settings {
    bool enabled=false, trace=false;
    double phaseMs=-2.2, fixedPeriodMs=0;
    UINT phaseParity=0;
    std::filesystem::path directory;
    static Settings Read() {
        Settings s; HMODULE module=nullptr; wchar_t path[32768]{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ClockMs), &module);
        if(!GetModuleFileNameW(module,path,32768)) return s;
        s.directory=std::filesystem::path(path).parent_path();
        auto ini=s.directory/L"XeFGPacing.ini";
        s.enabled=GetPrivateProfileIntW(L"OutputPacing",L"Enabled",0,ini.c_str())!=0;
        s.trace=GetPrivateProfileIntW(L"OutputPacing",L"Trace",0,ini.c_str())!=0;
        s.phaseParity=GetPrivateProfileIntW(L"OutputPacing",L"PhaseParity",0,ini.c_str())&1;
        auto number=[&](const wchar_t* key,double fallback,double minimum,double maximum) {
            wchar_t b[80]{}; GetPrivateProfileStringW(L"OutputPacing",key,L"",b,80,ini.c_str());
            wchar_t* end=nullptr; double v=wcstod(b,&end);
            return end!=b && *end==0 && std::isfinite(v) && v>=minimum && v<=maximum?v:fallback;
        };
        s.phaseMs=number(L"PhaseOffsetMs",-2.2,-4,4);
        s.fixedPeriodMs=number(L"SourcePeriodMs",0,0,100);
        if(s.fixedPeriodMs>0 && s.fixedPeriodMs<2) s.fixedPeriodMs=0;
        return s;
    }
};
inline HRESULT WaitIdle(ID3D12CommandQueue* q) {
    if(!q) return E_POINTER;
    ComPtr<ID3D12Device> d; HRESULT hr=q->GetDevice(IID_PPV_ARGS(&d)); if(FAILED(hr))return hr;
    ComPtr<ID3D12Fence> f; hr=d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)); if(FAILED(hr))return hr;
    HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!e)return HRESULT_FROM_WIN32(GetLastError());
    hr=q->Signal(f.Get(),1);
    if(SUCCEEDED(hr))hr=f->SetEventOnCompletion(1,e);
    if(SUCCEEDED(hr) && WaitForSingleObject(e,2000)!=WAIT_OBJECT_0)hr=DXGI_ERROR_DEVICE_HUNG;
    CloseHandle(e); return hr;
}
class Images {
    struct Slot {
        ComPtr<ID3D12Resource> source;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 copied=0;
    };
    std::vector<Slot> slots;
    std::vector<ComPtr<ID3D12Resource>> targets;
    ComPtr<ID3D12CommandQueue> output;
    ComPtr<ID3D12Fence> copiedFence;
    HANDLE event=nullptr;
    static void Barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* t,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x{}; x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource=t; x.Transition.StateBefore=a; x.Transition.StateAfter=b;
        x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; l->ResourceBarrier(1,&x);
    }
public:
    HRESULT Initialize(ID3D12Device* d,ID3D12CommandQueue* q,std::vector<ComPtr<ID3D12Resource>> buffers) {
        if(!d||!q||buffers.size()<2||buffers.size()>8)return E_INVALIDARG;
        output=q; targets=std::move(buffers); slots.resize(targets.size());
        HRESULT hr=d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&copiedFence));if(FAILED(hr))return hr;
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return HRESULT_FROM_WIN32(GetLastError());
        for(size_t i=0;i<slots.size();++i) {
            auto desc=targets[i]->GetDesc();
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels!=1 ||
               desc.DepthOrArraySize!=1 || desc.SampleDesc.Count!=1)return E_INVALIDARG;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            heap.CreationNodeMask=heap.VisibleNodeMask=1;
            auto& s=slots[i];
            hr=d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,
                nullptr,IID_PPV_ARGS(&s.source));if(FAILED(hr))return hr;
            hr=d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator));if(FAILED(hr))return hr;
            hr=d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list));if(FAILED(hr))return hr;
            hr=s.list->Close();if(FAILED(hr))return hr;
        }
        return S_OK;
    }
    HRESULT GetBuffer(UINT index,REFIID iid,void** value) {
        if(!value)return E_POINTER;*value=nullptr;
        return index<slots.size()?slots[index].source->QueryInterface(iid,value):E_INVALIDARG;
    }
    HRESULT Copy(UINT src,UINT dst,UINT64 serial) {
        if(src>=slots.size()||dst>=targets.size())return E_INVALIDARG;
        auto& s=slots[src];
        if(s.copied && copiedFence->GetCompletedValue()<s.copied) {
            auto hr=copiedFence->SetEventOnCompletion(s.copied,event);if(FAILED(hr))return hr;
            if(WaitForSingleObject(event,1500)!=WAIT_OBJECT_0)return DXGI_ERROR_DEVICE_HUNG;
        }
        HRESULT hr=s.allocator->Reset();if(FAILED(hr))return hr;
        hr=s.list->Reset(s.allocator.Get(),nullptr);if(FAILED(hr))return hr;
        Barrier(s.list.Get(),s.source.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(s.list.Get(),targets[dst].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
        s.list->CopyResource(targets[dst].Get(),s.source.Get());
        Barrier(s.list.Get(),targets[dst].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
        Barrier(s.list.Get(),s.source.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        hr=s.list->Close();if(FAILED(hr))return hr;
        ID3D12CommandList* lists[]={s.list.Get()};output->ExecuteCommandLists(1,lists);
        hr=output->Signal(copiedFence.Get(),serial);if(SUCCEEDED(hr))s.copied=serial;return hr;
    }
    ID3D12Fence* Fence()const{return copiedFence.Get();}
    ~Images(){if(event)CloseHandle(event);}
};
inline thread_local bool inNative=false;
struct NativeScope { bool old=inNative;NativeScope(){inNative=true;}~NativeScope(){inNative=old;} };
struct Packet {
    UINT64 id=0;UINT source=0,sync=0,flags=0,epoch=0;
    bool use1=false,paced=false,done=false;
    double period=0,submitted=0,deadline=0,presented=0;
    HRESULT result=E_PENDING;
    std::vector<RECT> dirty;bool scroll=false;RECT scrollRect{};POINT scrollOffset{};
};
struct Session {
    using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
    using Present1Fn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
    using BufferFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,REFIID,void**);
    using IndexFn=UINT(STDMETHODCALLTYPE*)(IDXGISwapChain3*);
    using ResizeFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
    using Resize1Fn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown*const*);
    using FullFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,BOOL,IDXGIOutput*);
    using ColorFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,DXGI_COLOR_SPACE_TYPE);
    using HdrFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain4*,DXGI_HDR_METADATA_TYPE,UINT,void*);
    using SizeFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain2*,UINT,UINT);
    void* owner=nullptr;Settings options;HWND hwnd=nullptr;
    ComPtr<IDXGISwapChain4> chain;ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> producer,display;
    ComPtr<ID3D12Fence> ready;
    std::unique_ptr<Images> images;
    std::array<void*,41> table{};void**oldTable=nullptr;
    PresentFn present=nullptr;Present1Fn present1=nullptr;BufferFn buffer=nullptr;IndexFn index=nullptr;
    ResizeFn resize=nullptr;Resize1Fn resize1=nullptr;FullFn fullscreen=nullptr;
    ColorFn color=nullptr;HdrFn hdr=nullptr;SizeFn sourceSize=nullptr;
    std::recursive_mutex submitMutex;
    std::mutex mutex,logMutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Packet>> packets;
    std::vector<UINT64> leases;
    std::thread worker;
    HANDLE readyEvent=nullptr,copyEvent=nullptr,timer=nullptr,stopEvent=nullptr;
    UINT cursor=0;UINT64 serial=0;
    size_t inFlight=0,peak=0;
    std::atomic<UINT64> accepted{0},completed{0};
    std::atomic<HRESULT> fatal{S_OK};std::atomic<bool> stopping{false},active{false};
    std::atomic<double> sourcePeriod{0};std::atomic<UINT> epoch{0};
    bool stopped=false;double nextDeadline=0,lastReport=0;UINT consumerEpoch=0;
    std::ofstream log,trace;
    void Log(const char* event,HRESULT hr=S_OK) {
        std::lock_guard<std::mutex> lock(logMutex);
        if(log){log<<std::fixed<<std::setprecision(3)<<ClockMs()<<" "<<event<<" hr="<<std::hex<<static_cast<unsigned>(hr)<<std::dec
            <<" accepted="<<accepted.load()<<" presented="<<completed.load()<<" active="<<active.load()
            <<" sourceMs="<<sourcePeriod.load()<<" phaseMs="<<options.phaseMs<<"\n";log.flush();}
    }
    HRESULT Rebuild() {
        DXGI_SWAP_CHAIN_DESC1 desc{};HRESULT hr=chain->GetDesc1(&desc);if(FAILED(hr))return hr;
        if(desc.BufferCount<2||desc.BufferCount>8)return E_INVALIDARG;
        std::vector<ComPtr<ID3D12Resource>> targets(desc.BufferCount);
        for(UINT i=0;i<desc.BufferCount;++i){hr=buffer(chain.Get(),i,IID_PPV_ARGS(&targets[i]));if(FAILED(hr))return hr;}
        auto fresh=std::make_unique<Images>();hr=fresh->Initialize(device.Get(),display.Get(),std::move(targets));if(FAILED(hr))return hr;
        ComPtr<ID3D12Fence> fence;hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));if(FAILED(hr))return hr;
        ready=std::move(fence);images=std::move(fresh);leases.assign(desc.BufferCount,0);cursor=index(chain.Get());serial=0;++epoch;
        Log("v26 RC1 resources-ready");return S_OK;
    }
    HRESULT Initialize(void* context,IDXGISwapChain1* sc,ID3D12CommandQueue* q,ID3D12CommandQueue* output,const Settings& cfg) {
        options=cfg;owner=context;producer=q;display=output;
        HRESULT hr=sc->QueryInterface(IID_PPV_ARGS(&chain));if(FAILED(hr)||chain.Get()!=sc)return E_NOINTERFACE;
        hr=q->GetDevice(IID_PPV_ARGS(&device));if(FAILED(hr))return hr;
        hr=chain->GetHwnd(&hwnd);if(FAILED(hr))return hr;
        oldTable=*reinterpret_cast<void***>(chain.Get());std::copy_n(oldTable,table.size(),table.begin());
        present=reinterpret_cast<PresentFn>(oldTable[8]);buffer=reinterpret_cast<BufferFn>(oldTable[9]);
        fullscreen=reinterpret_cast<FullFn>(oldTable[10]);resize=reinterpret_cast<ResizeFn>(oldTable[13]);
        present1=reinterpret_cast<Present1Fn>(oldTable[22]);sourceSize=reinterpret_cast<SizeFn>(oldTable[29]);
        index=reinterpret_cast<IndexFn>(oldTable[36]);color=reinterpret_cast<ColorFn>(oldTable[38]);
        resize1=reinterpret_cast<Resize1Fn>(oldTable[39]);hdr=reinterpret_cast<HdrFn>(oldTable[40]);
        log.open(options.directory/L"XeFGPacing.log",std::ios::app);
        if(options.trace){trace.open(options.directory/L"XeFGPacing-native.csv");trace<<"id,epoch,submitted_qpc_ms,present_qpc_ms,deadline_qpc_ms,source_period_ms,result\n";}
        hr=Rebuild();if(FAILED(hr))return hr;
        readyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);copyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        timer=CreateWaitableTimerExW(nullptr,nullptr,2,TIMER_ALL_ACCESS);if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        if(!readyEvent||!copyEvent||!stopEvent||!timer)return HRESULT_FROM_WIN32(GetLastError());
        Log("v26 RC1 output-pacing-attached");worker=std::thread([this]{Run();});return S_OK;
    }
    HRESULT Fail(HRESULT hr){fatal.store(hr);changed.notify_all();Log("terminal-error",hr);return hr;}
    HRESULT Protect(UINT i) {
        if(!images||i>=leases.size())return E_INVALIDARG;
        auto id=leases[i];if(id&&images->Fence()->GetCompletedValue()<id)return producer->Wait(images->Fence(),id);
        return S_OK;
    }
    void Source(double ms,bool enabled) {
        if(active.exchange(enabled)!=enabled)++epoch;
        if(std::isfinite(ms)&&ms>=2&&ms<=100){double old=sourcePeriod.load();sourcePeriod=old>0?old*.85+ms*.15:ms;}
    }
    bool FenceWait(ID3D12Fence* f,UINT64 ticket,HANDLE event) {
        if(f->GetCompletedValue()>=ticket)return true;
        if(FAILED(f->SetEventOnCompletion(ticket,event)))return false;
        return WaitForSingleObject(event,1500)==WAIT_OBJECT_0&&f->GetCompletedValue()>=ticket;
    }
    HRESULT Drain() {
        std::unique_lock<std::mutex> lock(mutex);
        if(!changed.wait_for(lock,std::chrono::milliseconds(2000),[&]{return inFlight==0||FAILED(fatal.load());}))return Fail(DXGI_ERROR_DEVICE_HUNG);
        return fatal.load();
    }
    HRESULT Submit(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params,bool use1) {
        std::lock_guard<std::recursive_mutex> submissions(submitMutex);
        if(FAILED(fatal.load()))return fatal.load();if(stopping||!images)return E_ABORT;
        if(sync>4)return DXGI_ERROR_INVALID_CALL;
        if(flags&DXGI_PRESENT_TEST){auto hr=Drain();if(FAILED(hr))return hr;NativeScope scope;
            return use1?present1(chain.Get(),sync,flags,params):present(chain.Get(),sync,flags);}
        auto p=std::make_shared<Packet>();p->sync=sync;p->flags=flags;p->use1=use1;
        if(params){
            if(params->DirtyRectsCount){if(!params->pDirtyRects||params->DirtyRectsCount>4096)return E_INVALIDARG;
                p->dirty.assign(params->pDirtyRects,params->pDirtyRects+params->DirtyRectsCount);}
            if(params->pScrollRect||params->pScrollOffset){if(!params->pScrollRect||!params->pScrollOffset)return E_INVALIDARG;
                p->scroll=true;p->scrollRect=*params->pScrollRect;p->scrollOffset=*params->pScrollOffset;}
        }
        const bool synchronous=sync!=0 || (flags&~DXGI_PRESENT_ALLOW_TEARING)!=0 || !active.load() || IsIconic(hwnd) || !IsWindowVisible(hwnd);
        // Nonstandard DXGI flags keep synchronous error/occlusion semantics.
        // DO_NOT_WAIT is not transformed into an asynchronous success result.
        if(flags&DXGI_PRESENT_DO_NOT_WAIT)return DXGI_ERROR_WAS_STILL_DRAWING;
        if(synchronous){auto hr=Drain();if(FAILED(hr))return hr;}
        std::unique_lock<std::mutex> lock(mutex);
        if(!changed.wait_for(lock,std::chrono::milliseconds(1500),[&]{return inFlight<leases.size()||FAILED(fatal.load())||stopping.load();}))return Fail(DXGI_ERROR_DEVICE_HUNG);
        if(stopping||FAILED(fatal.load()))return FAILED(fatal.load())?fatal.load():E_ABORT;
        p->id=++serial;p->source=cursor;p->epoch=epoch.load();p->submitted=ClockMs();
        p->period=options.fixedPeriodMs>0?options.fixedPeriodMs:sourcePeriod.load();
        p->paced=!synchronous&&p->period>=2&&p->period<=100;
        HRESULT hr=producer->Signal(ready.Get(),p->id);if(FAILED(hr))return Fail(hr);
        leases[cursor]=p->id;cursor=(cursor+1)%static_cast<UINT>(leases.size());
        packets.push_back(p);++inFlight;++accepted;peak=(std::max)(peak,inFlight);
        hr=Protect(cursor);if(FAILED(hr))return Fail(hr);
        lock.unlock();changed.notify_all();
        if(synchronous){
            lock.lock();
            if(!changed.wait_for(lock,std::chrono::milliseconds(2000),[&]{return p->done||FAILED(fatal.load());}))return Fail(DXGI_ERROR_DEVICE_HUNG);
            return p->done?p->result:fatal.load();
        }
        return S_OK;
    }
    void Run()noexcept {
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
        try {for(;;){
            std::shared_ptr<Packet> p;
            {std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return stopping||!packets.empty();});
                if(packets.empty()){if(stopping)break;else continue;}p=packets.front();packets.pop_front();}
            HRESULT hr=S_OK;
            if(!FenceWait(ready.Get(),p->id,readyEvent))hr=DXGI_ERROR_DEVICE_HUNG;
            UINT target=index(chain.Get());
            if(SUCCEEDED(hr))hr=display->Wait(ready.Get(),p->id);
            if(SUCCEEDED(hr))hr=images->Copy(p->source,target,p->id);
            if(SUCCEEDED(hr)&&!FenceWait(images->Fence(),p->id,copyEvent))hr=DXGI_ERROR_DEVICE_HUNG;
            double now=ClockMs();
            if(consumerEpoch!=p->epoch){nextDeadline=0;consumerEpoch=p->epoch;}
            if(SUCCEEDED(hr)&&p->paced&&active&&!stopping){
                double step=p->period*.5;
                nextDeadline=nextDeadline>0?nextDeadline+step:now+step;
                if(now>nextDeadline+step || nextDeadline>now+p->period)nextDeadline=now;
                double bound=p->period*.24;
                double offset=(p->id%2)==options.phaseParity?(std::max)(-bound,(std::min)(bound,options.phaseMs)):0;
                p->deadline=nextDeadline+offset;
                const double wait=p->deadline-ClockMs();
                if(wait>0){LARGE_INTEGER due;due.QuadPart=-static_cast<LONGLONG>(std::ceil(wait*10000));
                    if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)){HANDLE hs[]={timer,stopEvent};WaitForMultipleObjects(2,hs,FALSE,100);}}
            }else{nextDeadline=0;p->deadline=now;}
            p->presented=ClockMs();
            if(SUCCEEDED(hr)){
                DXGI_PRESENT_PARAMETERS args{};args.DirtyRectsCount=static_cast<UINT>(p->dirty.size());args.pDirtyRects=p->dirty.empty()?nullptr:p->dirty.data();
                args.pScrollRect=p->scroll?&p->scrollRect:nullptr;args.pScrollOffset=p->scroll?&p->scrollOffset:nullptr;
                NativeScope scope;hr=p->use1?present1(chain.Get(),p->sync,p->flags,&args):present(chain.Get(),p->sync,p->flags);++completed;
            }
            if(trace){trace<<p->id<<','<<p->epoch<<','<<std::fixed<<std::setprecision(5)<<p->submitted<<','<<p->presented<<','<<p->deadline<<','<<p->period<<','<<static_cast<long>(hr)<<'\n';}
            {std::lock_guard<std::mutex> lock(mutex);p->result=hr;p->done=true;--inFlight;}
            changed.notify_all();
            if(FAILED(hr)){Fail(hr);images->Fence()->Signal(serial);break;} // terminal cancellation only; never reported as a completed copy
            if(ClockMs()-lastReport>1000){Log("output-timing");if(trace)trace.flush();lastReport=ClockMs();}
        }}catch(...){Fail(E_UNEXPECTED);if(images)images->Fence()->Signal(serial);}
    }
    HRESULT Resize(UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags,const UINT* masks,IUnknown*const* queues,bool use1) {
        std::lock_guard<std::recursive_mutex> submissions(submitMutex);
        auto hr=Drain();if(FAILED(hr))return hr;
        hr=WaitIdle(producer.Get());if(FAILED(hr))return Fail(hr);
        hr=WaitIdle(display.Get());if(FAILED(hr))return Fail(hr);
        const UINT oldCount=static_cast<UINT>(leases.size());
        if((count&&count<2)||count>8)return DXGI_ERROR_INVALID_CALL;
        if(queues){for(UINT i=0;i<(count?count:oldCount);++i){ComPtr<ID3D12CommandQueue> q;ComPtr<ID3D12Device> d;
            if(!queues[i]||FAILED(queues[i]->QueryInterface(IID_PPV_ARGS(&q)))||FAILED(q->GetDevice(IID_PPV_ARGS(&d))))return E_INVALIDARG;
            auto a=d->GetAdapterLuid(),b=device->GetAdapterLuid();if(a.HighPart!=b.HighPart||a.LowPart!=b.LowPart)return DXGI_ERROR_UNSUPPORTED;}}
        images.reset();ready.Reset(); // ALL owned references to real DXGI buffers released before ResizeBuffers
        NativeScope scope;std::vector<IUnknown*> mapped(count?count:oldCount,display.Get());
        HRESULT result=use1?resize1(chain.Get(),count,width,height,format,flags,masks,queues?mapped.data():nullptr):resize(chain.Get(),count,width,height,format,flags);
        hr=Rebuild();if(FAILED(hr))return Fail(hr);
        Log("resize-rebuilt",result);return result;
    }
    template<class F> HRESULT Control(F&& call) {
        std::lock_guard<std::recursive_mutex> submissions(submitMutex);
        auto hr=Drain();if(FAILED(hr))return hr;NativeScope scope;return call();
    }
    HRESULT Stop() {
        if(stopped)return fatal.load();
        active=false;++epoch;SetEvent(stopEvent);
        HRESULT hr=Drain();stopping=true;changed.notify_all();
        if(worker.joinable())worker.join();stopped=true;
        HRESULT p=WaitIdle(producer.Get()),d=WaitIdle(display.Get());
        if(FAILED(p)||FAILED(d))hr=FAILED(p)?p:d;
        Log("v26 RC1 output-pacing-detached",hr);return hr;
    }
    ~Session(){if(worker.joinable())Stop();for(HANDLE h:{readyEvent,copyEvent,timer,stopEvent})if(h)CloseHandle(h);}
};
// Explicit SDK-lifetime ownership, not a static destructor joining from DllMain.
inline std::atomic<Session*> session{nullptr};
inline Session* Find(void* chain){auto s=session.load();return s&&s->chain.Get()==chain?s:nullptr;}
inline HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain*c,UINT i,UINT f){auto s=Find(c);if(!s)return E_UNEXPECTED;if(inNative)return s->present(c,i,f);try{return s->Submit(i,f,nullptr,false);}catch(...){return s->Fail(E_OUTOFMEMORY);}}
inline HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1*c,UINT i,UINT f,const DXGI_PRESENT_PARAMETERS*p){auto s=Find(c);if(!s)return E_UNEXPECTED;if(inNative)return s->present1(c,i,f,p);try{return s->Submit(i,f,p,true);}catch(...){return s->Fail(E_OUTOFMEMORY);}}
inline HRESULT STDMETHODCALLTYPE HookBuffer(IDXGISwapChain*c,UINT i,REFIID iid,void**p){auto s=Find(c);if(!s)return E_UNEXPECTED;if(inNative)return s->buffer(c,i,iid,p);std::lock_guard<std::recursive_mutex> lock(s->submitMutex);return s->images?s->images->GetBuffer(i,iid,p):E_ABORT;}
inline UINT STDMETHODCALLTYPE HookIndex(IDXGISwapChain3*c){auto s=Find(c);if(!s)return 0;if(inNative)return s->index(c);std::lock_guard<std::recursive_mutex> lock(s->submitMutex);if(FAILED(s->Protect(s->cursor)))s->Fail(E_FAIL);return s->cursor;}
inline HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain*c,UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags){auto s=Find(c);if(!s)return E_UNEXPECTED;if(inNative)return s->resize(c,n,w,h,f,flags);try{return s->Resize(n,w,h,f,flags,nullptr,nullptr,false);}catch(...){return s->Fail(E_OUTOFMEMORY);}}
inline HRESULT STDMETHODCALLTYPE HookResize1(IDXGISwapChain3*c,UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags,const UINT*m,IUnknown*const*q){auto s=Find(c);if(!s)return E_UNEXPECTED;if(inNative)return s->resize1(c,n,w,h,f,flags,m,q);try{return s->Resize(n,w,h,f,flags,m,q,true);}catch(...){return s->Fail(E_OUTOFMEMORY);}}
inline HRESULT STDMETHODCALLTYPE HookFull(IDXGISwapChain*c,BOOL full,IDXGIOutput*o){auto s=Find(c);if(!s)return E_UNEXPECTED;if(inNative)return s->fullscreen(c,full,o);return s->Control([&]{return s->fullscreen(c,full,o);});}
inline HRESULT STDMETHODCALLTYPE HookColor(IDXGISwapChain3*c,DXGI_COLOR_SPACE_TYPE color){auto s=Find(c);if(!s)return E_UNEXPECTED;if(inNative)return s->color(c,color);return s->Control([&]{return s->color(c,color);});}
inline HRESULT STDMETHODCALLTYPE HookHdr(IDXGISwapChain4*c,DXGI_HDR_METADATA_TYPE t,UINT n,void*p){auto s=Find(c);if(!s)return E_UNEXPECTED;if(inNative)return s->hdr(c,t,n,p);return s->Control([&]{return s->hdr(c,t,n,p);});}
inline HRESULT STDMETHODCALLTYPE HookSize(IDXGISwapChain2*c,UINT w,UINT h){auto s=Find(c);if(!s)return E_UNEXPECTED;if(inNative)return s->sourceSize(c,w,h);return s->Control([&]{return s->sourceSize(c,w,h);});}
inline void Source(void* owner,double ms,bool enabled){auto s=session.load();if(s&&s->owner==owner)s->Source(ms,enabled);}
inline void Pause(void* owner){auto s=session.load();if(s&&s->owner==owner){s->active=false;++s->epoch;s->Drain();}}
inline void Finish(void* owner){auto s=session.load();if(!s||s->owner!=owner)return;
    std::lock_guard<std::recursive_mutex> lock(s->submitMutex);auto hr=s->Stop();
    *reinterpret_cast<void***>(s->chain.Get())=s->oldTable;session=nullptr;
    // On device loss retain GPU-referenced allocations until process exit rather
    // than freeing memory that might still be referenced by cancelled work.
    if(SUCCEEDED(hr)){/* deleted after releasing the member lock below */}
}
inline void ReleaseFinished(Session* s){if(s&&s->stopped&&SUCCEEDED(s->fatal.load()))delete s;}
class FactoryScope {
    using CreateFn=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
    ComPtr<IDXGIFactory7> factory;ComPtr<ID3D12Device> expectedDevice;
    std::array<void*,32> table{};void**old=nullptr;CreateFn original=nullptr;
    Settings config;void*owner=nullptr;HWND target=nullptr;
    inline static FactoryScope* current=nullptr;
    static HRESULT STDMETHODCALLTYPE Create(IDXGIFactory2*f,IUnknown*d,HWND w,const DXGI_SWAP_CHAIN_DESC1*desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*full,IDXGIOutput*restrictOutput,IDXGISwapChain1**result) {
        auto t=current;if(!t)return E_UNEXPECTED;
        ComPtr<ID3D12CommandQueue> q;ComPtr<ID3D12Device> dev;
        if(session.load()||w!=t->target||!d||FAILED(d->QueryInterface(IID_PPV_ARGS(&q)))||FAILED(q->GetDevice(IID_PPV_ARGS(&dev))))return t->original(f,d,w,desc,full,restrictOutput,result);
        auto a=dev->GetAdapterLuid(),b=t->expectedDevice->GetAdapterLuid();
        if(a.HighPart!=b.HighPart||a.LowPart!=b.LowPart)return t->original(f,d,w,desc,full,restrictOutput,result);
        if(!desc||desc->BufferCount<2||desc->BufferCount>8||desc->SampleDesc.Count!=1)return t->original(f,d,w,desc,full,restrictOutput,result);
        ComPtr<ID3D12CommandQueue> output;auto qdesc=q->GetDesc();qdesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;qdesc.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        HRESULT hr=dev->CreateCommandQueue(&qdesc,IID_PPV_ARGS(&output));if(FAILED(hr))return hr;
        hr=t->original(f,output.Get(),w,desc,full,restrictOutput,result);if(FAILED(hr))return hr;
        try {
            auto s=std::make_unique<Session>();hr=s->Initialize(t->owner,*result,q.Get(),output.Get(),t->config);
            if(FAILED(hr)){(*result)->Release();*result=nullptr;return hr;}
            s->table[8]=reinterpret_cast<void*>(&HookPresent);s->table[9]=reinterpret_cast<void*>(&HookBuffer);
            s->table[10]=reinterpret_cast<void*>(&HookFull);s->table[13]=reinterpret_cast<void*>(&HookResize);
            s->table[22]=reinterpret_cast<void*>(&HookPresent1);s->table[29]=reinterpret_cast<void*>(&HookSize);
            s->table[36]=reinterpret_cast<void*>(&HookIndex);s->table[38]=reinterpret_cast<void*>(&HookColor);
            s->table[39]=reinterpret_cast<void*>(&HookResize1);s->table[40]=reinterpret_cast<void*>(&HookHdr);
            auto raw=s.release();session=raw;*reinterpret_cast<void***>(raw->chain.Get())=raw->table.data();return hr;
        }catch(...){if(result&&*result){(*result)->Release();*result=nullptr;}return E_OUTOFMEMORY;}
    }
public:
    FactoryScope(void* context,IDXGIFactory2* f,ID3D12CommandQueue* app,HWND w):owner(context),target(w) {
        config=Settings::Read();if(!config.enabled||current||session.load()||!f||!app)return;
        if(FAILED(app->GetDevice(IID_PPV_ARGS(&expectedDevice)))||FAILED(f->QueryInterface(IID_PPV_ARGS(&factory)))||factory.Get()!=f){factory.Reset();return;}
        old=*reinterpret_cast<void***>(f);std::copy_n(old,table.size(),table.begin());
        original=reinterpret_cast<CreateFn>(old[15]);table[15]=reinterpret_cast<void*>(&Create);
        current=this;*reinterpret_cast<void***>(f)=table.data();
    }
    ~FactoryScope(){if(old&&factory){*reinterpret_cast<void***>(factory.Get())=old;current=nullptr;}}
};
} // namespace XeFGGamePacing
