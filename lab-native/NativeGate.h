#pragma once
// Isolated lab only: meter the SDK's native display queue, never the render queue.
// No dropped calls, image copies, changed pixels, or invented Present results.
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace NativeGateLab {
using Microsoft::WRL::ComPtr;
inline double QpcMs() {
    LARGE_INTEGER a,b; QueryPerformanceCounter(&a); QueryPerformanceFrequency(&b);
    return 1000.0 * static_cast<double>(a.QuadPart)/static_cast<double>(b.QuadPart);
}
inline bool EnvFlag(const char* n) {
    char b[32]{}; return GetEnvironmentVariableA(n,b,sizeof(b)) && b[0]=='1';
}
inline std::string EnvPath() {
    char b[32768]{}; DWORD n=GetEnvironmentVariableA("XEFG_LAB_TRACE_DIR",b,sizeof(b));
    return n && n<sizeof(b) ? std::string(b) : ".";
}
inline std::atomic<double> sourcePeriod{0.0};
inline std::atomic<bool> active{false};
inline void Source(double ms,bool enabled) {
    active.store(enabled);
    if(std::isfinite(ms) && ms>=2.0 && ms<=100.0) {
        const double old=sourcePeriod.load();
        sourcePeriod.store(old>0 ? old*0.85+ms*0.15 : ms);
    }
}
inline float Hint(float actual) {
    char b[32]{}; if(!GetEnvironmentVariableA("XEFG_LAB_HINT_MS",b,sizeof(b))) return actual;
    char* end=nullptr; double x=strtod(b,&end);
    return end!=b && *end==0 && std::isfinite(x) && x>=0 && x<100 ? static_cast<float>(x):actual;
}
struct Record {
    UINT64 id=0; DWORD thread=0; UINT index=0, sync=0, flags=0;
    double enter=0, leave=0, ready=0, deadline=0, released=0, period=0;
    HRESULT result=S_OK, signalHr=S_OK, waitHr=S_OK;
    bool gate=false;
};
struct State {
    using PresentFn=HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
    using Present1Fn=HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
    ComPtr<IDXGISwapChain4> chain;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> readyFence,releaseFence;
    void** originalVtable=nullptr;
    std::array<void*,41> vtable{};
    PresentFn originalPresent=nullptr;
    Present1Fn originalPresent1=nullptr;
    HANDLE readyEvent=nullptr,stopEvent=nullptr,timer=nullptr;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<Record> records;
    std::deque<size_t> tickets;
    std::thread worker;
    std::atomic<bool> stop{false},failed{false};
    std::atomic<UINT64> lastSerial{0};
    bool sameQueue=false, requested=false;
    unsigned timeouts=0,overflows=0;
    double epoch=QpcMs(),lastRelease=0;
    std::string path=EnvPath();
    State() { records.reserve(200000); }
    void Initialize(IDXGISwapChain1* sc,ID3D12CommandQueue* q,ID3D12CommandQueue* app) {
        if(FAILED(sc->QueryInterface(IID_PPV_ARGS(&chain))) || chain.Get()!=sc)
            throw std::runtime_error("Native swapchain identity unsupported");
        queue=q; sameQueue=q==app; requested=EnvFlag("XEFG_LAB_GATE");
        ComPtr<ID3D12Device> dev;
        if(FAILED(q->GetDevice(IID_PPV_ARGS(&dev))) ||
           FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&readyFence))) ||
           FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&releaseFence))))
            throw std::runtime_error("Native queue diagnostic fences unavailable");
        readyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        timer=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);
        if(!timer) timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        if(!readyEvent || !stopEvent || !timer) throw std::runtime_error("Wait handles unavailable");
        originalVtable=*reinterpret_cast<void***>(chain.Get());
        std::copy_n(originalVtable,vtable.size(),vtable.begin());
        originalPresent=reinterpret_cast<PresentFn>(originalVtable[8]);
        originalPresent1=reinterpret_cast<Present1Fn>(originalVtable[22]);
        worker=std::thread([this]{Worker();});
    }
    size_t Before(UINT sync,UINT flags) {
        const double now=QpcMs();
        Record r; r.thread=GetCurrentThreadId(); r.enter=now-epoch;
        r.index=chain->GetCurrentBackBufferIndex(); r.sync=sync;r.flags=flags;
        r.period=sourcePeriod.load();
        std::unique_lock<std::mutex> lock(mutex);
        if(records.size()>=199990 || stop || failed) return SIZE_MAX;
        r.id=++lastSerial;
        r.gate=requested && !sameQueue && active.load() && sync==0 &&
               !(flags&(DXGI_PRESENT_TEST|DXGI_PRESENT_DO_NOT_WAIT)) && r.period>0;
        // SDK calls are not skipped if the lab queue cannot keep up.
        if(tickets.size()>=8) { r.gate=false; ++overflows; failed=true; }
        size_t i=records.size(); records.push_back(r);
        HRESULT a=queue->Signal(readyFence.Get(),r.id);
        HRESULT b=S_OK;
        if(SUCCEEDED(a) && r.gate) b=queue->Wait(releaseFence.Get(),r.id);
        records[i].signalHr=a;records[i].waitHr=b;
        if(FAILED(a)||FAILED(b)) {
            failed=true;releaseFence->Signal(lastSerial.load());
            return i;
        }
        tickets.push_back(i);
        lock.unlock();condition.notify_one();
        return i;
    }
    void After(size_t i,HRESULT h) {
        if(i==SIZE_MAX)return;
        std::lock_guard<std::mutex> lock(mutex);
        records[i].leave=QpcMs()-epoch;records[i].result=h;
    }
    void Worker() noexcept {
        try {
            for(;;) {
                size_t i;Record r;
                { std::unique_lock<std::mutex> lock(mutex);
                  condition.wait(lock,[&]{return stop || !tickets.empty();});
                  if(tickets.empty()) { if(stop)break;else continue; }
                  i=tickets.front();tickets.pop_front();r=records[i]; }
                const double waitStart=QpcMs();
                if(readyFence->GetCompletedValue()<r.id) {
                    HRESULT hr=readyFence->SetEventOnCompletion(r.id,readyEvent);
                    if(FAILED(hr)){failed=true;}
                    while(!failed && readyFence->GetCompletedValue()<r.id) {
                        if(stop) releaseFence->Signal(lastSerial.load());
                        WaitForSingleObject(readyEvent,2);
                        if(QpcMs()-waitStart>1500.0) {++timeouts;failed=true;break;}
                    }
                }
                const double ready=QpcMs();
                double target=ready;
                if(r.gate && !stop && !failed && active.load()) {
                    // Move only display release times. Shader work is already submitted.
                    target=std::max(ready,lastRelease+r.period*0.5);
                    if(target-ready>r.period) target=ready; // no accumulated latency debt
                    const double delay=target-QpcMs();
                    if(delay>0.02) {
                        LARGE_INTEGER due;due.QuadPart=-static_cast<LONGLONG>(std::ceil(delay*10000.0));
                        if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)) {
                            HANDLE hs[]={timer,stopEvent};
                            WaitForMultipleObjects(2,hs,FALSE,100);
                        }
                    }
                }
                // Always release our own waits, including failure/stop paths.
                const UINT64 release=failed || stop ? lastSerial.load():r.id;
                if(releaseFence->GetCompletedValue()<release) releaseFence->Signal(release);
                const double released=QpcMs();
                lastRelease=released;
                {std::lock_guard<std::mutex> lock(mutex);
                 records[i].ready=ready-epoch; records[i].deadline=target-epoch;
                 records[i].released=released-epoch;}
            }
        } catch(...) {failed=true;releaseFence->Signal(lastSerial.load());}
    }
    void Shutdown() {
        active=false;stop=true;SetEvent(stopEvent);
        releaseFence->Signal(lastSerial.load());condition.notify_all();
        if(worker.joinable())worker.join();
        if(chain && originalVtable)*reinterpret_cast<void***>(chain.Get())=originalVtable;
        std::ofstream f(path+"\\native-present.csv");
        f<<"id,thread,index,sync,flags,enter_ms,return_ms,gpu_ready_observed_ms,deadline_ms,release_ms,source_period_ms,gated,result,signal_hr,wait_hr\n";
        f<<std::fixed<<std::setprecision(5);
        for(const auto& r:records)
            f<<r.id<<','<<r.thread<<','<<r.index<<','<<r.sync<<','<<r.flags<<','<<r.enter<<','<<r.leave<<','
             <<r.ready<<','<<r.deadline<<','<<r.released<<','<<r.period<<','<<r.gate<<','
             <<static_cast<long>(r.result)<<','<<static_cast<long>(r.signalHr)<<','<<static_cast<long>(r.waitHr)<<'\n';
        std::ofstream m(path+"\\native-gate-meta.json");
        m<<"{\"kind\":\"isolated_native_display_queue_experiment\",\"requested_gate\":"<<(requested?"true":"false")
         <<",\"same_queue_as_render\":"<<(sameQueue?"true":"false")
         <<",\"failed\":"<<(failed?"true":"false")<<",\"ready_timeouts\":"<<timeouts
         <<",\"overflows\":"<<overflows<<",\"records\":"<<records.size()
         <<",\"pixels_or_resolution_changed\":false,\"game_modified\":false}\n";
    }
    ~State() {
        if(worker.joinable())Shutdown();
        if(readyEvent)CloseHandle(readyEvent);
        if(stopEvent)CloseHandle(stopEvent);
        if(timer)CloseHandle(timer);
    }
};
inline std::unique_ptr<State> state;
inline thread_local bool insideNative=false;
struct NativeScope{NativeScope(){insideNative=true;}~NativeScope(){insideNative=false;}};
inline HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* sc,UINT interval,UINT flags) {
    State* s=state.get(); if(!s)return E_UNEXPECTED;
    if(insideNative || sc!=s->chain.Get() || (flags&DXGI_PRESENT_TEST))return s->originalPresent(sc,interval,flags);
    NativeScope scope;size_t i=s->Before(interval,flags);HRESULT hr=s->originalPresent(sc,interval,flags);s->After(i,hr);return hr;
}
inline HRESULT STDMETHODCALLTYPE OnPresent1(IDXGISwapChain1* sc,UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS* p) {
    State* s=state.get();if(!s)return E_UNEXPECTED;
    if(insideNative || sc!=s->chain.Get() || (flags&DXGI_PRESENT_TEST))return s->originalPresent1(sc,interval,flags,p);
    NativeScope scope;size_t i=s->Before(interval,flags);HRESULT hr=s->originalPresent1(sc,interval,flags,p);s->After(i,hr);return hr;
}
inline void Finish() {if(state){state->Shutdown();state.reset();}}
class FactoryTap {
    using CreateFn=HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
    ComPtr<IDXGIFactory7> factory;
    void** old=nullptr;std::array<void*,32> table{};
    inline static FactoryTap* current=nullptr;
    CreateFn original=nullptr;
    ComPtr<ID3D12CommandQueue> applicationQueue;
    static HRESULT STDMETHODCALLTYPE Create(IDXGIFactory2* f,IUnknown* d,HWND w,const DXGI_SWAP_CHAIN_DESC1* sd,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd,IDXGIOutput* restrictOutput,IDXGISwapChain1** result) {
        FactoryTap* t=current;
        const HRESULT hr=t->original(f,d,w,sd,fd,restrictOutput,result);
        if(SUCCEEDED(hr) && result && *result && !state) {
            ComPtr<ID3D12CommandQueue> q;
            if(SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&q)))) {
                auto next=std::make_unique<State>();next->Initialize(*result,q.Get(),t->applicationQueue.Get());
                next->vtable[8]=reinterpret_cast<void*>(&OnPresent);next->vtable[22]=reinterpret_cast<void*>(&OnPresent1);
                state=std::move(next);
                *reinterpret_cast<void***>(state->chain.Get())=state->vtable.data();
            }
        }
        return hr;
    }
public:
    FactoryTap(IDXGIFactory* f,ID3D12CommandQueue* q):applicationQueue(q) {
        if(current)throw std::runtime_error("Only one scoped factory tap is allowed");
        if(FAILED(f->QueryInterface(IID_PPV_ARGS(&factory)))||factory.Get()!=f)
            throw std::runtime_error("Factory7 identity unsupported by isolated probe");
        old=*reinterpret_cast<void***>(f);std::copy_n(old,table.size(),table.begin());
        original=reinterpret_cast<CreateFn>(old[15]);table[15]=reinterpret_cast<void*>(&Create);
        current=this;*reinterpret_cast<void***>(f)=table.data();
    }
    ~FactoryTap(){if(factory && old)*reinterpret_cast<void***>(factory.Get())=old;current=nullptr;}
};
} // namespace
