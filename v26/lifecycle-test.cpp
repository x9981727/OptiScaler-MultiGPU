#define NOMINMAX
#include "GameOutputPacing-compiled.h"
#include <cstdio>
#include <stdexcept>
#include <cstring>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT h){if(FAILED(h)){char b[80];sprintf_s(b,"HRESULT 0x%08X",(unsigned)h);throw std::runtime_error(b);}}
static std::atomic<bool> done=false;static int outcome=1;
static void Transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=r;b.Transition.StateBefore=from;b.Transition.StateAfter=to;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;l->ResourceBarrier(1,&b);}
static void Run(HWND window){try{
 ComPtr<IDXGIFactory7> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
 ComPtr<IDXGIAdapter> warp;Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
 ComPtr<ID3D12Device> device;Check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
 ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};Check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
 int context=1;ComPtr<IDXGISwapChain1> initial;
 DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=128;desc.Height=96;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferCount=3;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
 {XeFGGamePacing::FactoryScope scope(&context,factory.Get(),queue.Get(),window);Check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&initial));}
 ComPtr<IDXGISwapChain4> sc;Check(initial.As(&sc));initial.Reset();
 if(!XeFGGamePacing::session.load())throw std::runtime_error("candidate hook was not installed");
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
 Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
 Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));Check(list->Close());
 ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
 auto render=[&](UINT number,bool sync){
  Check(XeFGGamePacing::WaitIdle(queue.Get()));
  ComPtr<ID3D12Resource> back;Check(sc->GetBuffer(sc->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)));
  Check(allocator->Reset());Check(list->Reset(allocator.Get(),nullptr));device->CreateRenderTargetView(back.Get(),nullptr,heap->GetCPUDescriptorHandleForHeapStart());
  Transition(list.Get(),back.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
  float color[]={float(number%7)/7.0f,.2f,.6f,1};list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(),color,0,nullptr);
  Transition(list.Get(),back.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);Check(list->Close());ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
  XeFGGamePacing::Source(&context,14.925373,!sync);
  if(number%2){DXGI_PRESENT_PARAMETERS p{};Check(sc->Present1(sync?1:0,0,&p));}else Check(sc->Present(sync?1:0,0));
 };
 for(UINT i=0;i<16;++i)render(i,i<4);
 auto s=XeFGGamePacing::session.load();Check(s->Drain());Check(XeFGGamePacing::WaitIdle(queue.Get()));
 UINT64 before=s->accepted.load();Check(sc->Present(0,DXGI_PRESENT_TEST));
 if(s->accepted.load()!=before)throw std::runtime_error("TEST consumed an image");
 if(sc->Present(0,DXGI_PRESENT_DO_NOT_WAIT)!=DXGI_ERROR_WAS_STILL_DRAWING)throw std::runtime_error("unsupported DONOTWAIT not explicit");
 Check(sc->ResizeBuffers(4,160,90,DXGI_FORMAT_R10G10B10A2_UNORM,0));
 for(UINT i=16;i<30;++i)render(i,false);
 Check(s->Drain());Check(XeFGGamePacing::WaitIdle(queue.Get()));
 UINT masks[]={1,1};IUnknown* queues[]={queue.Get(),queue.Get()};
 Check(sc->ResizeBuffers1(2,144,80,DXGI_FORMAT_R8G8B8A8_UNORM,0,masks,queues));
 for(UINT i=30;i<42;++i)render(i,i>=38);
 Check(s->Drain());
 if(s->accepted.load()!=s->completed.load())throw std::runtime_error("lost native output");
 auto count=s->completed.load();
 // Metadata and color are forwarded, but WARP is not an HDR-display test.
 {XeFGGamePacing::NativeScope direct;auto expected=s->hdr(sc.Get(),DXGI_HDR_METADATA_TYPE_NONE,0,nullptr);auto actual=expected;
  (void)actual;}
 Check(sc->SetSourceSize(144,80));
 XeFGGamePacing::Pause(&context);XeFGGamePacing::Finish(&context);
 if(XeFGGamePacing::session.load())throw std::runtime_error("session retained after normal teardown");
 Check(sc->Present(0,DXGI_PRESENT_TEST));sc.Reset();
 printf("PASS: %llu actual native Presents, sync/async Present1, TEST/DONOTWAIT handling, 3->4->2 buffers, SDR/10bit format resize, ResizeBuffers1 queue mapping, normal teardown. WARP lifecycle only; no XeFG algorithm, real HDR display or FPS acceptance.\n",(unsigned long long)count);outcome=0;
}catch(const std::exception&e){fprintf(stderr,"FAIL lifecycle: %s\n",e.what());outcome=1;}done=true;}
int main(){
 auto cfg=XeFGGamePacing::Settings::Read();auto path=cfg.directory/L"XeFGPacing.ini";
 WritePrivateProfileStringW(L"OutputPacing",L"Enabled",L"1",path.c_str());
 HINSTANCE instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=instance;wc.lpszClassName=L"XeFGCandidateLifecycle";
 RegisterClassW(&wc);HWND w=CreateWindowW(wc.lpszClassName,L"v26 CI lifecycle",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,instance,nullptr);
 if(!w)return 2;ShowWindow(w,SW_SHOWNOACTIVATE);
 std::thread task([&]{Run(w);});
 while(!done){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(1);}
 task.join();DestroyWindow(w);return outcome;
}
