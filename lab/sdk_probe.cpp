#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell_d3d12.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <optional>
using Microsoft::WRL::ComPtr;
static std::string Esc(const std::wstring& s){
 int n=WideCharToMultiByte(CP_UTF8,0,s.c_str(),-1,nullptr,0,nullptr,nullptr);
 std::string out(static_cast<size_t>(n),'\0');WideCharToMultiByte(CP_UTF8,0,s.c_str(),-1,out.data(),n,nullptr,nullptr);out.resize(n-1);
 std::string e;for(unsigned char c:out){if(c=='"'||c=='\\')e+='\\';if(c>=32)e+=static_cast<char>(c);}return e;
}
template<class F>static F Fn(HMODULE m,const char*n){return reinterpret_cast<F>(GetProcAddress(m,n));}
int wmain(int argc,wchar_t**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 std::cout<<"{\"capability_only\":true,\"physical_fps_tested\":false,\"pixel_flicker_tested\":false,\"adapters\":[";
 ComPtr<IDXGIFactory4>factory;
 if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))){std::cout<<"],\"error\":\"DXGI factory\"}\n";return 1;}
 bool first=true,primary=false,secondary=false;
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;if(!a)break;
  DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);std::wstring name=d.Description;
  if(!first)std::cout<<',';first=false;
  const bool software=(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)!=0;
  primary|=!software&&d.VendorId==0x1002&&name.find(L"9070 XT")!=std::wstring::npos;
  secondary|=!software&&d.VendorId==0x1002&&name.find(L"6600 XT")!=std::wstring::npos;
  std::cout<<"{\"name\":\""<<Esc(name)<<"\",\"vendor_id\":"<<d.VendorId<<",\"device_id\":"<<d.DeviceId<<",\"software\":"<<(software?"true":"false")<<'}';
 }
 std::cout<<"],\"target_pair_available\":"<<(primary&&secondary?"true":"false");
 ComPtr<IDXGIAdapter>warp;ComPtr<ID3D12Device>device;
 auto hr=factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));if(SUCCEEDED(hr))hr=D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device));
 std::cout<<",\"warp_device_hresult\":"<<static_cast<long>(hr);
 if(FAILED(hr)||argc!=2){std::cout<<",\"sdk_test\":\"not_run\"}\n";return 0;}
 const auto base=std::filesystem::absolute(argv[1]);
 const auto fgPath=base/L"libxess_fg.dll",llPath=base/L"libxell.dll";
 HMODULE fg=LoadLibraryExW(fgPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
 HMODULE ll=LoadLibraryExW(llPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
 if(!fg||!ll){std::cout<<",\"sdk_test\":\"library_unavailable\",\"win32_error\":"<<GetLastError()<<"}\n";return 0;}
 const auto create=Fn<decltype(&xefgSwapChainD3D12CreateContext)>(fg,"xefgSwapChainD3D12CreateContext");
 const auto destroy=Fn<decltype(&xefgSwapChainDestroy)>(fg,"xefgSwapChainDestroy");
 const auto init=Fn<decltype(&xefgSwapChainD3D12InitFromSwapChainDesc)>(fg,"xefgSwapChainD3D12InitFromSwapChainDesc");
 const auto bind=Fn<decltype(&xefgSwapChainSetLatencyReduction)>(fg,"xefgSwapChainSetLatencyReduction");
 const auto createLL=Fn<decltype(&xellD3D12CreateContext)>(ll,"xellD3D12CreateContext");
 const auto destroyLL=Fn<decltype(&xellDestroyContext)>(ll,"xellDestroyContext");
 const auto mode=Fn<decltype(&xellSetSleepMode)>(ll,"xellSetSleepMode");
 if(!create||!destroy||!init||!bind||!createLL||!destroyLL||!mode){std::cout<<",\"sdk_test\":\"missing_exports\"}\n";return 0;}
 xefg_swapchain_handle_t ctx=nullptr;auto cr=create(device.Get(),&ctx);
 std::cout<<",\"warp_xefg_create_result\":"<<static_cast<int>(cr);
 xell_context_handle_t llctx=nullptr;HWND window=nullptr;
 if(cr==XEFG_SWAPCHAIN_RESULT_SUCCESS&&ctx){
  auto lr=createLL(device.Get(),&llctx);std::cout<<",\"warp_xell_create_result\":"<<static_cast<int>(lr);
  if(lr==XELL_RESULT_SUCCESS&&llctx){
   xell_sleep_params_t sp{};sp.bLowLatencyMode=true;
   auto sr=mode(llctx,&sp);auto br=bind(ctx,llctx);
   std::cout<<",\"xell_mode_result\":"<<static_cast<int>(sr)<<",\"bind_result\":"<<static_cast<int>(br);
   WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"XeFGVirtualLabProbe";RegisterClassW(&wc);
   window=CreateWindowW(wc.lpszClassName,L"Capability probe only",WS_OVERLAPPEDWINDOW,0,0,1280,720,nullptr,nullptr,wc.hInstance,nullptr);
   if(window){
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ComPtr<ID3D12CommandQueue>queue;hr=device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue));
    if(SUCCEEDED(hr)){
     DXGI_SWAP_CHAIN_DESC1 d{};d.Width=1280;d.Height=720;d.Format=DXGI_FORMAT_R10G10B10A2_UNORM;d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=3;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
     xefg_swapchain_d3d12_init_params_t p{};p.maxInterpolatedFrames=1;p.uiMode=XEFG_SWAPCHAIN_UI_MODE_NONE;
     auto r=init(ctx,window,&d,nullptr,queue.Get(),factory.Get(),&p);
     std::cout<<",\"warp_xefg_initialization_result\":"<<static_cast<int>(r);
    }
   }
  }
  const auto dr=destroy(ctx);std::cout<<",\"xefg_destroy_result\":"<<static_cast<int>(dr);
 }
 if(llctx)destroyLL(llctx);if(window)DestroyWindow(window);
 std::cout<<",\"sdk_test\":\"initialization_probe_only\",\"final_release_allowed\":false}\n";
 // Modules remain loaded until process teardown. No speculative GPU queue reuse.
 return 0;
}
