#pragma once
// Isolated research backend, not a game release. Uses actual supported OS
// target-time presentation rather than claiming CPU wake-up equals scanout.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <presentation.h>
#include <wrl/client.h>
#include <vector>
#include <array>
#include <fstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>
namespace CompositionOutputLab {
using Microsoft::WRL::ComPtr;
inline bool Enabled(){char b[8]{};return GetEnvironmentVariableA("XEFG_COMPOSITION_OUTPUT",b,8)>0&&b[0]=='1';}
inline void Check(HRESULT h){if(FAILED(h)){char b[80];std::snprintf(b,sizeof(b),"Composition HRESULT=0x%08X",unsigned(h));throw std::runtime_error(b);}}
inline UINT64 Now(){UINT64 v=0;QueryInterruptTimePrecise(&v);return v;}
struct Renderer {
 ComPtr<ID3D11Device> device11;
 ComPtr<ID3D11DeviceContext> context11;
 ComPtr<IPresentationManager> manager;
 ComPtr<IPresentationSurface> surface;
 ComPtr<IDCompositionDevice> compositor;
 ComPtr<IDCompositionTarget> target;
 ComPtr<IDCompositionVisual> visual;
 ComPtr<IUnknown> content;
 HANDLE surfaceHandle=nullptr,lostEvent=nullptr,statisticsEvent=nullptr;
 std::vector<ComPtr<ID3D11Texture2D>> textures11;
 std::vector<ComPtr<ID3D12Resource>> textures12;
 std::vector<ComPtr<IPresentationBuffer>> buffers;
 std::vector<HANDLE> available;
 std::vector<std::array<UINT64,5>> submitted,displayed,statuses;
 UINT64 targetTime=0;long double intervalTicks=74626.86567164179L;long double ideal=0;
 UINT64 lateTargets=0;bool requestDuration=false;
 void Initialize(ID3D12Device* d12,HWND hwnd,UINT width,UINT height,DXGI_FORMAT format,UINT count){
  ComPtr<IDXGIFactory4> f;Check(CreateDXGIFactory1(IID_PPV_ARGS(&f)));
  ComPtr<IDXGIAdapter1> a;Check(f->EnumAdapterByLuid(d12->GetAdapterLuid(),IID_PPV_ARGS(&a)));
  Check(D3D11CreateDevice(a.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,nullptr,0,D3D11_SDK_VERSION,&device11,nullptr,&context11));
  ComPtr<IPresentationFactory> pf;Check(CreatePresentationFactory(device11.Get(),IID_PPV_ARGS(&pf)));
  const bool supported=pf->IsPresentationSupported(),direct=pf->IsPresentationSupportedWithIndependentFlip();
  std::fprintf(stderr,"COMPOSITION_CAPS presentation=%d independentFlip=%d\n",int(supported),int(direct));std::fflush(stderr);
  if(!supported||!direct)throw std::runtime_error("Direct OS presentation unavailable; no silent fallback");
  Check(pf->CreatePresentationManager(&manager));
  Check(manager->GetLostEvent(&lostEvent));Check(manager->GetPresentStatisticsAvailableEvent(&statisticsEvent));
  for(auto k:{PresentStatisticsKind_PresentStatus,PresentStatisticsKind_CompositionFrame,PresentStatisticsKind_IndependentFlipFrame})Check(manager->EnablePresentStatisticsKind(k,true));
  Check(DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS,nullptr,&surfaceHandle));
  Check(manager->CreatePresentationSurface(surfaceHandle,&surface));surface->SetTag(0x584647);
  Check(surface->SetAlphaMode(DXGI_ALPHA_MODE_IGNORE));Check(surface->SetColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
  RECT rect{0,0,LONG(width),LONG(height)};Check(surface->SetSourceRect(&rect));
  ComPtr<IDXGIDevice> dxgi;Check(device11.As(&dxgi));Check(DCompositionCreateDevice(dxgi.Get(),IID_PPV_ARGS(&compositor)));
  Check(compositor->CreateTargetForHwnd(hwnd,TRUE,&target));Check(compositor->CreateVisual(&visual));
  Check(compositor->CreateSurfaceFromHandle(surfaceHandle,&content));Check(visual->SetContent(content.Get()));Check(target->SetRoot(visual.Get()));Check(compositor->Commit());
  for(UINT i=0;i<count;++i){
   D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=1;td.ArraySize=1;td.Format=format;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;
   td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE;
   ComPtr<ID3D11Texture2D> t;Check(device11->CreateTexture2D(&td,nullptr,&t));
   ComPtr<IPresentationBuffer> pb;Check(manager->AddBufferFromResource(t.Get(),&pb));HANDLE e=nullptr;Check(pb->GetAvailableEvent(&e));available.push_back(e);
   ComPtr<IDXGIResource1> shared;Check(t.As(&shared));HANDLE h=nullptr;Check(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&h));
   ComPtr<ID3D12Resource> r;auto hr=d12->OpenSharedHandle(h,IID_PPV_ARGS(&r));CloseHandle(h);Check(hr);
   buffers.push_back(pb);textures11.push_back(t);textures12.push_back(r);
  }
  char b[64]{};if(GetEnvironmentVariableA("XEFG_GATE_INTERVAL_US",b,64)){double x=std::atof(b);if(x>=1000&&x<=50000)intervalTicks=static_cast<long double>(x)*10;}
  requestDuration=GetEnvironmentVariableA("XEFG_COMPOSITION_DURATION",b,64)>0&&b[0]=='1';
  if(requestDuration)Check(manager->SetPreferredPresentDuration(SystemInterruptTime{static_cast<UINT64>(intervalTicks+.5L)},SystemInterruptTime{1000}));
  std::fprintf(stderr,"COMPOSITION_INIT width=%u height=%u format=%u buffers=%u requestDuration=%d intervalUs=%.6f\n",width,height,unsigned(format),count,int(requestDuration),double(intervalTicks/10));std::fflush(stderr);
 }
 void WaitAvailable(size_t index){
  if(index>=available.size())throw std::runtime_error("Invalid presentation buffer");
  HANDLE events[]={available[index],lostEvent};const auto result=WaitForMultipleObjects(2,events,FALSE,3000);
  if(result!=WAIT_OBJECT_0)throw std::runtime_error(result==WAIT_OBJECT_0+1?"Presentation manager lost":"Buffer retirement timeout");
 }
 void ReadStatistics(){
  for(UINT i=0;i<4096&&WaitForSingleObject(statisticsEvent,0)==WAIT_OBJECT_0;++i){
   ComPtr<IPresentStatistics> st;Check(manager->GetNextPresentStatistics(&st));if(!st)break;
   auto id=st->GetPresentId();const auto kind=st->GetKind();
   if(kind==PresentStatisticsKind_IndependentFlipFrame){
    ComPtr<IIndependentFlipFramePresentStatistics> item;Check(st.As(&item));
    if(displayed.size()<100000)displayed.push_back({id,item->GetDisplayedTime().value,item->GetPresentDuration().value,static_cast<UINT64>(kind),0});
   }else if(kind==PresentStatisticsKind_PresentStatus){
    ComPtr<IPresentStatusPresentStatistics> item;Check(st.As(&item));
    if(statuses.size()<100000)statuses.push_back({id,static_cast<UINT64>(item->GetPresentStatus()),item->GetCompositionFrameId(),Now(),0});
   }else if(kind==PresentStatisticsKind_CompositionFrame){
    ComPtr<ICompositionFramePresentStatistics> item;Check(st.As(&item));
    // Store frame identity; composition timing requires separate OS target-stat
    // correlation. Never fabricate a display time when iflip is not used.
    if(displayed.size()<100000)displayed.push_back({id,0,0,static_cast<UINT64>(kind),item->GetCompositionFrameId()});
   }
  }
 }
 void Present(size_t slot,UINT64 sourceOutput){
  ReadStatistics();const auto now=Now();
  if(!targetTime)ideal=static_cast<long double>(now)+intervalTicks*2;
  else ideal+=intervalTicks;
  if(ideal<static_cast<long double>(now)+5000){ideal=static_cast<long double>(now)+5000;++lateTargets;}
  targetTime=static_cast<UINT64>(ideal+.5L);
  Check(surface->SetBuffer(buffers.at(slot).Get()));Check(manager->SetTargetTime(SystemInterruptTime{targetTime}));
  const auto id=manager->GetNextPresentId();Check(manager->Present());
  if(submitted.size()<100000)submitted.push_back({id,sourceOutput,targetTime,now,static_cast<UINT64>(slot)});
 }
 void Shutdown(){
  if(manager){for(int i=0;i<10;++i){ReadStatistics();Sleep(5);}}
  char path[32768]{};DWORD n=GetEnvironmentVariableA("XEFG_LAB_TRACE_DIR",path,sizeof(path));
  if(n&&n<sizeof(path)){
   const std::string prefix=std::string(path)+"/";
   {std::ofstream f(prefix+"composition-submitted.csv");f<<"present_id,native_output_id,target_100ns,issued_100ns,slot\n";for(auto&r:submitted)f<<r[0]<<','<<r[1]<<','<<r[2]<<','<<r[3]<<','<<r[4]<<'\n';}
   {std::ofstream f(prefix+"composition-displayed.csv");f<<"present_id,displayed_100ns,approved_duration_100ns,kind,composition_frame_id\n";for(auto&r:displayed)f<<r[0]<<','<<r[1]<<','<<r[2]<<','<<r[3]<<','<<r[4]<<'\n';}
   {std::ofstream f(prefix+"composition-status.csv");f<<"present_id,status,composition_frame_id,observed_100ns,reserved\n";for(auto&r:statuses)f<<r[0]<<','<<r[1]<<','<<r[2]<<','<<r[3]<<','<<r[4]<<'\n';}
  }
  if(target)target->SetRoot(nullptr);if(compositor)compositor->Commit();
  std::fprintf(stderr,"COMPOSITION_SUMMARY submitted=%zu displayStatistics=%zu statuses=%zu lateTargets=%llu\n",submitted.size(),displayed.size(),statuses.size(),lateTargets);std::fflush(stderr);
 }
 ~Renderer(){for(auto h:available)if(h)CloseHandle(h);if(lostEvent)CloseHandle(lostEvent);if(statisticsEvent)CloseHandle(statisticsEvent);if(surfaceHandle)CloseHandle(surfaceHandle);}
};
}
