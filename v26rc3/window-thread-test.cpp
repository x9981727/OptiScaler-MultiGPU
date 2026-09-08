#define NOMINMAX
#ifdef TEST_RC2_NEGATIVE
#include "GameOutputPacing-before-rc3.h"
#else
#include "GameOutputPacing-compiled.h"
#endif
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static constexpr UINT SentProbe=WM_APP+71,PostedProbe=WM_APP+72;
static HWND window=nullptr;static HANDLE callbackEntered=nullptr;
static std::atomic<unsigned> sentCount{0},postedCount{0},timeouts{0};
static std::atomic<bool> needMessage{false};
static XeFGGamePacing::Session::PresentFn nativePresent=nullptr;
static XeFGGamePacing::Session::Present1Fn nativePresent1=nullptr;
static DWORD lastNativeThread=0;
static void Check(HRESULT hr){if(FAILED(hr)){char s[96];sprintf_s(s,"HRESULT %08x",(unsigned)hr);throw std::runtime_error(s);}}
static void Need(bool result,const char* text){if(!result)throw std::runtime_error(text);}
static LRESULT CALLBACK WindowProcedure(HWND h,UINT msg,WPARAM w,LPARAM l){
    if(msg==SentProbe){++sentCount;return 73;}
    if(msg==PostedProbe){++postedCount;return 0;}
    return DefWindowProcW(h,msg,w,l);
}
static HRESULT SendProbe(){
    lastNativeThread=GetCurrentThreadId();
    if(!needMessage.exchange(false))return S_OK;
    SetEvent(callbackEntered);
    DWORD_PTR result=0;SetLastError(0);
    if(!SendMessageTimeoutW(window,SentProbe,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,650,&result)){
        ++timeouts;return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    return result==73?S_OK:E_FAIL;
}
static HRESULT STDMETHODCALLTYPE CheckedPresent(IDXGISwapChain* sc,UINT sync,UINT flags){
    auto hr=SendProbe();return FAILED(hr)?hr:nativePresent(sc,sync,flags);
}
static HRESULT STDMETHODCALLTYPE CheckedPresent1(IDXGISwapChain1* sc,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS*p){
    auto hr=SendProbe();return FAILED(hr)?hr:nativePresent1(sc,sync,flags,p);
}
int main(){
    int result=1;int owner=1;ComPtr<IDXGISwapChain4> sc;
    auto cfg=XeFGGamePacing::Settings::Read();
    auto config=cfg.directory/L"XeFGPacing.ini";
    WritePrivateProfileStringW(L"OutputPacing",L"Enabled",L"1",config.c_str());
    WritePrivateProfileStringW(L"OutputPacing",L"Trace",L"1",config.c_str());
    HINSTANCE instance=GetModuleHandleW(nullptr);WNDCLASSW cls{};cls.hInstance=instance;cls.lpszClassName=L"XeFGWindowOwnerRegression";cls.lpfnWndProc=WindowProcedure;
    RegisterClassW(&cls);window=CreateWindowW(cls.lpszClassName,L"XeFG bounded window-owner regression",WS_OVERLAPPEDWINDOW,50,50,400,300,nullptr,nullptr,instance,nullptr);
    callbackEntered=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    try{
        Need(window&&callbackEntered,"window setup failed");ShowWindow(window,SW_SHOWNOACTIVATE);
        ComPtr<IDXGIFactory7> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp;Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device;Check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qdesc{};Check(device->CreateCommandQueue(&qdesc,IID_PPV_ARGS(&queue)));
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=128;desc.Height=96;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferCount=3;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> first;
        {XeFGGamePacing::FactoryScope install(&owner,factory.Get(),queue.Get(),window);Check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&first));}
        Check(first.As(&sc));first.Reset();auto session=XeFGGamePacing::session.load();Need(session!=nullptr,"hook absent");
        nativePresent=session->present;nativePresent1=session->present1;session->present=CheckedPresent;session->present1=CheckedPresent1;
        auto startAsync=[&]{
            XeFGGamePacing::Source(&owner,14.0,true);needMessage=true;
            Check(sc->Present(0,0));
            Need(WaitForSingleObject(callbackEntered,1500)==WAIT_OBJECT_0,"consumer did not enter native callback");
        };
#ifdef TEST_RC2_NEGATIVE
        startAsync();HRESULT blocked=session->Drain();
        Need(FAILED(blocked)&&timeouts.load()==1&&sentCount.load()==0,"expected RC2 missing-message-service failure not reproduced");
        printf("PASS negative control: unchanged RC2 fails HWND-owner Drain when native Present synchronously sends a window message (bounded 650ms injection, no game execution).\n");
#else
        // 1. HWND owner waiting for an async Present must service SENT messages,
        // while a POSTED application command remains queued and undispatched.
        PostMessageW(window,PostedProbe,0,0);startAsync();Check(session->Drain());
        Need(sentCount.load()==1&&postedCount.load()==0,"sent-only service contract violated");
        MSG posted{};Need(PeekMessageW(&posted,window,PostedProbe,PostedProbe,PM_REMOVE)!=0,"posted command removed by wait");
        // 2. Synchronous Present and Present1 retain their original caller thread.
        XeFGGamePacing::Source(&owner,14.0,false);needMessage=true;Check(sc->Present(0,0));
        Need(lastNativeThread==GetCurrentThreadId(),"sync Present moved to worker");
        needMessage=true;DXGI_PRESENT_PARAMETERS params{};Check(sc->Present1(1,0,&params));
        Need(lastNativeThread==GetCurrentThreadId(),"sync Present1 moved to worker");
        // 3. Resize on the HWND owner while a consumer awaits a window response.
        startAsync();Check(sc->ResizeBuffers(4,320,180,DXGI_FORMAT_R10G10B10A2_UNORM,0));
        // 4. Control and previously unhooked ResizeTarget use the same drain.
        startAsync();Check(sc->SetSourceSize(320,180));
        startAsync();DXGI_MODE_DESC mode{};mode.Width=400;mode.Height=300;mode.Format=DXGI_FORMAT_UNKNOWN;
        Check(sc->ResizeTarget(&mode));
        // 5. Contended submit mutex must not stop a HWND-owner message response.
        HANDLE locked=CreateEventW(nullptr,FALSE,FALSE,nullptr);HRESULT contenderResult=E_PENDING;
        unsigned before=sentCount.load();
        std::thread contender([&]{
            std::lock_guard<XeFGGamePacing::WindowSubmissionMutex> hold(session->submitMutex);
            SetEvent(locked);DWORD_PTR response=0;
            bool ok=SendMessageTimeoutW(window,SentProbe,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,650,&response)!=0;
            contenderResult=ok&&response==73?S_OK:E_FAIL;
        });
        WaitForSingleObject(locked,1000);auto control=session->Control([]{return S_OK;});contender.join();CloseHandle(locked);
        Check(control);Check(contenderResult);Need(sentCount.load()==before+1,"contended mutex starved owner");
        // 6. Actual maximize/restore and 0x0 client-size ResizeBuffers on owner.
        ShowWindow(window,SW_MAXIMIZE);Need(IsZoomed(window)!=FALSE,"test window did not maximize");
        Check(sc->ResizeBuffers(0,0,0,DXGI_FORMAT_UNKNOWN,0));
        RECT client{};GetClientRect(window,&client);DXGI_SWAP_CHAIN_DESC1 actual{};Check(sc->GetDesc1(&actual));
        Need(actual.Width==static_cast<UINT>(client.right)&&actual.Height==static_cast<UINT>(client.bottom),"maximized client dimensions not applied");
        ShowWindow(window,SW_RESTORE);Check(sc->ResizeBuffers(0,0,0,DXGI_FORMAT_UNKNOWN,0));
        ShowWindow(window,SW_MINIMIZE);XeFGGamePacing::Source(&owner,14.0,true);needMessage=true;Check(sc->Present(0,0));
        Need(lastNativeThread==GetCurrentThreadId(),"minimized Present not caller synchronous");ShowWindow(window,SW_RESTORE);
        Check(sc->SetFullscreenState(FALSE,nullptr));
        UINT masks[]={1,1};IUnknown* queues[]={queue.Get(),queue.Get()};
        startAsync();Check(sc->ResizeBuffers1(2,160,100,DXGI_FORMAT_R8G8B8A8_UNORM,0,masks,queues));
        // 7. Normal teardown drains a consumer requesting a window response.
        startAsync();XeFGGamePacing::Pause(&owner);Check(session->Drain());
        Need(session->accepted.load()==session->completed.load(),"output packet loss");
        Need(timeouts.load()==0,"window probe timed out");
        printf("PASS RC3: HWND-owner async drain, same-thread sync Present/Present1, pending-output resize and ResizeTarget, contended submission mutex, maximize/client-size rebuild, restore/minimize, ResizeBuffers1, sent-only dispatch preserving posted commands; %u sent probes, zero probe timeouts. WARP regression only; not game FPS/HDR validation.\n",sentCount.load());
#endif
        XeFGGamePacing::Finish(&owner);result=0;
    }catch(const std::exception& e){fprintf(stderr,"FAIL window-thread regression: %s\n",e.what());}
    sc.Reset();if(window)DestroyWindow(window);if(callbackEntered)CloseHandle(callbackEntered);return result;
}
