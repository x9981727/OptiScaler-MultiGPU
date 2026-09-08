// Isolated research harness: GPU-queue presentation gate, never a game release.
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <detours.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
namespace {
using CreateFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using Present1Fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
CreateFn realCreate = nullptr;
PresentFn realPresent = nullptr;
Present1Fn realPresent1 = nullptr;
thread_local bool nested = false;
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<UINT64> pending;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> ready, release;
    ComPtr<ID3D12Device> device;
    IUnknown* nativeIdentity = nullptr;
    ID3D12CommandQueue* computeIdentity = nullptr;
    HWND target = nullptr;
    std::thread worker;
    HANDLE readyEvent = nullptr, timer = nullptr;
    std::atomic<bool> armed{false}, stop{false}, failed{false};
    std::atomic<UINT64> issued{0}, retired{0}, gated{0}, maxDepth{0}, nativeCalls{0};
    bool active = false;
    double intervalUs = 7462.686567;
    std::atomic<HRESULT> failure{S_OK};
    std::mutex signalMutex;
    UINT64 lastReleased = 0;
    int priority = 0;
    double waitMs = 0, elapsedMs = 0;
    ~Gate() { if(worker.joinable()) Finish(); }
    static bool Yes(const char* n) { char b[16]{}; return GetEnvironmentVariableA(n,b,16)>0 && b[0]=='1'; }
    bool Matches(IUnknown* p) {
        if (!nativeIdentity) return false;
        IUnknown* identity = nullptr;
        if (FAILED(p->QueryInterface(IID_PPV_ARGS(&identity)))) return false;
        const bool yes = identity == nativeIdentity;
        identity->Release(); return yes;
    }
    HRESULT ReleaseTo(UINT64 id) {
        std::lock_guard l(signalMutex);
        if (!release || id <= lastReleased) return S_OK;
        const auto hr=release->Signal(id); if(SUCCEEDED(hr))lastReleased=id; return hr;
    }
    void Fail(HRESULT hr) {
        failure = hr; failed.store(true); cv.notify_all();
        if (release) ReleaseTo(issued.load());
    }
    void Worker() {
        auto deadline = Clock::time_point{};
        auto beginning = Clock::now();
        for (;;) {
            UINT64 id;
            { std::unique_lock l(mutex); cv.wait(l,[&]{return stop || !pending.empty();});
              if (pending.empty()) { if(stop) break; continue; }
              id=pending.front(); pending.pop_front(); }
            if (!failed) {
                const auto readyStart=Clock::now();
                const auto hr=ready->SetEventOnCompletion(id,readyEvent);
                if (FAILED(hr)) Fail(hr);
                while(!failed && ready->GetCompletedValue()<id) {
                    const auto result=WaitForSingleObject(readyEvent,20);
                    if (result==WAIT_FAILED) { Fail(HRESULT_FROM_WIN32(GetLastError())); break; }
                    if (Clock::now()-readyStart > std::chrono::seconds(3)) { Fail(DXGI_ERROR_DEVICE_HUNG); break; }
                }
                if (ready->GetCompletedValue()==UINT64_MAX) Fail(DXGI_ERROR_DEVICE_REMOVED);
                auto now=Clock::now();
                if (deadline.time_since_epoch().count()==0) deadline=now;
                else deadline=std::max(deadline+std::chrono::nanoseconds(static_cast<long long>(intervalUs*1000)),now);
                if (!failed && deadline>now) {
                    const auto start=now;
                    auto left=deadline-now;
                    if(left>std::chrono::microseconds(200)) {
                        LARGE_INTEGER due{};
                        due.QuadPart=-std::chrono::duration_cast<std::chrono::nanoseconds>(left-std::chrono::microseconds(150)).count()/100;
                        if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)) WaitForSingleObject(timer,100);
                    }
                    while(Clock::now()<deadline) YieldProcessor();
                    waitMs+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
                }
            }
            auto hr=ReleaseTo(id);
            if(FAILED(hr)) Fail(hr);
            ++retired; cv.notify_all();
        }
        elapsedMs=std::chrono::duration<double,std::milli>(Clock::now()-beginning).count();
    }
    HRESULT Setup(IDXGISwapChain1* sc, IUnknown* q) {
        ComPtr<ID3D12CommandQueue> candidate;
        if(FAILED(q->QueryInterface(IID_PPV_ARGS(&candidate)))) return E_NOINTERFACE;
        priority=candidate->GetDesc().Priority;
        if(candidate.Get()==computeIdentity) return E_INVALIDARG;
        if(queue) return E_UNEXPECTED;
        queue=candidate;
        HRESULT hr=queue->GetDevice(IID_PPV_ARGS(&device)); if(FAILED(hr)) return hr;
        hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ready)); if(FAILED(hr)) return hr;
        hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&release)); if(FAILED(hr)) return hr;
        readyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        if(!timer) timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        if(!readyEvent || !timer) return HRESULT_FROM_WIN32(GetLastError());
        IUnknown* identity=nullptr;
        hr=sc->QueryInterface(IID_PPV_ARGS(&identity)); if(FAILED(hr)) return hr;
        nativeIdentity=identity; identity->Release();
        active=Yes("XEFG_NATIVE_GATE");
        char val[64]{};
        if(GetEnvironmentVariableA("XEFG_GATE_INTERVAL_US",val,64)) {
            double n=std::atof(val); if(n>=1000 && n<=50000) intervalUs=n;
        }
        if(active) worker=std::thread([this]{Worker();});
        std::fprintf(stderr,"NATIVE_GATE_SETUP active=%d priority=%d distinctQueue=1 intervalUs=%.6f\n",active,priority,intervalUs);std::fflush(stderr);
        return S_OK;
    }
    HRESULT Schedule() {
        ++nativeCalls;
        if(!active || failed) return failed?failure.load():S_OK;
        std::unique_lock l(mutex);
        cv.wait(l,[&]{return issued-retired<12 || failed || stop;});
        if(failed || stop) return failed?failure.load():E_ABORT;
        const UINT64 id=++issued;
        HRESULT hr=queue->Signal(ready.Get(),id);
        if(SUCCEEDED(hr)) hr=queue->Wait(release.Get(),id);
        if(FAILED(hr)) { Fail(hr); return hr; }
        pending.push_back(id);++gated;
        auto depth=issued-retired;maxDepth.store(std::max(maxDepth.load(),depth));
        l.unlock();cv.notify_one();return S_OK;
    }
    void Finish() {
        {std::lock_guard l(mutex);stop=true;}cv.notify_all();
        if(worker.joinable()) worker.join();
        std::fprintf(stderr,"NATIVE_GATE_SUMMARY calls=%llu issued=%llu retired=%llu gated=%llu maxPending=%llu intervalUs=%.6f waitMs=%.3f elapsedMs=%.3f failed=%d hr=0x%08X\n",nativeCalls.load(),issued.load(),retired.load(),gated.load(),maxDepth.load(),intervalUs,waitMs,elapsedMs,int(failed.load()),unsigned(failure.load()));std::fflush(stderr);
        if(readyEvent) CloseHandle(readyEvent);
        if(timer)CloseHandle(timer);
        readyEvent=timer=nullptr;
    }
} gate;
HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc,UINT interval,UINT flags) {
    if(nested || !gate.Matches(sc) || (flags&DXGI_PRESENT_TEST) || interval!=0) return realPresent(sc,interval,flags);
    const auto hr=gate.Schedule();if(FAILED(hr))return hr;
    nested=true;const auto result=realPresent(sc,interval,flags);nested=false;return result;
}
HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc,UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS* p) {
    if(nested || !gate.Matches(sc) || (flags&DXGI_PRESENT_TEST) || interval!=0) return realPresent1(sc,interval,flags,p);
    const auto hr=gate.Schedule();if(FAILED(hr))return hr;
    nested=true;const auto result=realPresent1(sc,interval,flags,p);nested=false;return result;
}
HRESULT STDMETHODCALLTYPE HookCreate(IDXGIFactory2* f,IUnknown* q,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1* d,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,IDXGIOutput* o,IDXGISwapChain1** sc) {
    auto hr=realCreate(f,q,hwnd,d,fs,o,sc);
    if(SUCCEEDED(hr) && gate.armed && hwnd==gate.target && sc && *sc) {
        hr=gate.Setup(*sc,q);
        if(FAILED(hr)) {gate.Fail(hr);return hr;}
        auto v=*reinterpret_cast<void***>(*sc);
        realPresent=reinterpret_cast<PresentFn>(v[8]);realPresent1=reinterpret_cast<Present1Fn>(v[22]);
        LONG e=DetourTransactionBegin();
        if(e==NO_ERROR)e=DetourUpdateThread(GetCurrentThread());
        if(e==NO_ERROR)e=DetourAttach(reinterpret_cast<PVOID*>(&realPresent),HookPresent);
        if(e==NO_ERROR)e=DetourAttach(reinterpret_cast<PVOID*>(&realPresent1),HookPresent1);
        if(e==NO_ERROR)e=DetourTransactionCommit();else DetourTransactionAbort();
        if(e!=NO_ERROR){hr=HRESULT_FROM_WIN32(e);gate.Fail(hr);}
    }
    return hr;
}
}
extern "C" void InstallNativeGate(IDXGIFactory2* f,HWND hwnd,ID3D12CommandQueue* source) {
    gate.target=hwnd;gate.computeIdentity=source;
    auto v=*reinterpret_cast<void***>(f);realCreate=reinterpret_cast<CreateFn>(v[15]);
    LONG e=DetourTransactionBegin();if(e==NO_ERROR)e=DetourUpdateThread(GetCurrentThread());
    if(e==NO_ERROR)e=DetourAttach(reinterpret_cast<PVOID*>(&realCreate),HookCreate);
    if(e==NO_ERROR)e=DetourTransactionCommit();else DetourTransactionAbort();
    if(e!=NO_ERROR) gate.Fail(HRESULT_FROM_WIN32(e));else gate.armed=true;
}
extern "C" void FinishNativeGate() {gate.Finish();}
extern "C" bool FastReferenceMode() {return Gate::Yes("XEFG_FAST_REFERENCE");}
