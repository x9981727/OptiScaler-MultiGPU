#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <presentation.h>
#include <wrl/client.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;
int main(){
 ComPtr<IDXGIFactory4> factory;auto hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));if(FAILED(hr))return 1;
 for(UINT i=0;i<8;++i){
  ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
  DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);if(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
  std::printf("ADAPTER index=%u vendor=%04X device=%04X luid=%08X%08X\n",i,d.VendorId,d.DeviceId,unsigned(d.AdapterLuid.HighPart),d.AdapterLuid.LowPart);
  ComPtr<ID3D11Device> dev11;ComPtr<ID3D11DeviceContext> context;
  hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,nullptr,0,D3D11_SDK_VERSION,&dev11,nullptr,&context);
  std::printf("D3D11 hr=0x%08X\n",unsigned(hr));if(FAILED(hr))continue;
  ComPtr<IPresentationFactory> pf;hr=CreatePresentationFactory(dev11.Get(),IID_PPV_ARGS(&pf));
  std::printf("PRESENTATION_FACTORY hr=0x%08X\n",unsigned(hr));if(FAILED(hr))continue;
  std::printf("CAPS presentation=%d independentFlip=%d\n",int(pf->IsPresentationSupported()),int(pf->IsPresentationSupportedWithIndependentFlip()));
  if(!pf->IsPresentationSupported())continue;
  ComPtr<IPresentationManager> manager;hr=pf->CreatePresentationManager(&manager);std::printf("MANAGER hr=0x%08X\n",unsigned(hr));if(FAILED(hr))continue;
  ComPtr<ID3D12Device> dev12;hr=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&dev12));std::printf("D3D12 hr=0x%08X\n",unsigned(hr));if(FAILED(hr))continue;
  for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT}){
   D3D11_TEXTURE2D_DESC td{};td.Width=64;td.Height=64;td.MipLevels=1;td.ArraySize=1;td.Format=format;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;
   td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
   td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE;
   ComPtr<ID3D11Texture2D> texture;hr=dev11->CreateTexture2D(&td,nullptr,&texture);std::printf("DISPLAYABLE_TEXTURE format=%u hr=0x%08X\n",unsigned(format),unsigned(hr));if(FAILED(hr))continue;
   ComPtr<IPresentationBuffer> pb;hr=manager->AddBufferFromResource(texture.Get(),&pb);std::printf("ADD_BUFFER hr=0x%08X\n",unsigned(hr));if(FAILED(hr))continue;
   HANDLE event=nullptr;hr=pb->GetAvailableEvent(&event);std::printf("AVAILABLE_EVENT hr=0x%08X state=%lu\n",unsigned(hr),event?WaitForSingleObject(event,0):WAIT_FAILED);if(event)CloseHandle(event);
   ComPtr<IDXGIResource1> resource;hr=texture.As(&resource);if(FAILED(hr))continue;
   HANDLE shared=nullptr;hr=resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&shared);
   std::printf("SHARE_TEXTURE hr=0x%08X\n",unsigned(hr));if(FAILED(hr))continue;
   ComPtr<ID3D12Resource> opened;hr=dev12->OpenSharedHandle(shared,IID_PPV_ARGS(&opened));CloseHandle(shared);
   std::printf("OPEN_D3D12 hr=0x%08X\n",unsigned(hr));
   if(SUCCEEDED(hr)){auto rd=opened->GetDesc();D3D12_HEAP_PROPERTIES hp{};D3D12_HEAP_FLAGS flags{};opened->GetHeapProperties(&hp,&flags);std::printf("SHARED_DESC format=%u flags=%u layout=%u heapFlags=%u\n",unsigned(rd.Format),unsigned(rd.Flags),unsigned(rd.Layout),unsigned(flags));}
  }
 }
 std::puts("PROBE_SCOPE capability_and_resource_creation_only_no_presentation_or_performance_validation");return 0;
}
