#include <framegen/CopyCommonTexture.h>
#include <framegen/XeFGTransferPool.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <array>
#include <vector>
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <cstdint>
using Microsoft::WRL::ComPtr;
static void Check(bool ok,const char* s){if(!ok){std::cerr<<"FAIL "<<s<<'\n';std::exit(1);}}
static void Hr(HRESULT h,const char*s){if(FAILED(h)){std::cerr<<std::hex<<(UINT)h<<' ';Check(false,s);}}
static void Barrier(ID3D12GraphicsCommandList*l,ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition.pResource=r;
 v.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;v.Transition.StateBefore=a;v.Transition.StateAfter=b;l->ResourceBarrier(1,&v);
}
static std::uint32_t Value(UINT frame,UINT x,UINT y,UINT lane,UINT seed){
 return 0xD328019Fu^(frame*0x1020304u)^(x*0x31415927u)^(y*0x53739267u)^(lane*0x01025379u)^seed;
}
static void Run(ID3D12Device*d,UINT width,UINT height,DXGI_FORMAT format,UINT words,UINT frames,UINT seed){
 const UINT slots=3;
 auto queue=[&](D3D12_COMMAND_LIST_TYPE type){D3D12_COMMAND_QUEUE_DESC q{};q.Type=type;ComPtr<ID3D12CommandQueue>r;Hr(d->CreateCommandQueue(&q,IID_PPV_ARGS(&r)),"queue");return r;};
 auto render=queue(D3D12_COMMAND_LIST_TYPE_DIRECT),exporter=queue(D3D12_COMMAND_LIST_TYPE_COPY),importer=queue(D3D12_COMMAND_LIST_TYPE_COPY),consumer=queue(D3D12_COMMAND_LIST_TYPE_DIRECT);
 auto buffer=[&](D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,UINT64 bytes){
  D3D12_HEAP_PROPERTIES hp{};hp.Type=type;hp.CreationNodeMask=hp.VisibleNodeMask=1;
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource>r;Hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)),"buffer");return r;
 };
 D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=format;
 auto texture=[&](){D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;ComPtr<ID3D12Resource>r;Hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)),"texture");return r;};
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0,rowBytes=0;UINT rows=0;
 d->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&bytes);
 Check(rowBytes==static_cast<UINT64>(width)*words*4 && rows==height,"format byte width");
 const UINT64 stride=(bytes+511)&~UINT64(511);
 auto upload=buffer(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,stride*frames);
 auto readback=buffer(D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,stride*frames);
 void* map=nullptr;D3D12_RANGE noRead{0,0};Hr(upload->Map(0,&noRead,&map),"upload map");
 std::memset(map,0,static_cast<size_t>(stride*frames));
 for(UINT n=0;n<frames;++n)for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)for(UINT k=0;k<words;++k){
  const auto v=Value(n,x,y,k,seed);std::memcpy(static_cast<char*>(map)+stride*n+footprint.Footprint.RowPitch*y+(x*words+k)*4,&v,4);
 }
 upload->Unmap(0,nullptr);
 std::array<ComPtr<ID3D12Resource>,3> src,bridge,staging,proxy;
 for(UINT i=0;i<slots;++i){src[i]=texture();bridge[i]=buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,stride);staging[i]=texture();proxy[i]=texture();}
 ComPtr<ID3D12Fence> gate,observed;Hr(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"gate");Hr(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&observed)),"observed");
 Hr(consumer->Wait(gate.Get(),1),"hold consumer");
 MultiGPU::XeFGTransferPool producers,exports,imports,consumers;
 Check(imports.QueueWaitForSlot(consumer.Get(),0)==DXGI_ERROR_INVALID_CALL,"unsubmitted slot accepted");
 Check(imports.QueueWaitForSlot(nullptr,0)==E_POINTER,"null consumer accepted");
 for(UINT n=0;n<frames;++n){
  const auto slot=n%slots;
  Hr(consumers.WaitForSlot(slot),"prevent in-flight overwrite");Hr(imports.WaitForSlot(slot),"import retire");
  if(n>=slots)Hr(exports.QueueWaitForSlot(render.Get(),slot),"producer cannot overwrite COPY source");
  ID3D12GraphicsCommandList*l=nullptr;Hr(producers.Begin(render.Get(),slot,&l),"producer begin");
  Barrier(l,src[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=upload.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=footprint;a.PlacedFootprint.Offset=n*stride;
  b.pResource=src[slot].Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l->CopyTextureRegion(&b,0,0,0,&a,nullptr);
  Barrier(l,src[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);Hr(producers.Submit(slot),"producer submit");
  Hr(exports.Begin(exporter.Get(),slot,&l),"export begin");Hr(producers.QueueWaitForSlot(exporter.Get(),slot),"source fence");
  Check(MultiGPU::CopyCommonTextureToBuffer(nullptr,src[slot].Get(),bridge[slot].Get(),footprint)==E_POINTER,"null export accepted");
  auto bad=footprint;bad.Footprint.Width+=1;
  Check(MultiGPU::CopyCommonTextureToBuffer(l,src[slot].Get(),bridge[slot].Get(),bad)==E_INVALIDARG,"size mismatch accepted");
  Hr(MultiGPU::CopyCommonTextureToBuffer(l,src[slot].Get(),bridge[slot].Get(),footprint),"production export");Hr(exports.Submit(slot),"export submit");
  Hr(imports.Begin(importer.Get(),slot,&l),"import begin");Hr(exports.QueueWaitForSlot(importer.Get(),slot),"exact export fence");
  Hr(MultiGPU::CopyCommonBufferToTexture(l,bridge[slot].Get(),staging[slot].Get(),footprint),"production import");Hr(imports.Submit(slot),"import submit");
  if(n==1){
   Hr(imports.WaitForSlot(slot),"COPY progress while consumer held");Check(observed->GetCompletedValue()==0,"gate skipped");
   Check(FAILED(consumers.WaitForSlot(0,2)),"in-flight consumer slot incorrectly reusable");
   Hr(gate->Signal(1),"release gate");
  }
  Hr(consumers.Begin(consumer.Get(),slot,&l),"consumer begin");Hr(imports.QueueWaitForSlot(consumer.Get(),slot),"exact import fence");
  Hr(MultiGPU::CopyCommonTexture(l,staging[slot].Get(),proxy[slot].Get()),"production direct handoff");
  Barrier(l,proxy[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
  a={};b={};a.pResource=proxy[slot].Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=footprint;b.PlacedFootprint.Offset=n*stride;l->CopyTextureRegion(&b,0,0,0,&a,nullptr);
  Barrier(l,proxy[slot].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);Hr(consumers.Submit(slot),"consumer submit");
  if(n==0)Hr(consumer->Signal(observed.Get(),1),"gate observation");
 }
 Hr(producers.Drain(),"producer drain");Hr(exports.Drain(),"export drain");Hr(imports.Drain(),"import drain");Hr(consumers.Drain(),"consumer drain");
 Hr(readback->Map(0,nullptr,&map),"readback map");
 std::uint64_t checked=0;
 for(UINT n=0;n<frames;++n)for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)for(UINT k=0;k<words;++k){
  UINT32 v=0;std::memcpy(&v,static_cast<char*>(map)+stride*n+footprint.Footprint.RowPitch*y+(x*words+k)*4,4);
  Check(v==Value(n,x,y,k,seed),"stale/changed pixel, alpha, resource or frame ID");++checked;
 }
 Check(Value(1,0,0,0,seed)!=Value(2,0,0,0,seed),"pixel oracle cannot distinguish frames");
 readback->Unmap(0,&noRead);
 std::cout<<"PASS transfer "<<width<<'x'<<height<<" format="<<format<<" frames="<<frames<<" wordsChecked="<<checked<<" slots=3 queues=4\n";
}
int main(){
 ComPtr<ID3D12Debug>debug;Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"debug layer required");debug->EnableDebugLayer();
 ComPtr<IDXGIFactory4>factory;ComPtr<IDXGIAdapter>warp;ComPtr<ID3D12Device>d;
 Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP");Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)),"WARP device");
 ComPtr<ID3D12InfoQueue>messages;Hr(d.As(&messages),"debug info queue");messages->ClearStoredMessages();
 Run(d.Get(),67,13,DXGI_FORMAT_R10G10B10A2_UNORM,1,33,1);
 Run(d.Get(),3840,2160,DXGI_FORMAT_R10G10B10A2_UNORM,1,9,2);
 Run(d.Get(),2953,1661,DXGI_FORMAT_R32_FLOAT,1,9,3);
 Run(d.Get(),2953,1661,DXGI_FORMAT_R16G16_FLOAT,1,9,4);
 Run(d.Get(),3840,2160,DXGI_FORMAT_R16G16B16A16_FLOAT,2,9,5);
 Run(d.Get(),1919,1079,DXGI_FORMAT_R8G8B8A8_UNORM,1,15,6);
 for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){
  SIZE_T size=0;Hr(messages->GetMessage(i,nullptr,&size),"debug message size");std::vector<char>data(size);
  auto m=reinterpret_cast<D3D12_MESSAGE*>(data.data());Hr(messages->GetMessage(i,m,&size),"debug message");
  if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<m->pDescription<<'\n';Check(false,"D3D12 debug error");}
 }
 std::cout<<"PASS no D3D12 ERROR/CORRUPTION. WARP single-adapter multi-queue transfer tests ONLY; NOT XeFG image quality, AMD PCIe transfer, display scanout or hardware FPS.\n";
 return 0;
}
