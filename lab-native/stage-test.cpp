#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "NativeFrameStage.h"
using Microsoft::WRL::ComPtr;
static void Hr(HRESULT hr){if(FAILED(hr)){char b[80];sprintf_s(b,"HRESULT %08x",(unsigned)hr);throw std::runtime_error(b);}}
static void Barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition.pResource=r;
 x.Transition.StateBefore=a;x.Transition.StateAfter=b;x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;l->ResourceBarrier(1,&x);
}
static ComPtr<ID3D12Resource> Buffer(ID3D12Device* d,UINT64 size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){
 D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CreationNodeMask=h.VisibleNodeMask=1;
 D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;r.Width=size;r.Height=1;
 r.DepthOrArraySize=r.MipLevels=1;r.SampleDesc.Count=1;r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 ComPtr<ID3D12Resource> p;Hr(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&r,state,nullptr,IID_PPV_ARGS(&p)));return p;
}
static UINT64 Test(ID3D12Device* d,UINT w,UINT h){
 D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
 ComPtr<ID3D12CommandQueue> producer,consumer;Hr(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&producer)));Hr(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&consumer)));
 D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
 desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
 std::vector<ComPtr<ID3D12Resource>> targets(3);
 for(auto& t:targets)Hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&t)));
 NativeGateLab::NativeFrameStage stage;Hr(stage.Initialize(d,consumer.Get(),targets));
 void* invalid=reinterpret_cast<void*>(1);if(stage.GetBuffer(9,__uuidof(ID3D12Resource),&invalid)!=E_INVALIDARG || invalid)throw std::runtime_error("invalid GetBuffer accepted");
 if(stage.Prepare(9)!=E_INVALIDARG)throw std::runtime_error("invalid Prepare accepted");
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows=0;UINT64 rowBytes=0,total=0;
 d->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&rowBytes,&total);
 auto upload=Buffer(d,total,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
 auto readback=Buffer(d,total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
 unsigned char* mapped=nullptr;D3D12_RANGE none{0,0};Hr(upload->Map(0,&none,reinterpret_cast<void**>(&mapped)));
 ComPtr<ID3D12CommandAllocator> a,b;ComPtr<ID3D12GraphicsCommandList> x,y;
 Hr(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&a)));Hr(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&b)));
 Hr(d->CreateCommandList(0,qd.Type,a.Get(),nullptr,IID_PPV_ARGS(&x)));Hr(x->Close());
 Hr(d->CreateCommandList(0,qd.Type,b.Get(),nullptr,IID_PPV_ARGS(&y)));Hr(y->Close());
 ComPtr<ID3D12Fence> ready,finished;Hr(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ready)));Hr(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&finished)));
 HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("event");UINT64 checked=0;
 for(UINT frame=1;frame<=12;++frame){
  UINT slot=(frame-1)%3;ComPtr<ID3D12Resource> src;Hr(stage.GetBuffer(slot,IID_PPV_ARGS(&src)));Hr(stage.Prepare(slot));
  for(UINT row=0;row<h;++row)for(UINT col=0;col<w;++col){UINT v=(frame*0x1020304u)^(row*1237u)^(col*69069u);memcpy(mapped+fp.Offset+UINT64(row)*fp.Footprint.RowPitch+col*4,&v,4);}
  Hr(a->Reset());Hr(x->Reset(a.Get(),nullptr));Barrier(x.Get(),src.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
  to.pResource=src.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;x->CopyTextureRegion(&to,0,0,0,&from,nullptr);
  Barrier(x.Get(),src.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);Hr(x->Close());ID3D12CommandList* xx[]={x.Get()};producer->ExecuteCommandLists(1,xx);Hr(producer->Signal(ready.Get(),frame));
  Hr(consumer->Wait(ready.Get(),frame));Hr(stage.Copy(slot,frame));Hr(producer->Wait(stage.CopyFence(),frame));
  Hr(b->Reset());Hr(y->Reset(b.Get(),nullptr));Barrier(y.Get(),targets[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
  from={};to={};from.pResource=targets[slot].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;y->CopyTextureRegion(&to,0,0,0,&from,nullptr);
  Barrier(y.Get(),targets[slot].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);Hr(y->Close());ID3D12CommandList* yy[]={y.Get()};consumer->ExecuteCommandLists(1,yy);Hr(consumer->Signal(finished.Get(),frame));
  Hr(finished->SetEventOnCompletion(frame,event));if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("fence timeout");
  unsigned char* result=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(total)};Hr(readback->Map(0,&range,reinterpret_cast<void**>(&result)));
  for(UINT row=0;row<h;++row){if(memcmp(mapped+fp.Offset+UINT64(row)*fp.Footprint.RowPitch,result+fp.Offset+UINT64(row)*fp.Footprint.RowPitch,w*4)!=0)throw std::runtime_error("pixel mismatch");checked+=w;}
  readback->Unmap(0,&none);
 }
 Hr(stage.Drain());CloseHandle(event);upload->Unmap(0,nullptr);return checked;
}
int main(){try{
 ComPtr<IDXGIFactory4> f;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&f)));ComPtr<IDXGIAdapter> warp;Hr(f->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
 ComPtr<ID3D12Device> d;Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
 auto words=Test(d.Get(),67,13)+Test(d.Get(),3840,2160);
 printf("PASS: owned-frame production helper, 24 frames, 3 slots, %llu pixel words; WARP copy/ownership test only, no XeFG or hardware pacing assertion.\n",(unsigned long long)words);return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
