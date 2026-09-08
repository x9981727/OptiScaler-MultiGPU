#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <cstring>
namespace NativeGateLab {
class NativeGpuReadyProbe {
 using Microsoft::WRL::ComPtr;
 struct Slot {ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;UINT64 ticket=0;};
 ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12QueryHeap> queries;ComPtr<ID3D12Resource> readback;
 std::vector<Slot> slots;UINT64 frequency=0;unsigned char* mapped=nullptr;
public:
 HRESULT Initialize(ID3D12Device* device,ID3D12CommandQueue* producer,UINT count){
  if(!device||!producer||count<2||count>8)return E_INVALIDARG;
  queue=producer;HRESULT hr=queue->GetTimestampFrequency(&frequency);if(FAILED(hr)||!frequency)return FAILED(hr)?hr:E_FAIL;
  D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=count;
  hr=device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries));if(FAILED(hr))return hr;
  D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_READBACK;h.CreationNodeMask=h.VisibleNodeMask=1;
  D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=count*sizeof(UINT64);d.Height=1;
  d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  hr=device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback));if(FAILED(hr))return hr;
  D3D12_RANGE range{0,static_cast<SIZE_T>(d.Width)};hr=readback->Map(0,&range,reinterpret_cast<void**>(&mapped));if(FAILED(hr))return hr;
  slots.resize(count);
  for(auto& s:slots){
   hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator));if(FAILED(hr))return hr;
   hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list));if(FAILED(hr))return hr;
   hr=s.list->Close();if(FAILED(hr))return hr;
  }
  return S_OK;
 }
 HRESULT Record(UINT index,UINT64 ticket,ID3D12Fence* completed){
  if(index>=slots.size()||!completed||ticket==0)return E_INVALIDARG;
  auto& s=slots[index];if(s.ticket&&completed->GetCompletedValue()<s.ticket)return DXGI_ERROR_WAS_STILL_DRAWING;
  HRESULT hr=s.allocator->Reset();if(FAILED(hr))return hr;
  hr=s.list->Reset(s.allocator.Get(),nullptr);if(FAILED(hr))return hr;
  s.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index);
  s.list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index,1,readback.Get(),index*sizeof(UINT64));
  hr=s.list->Close();if(FAILED(hr))return hr;
  ID3D12CommandList* lists[]={s.list.Get()};queue->ExecuteCommandLists(1,lists);s.ticket=ticket;return S_OK;
 }
 double ReadMilliseconds(UINT index,UINT64 ticket,ID3D12Fence* completed)const{
  if(index>=slots.size()||!mapped||!completed||!frequency||slots[index].ticket!=ticket||completed->GetCompletedValue()<ticket)return -1;
  UINT64 value=0;std::memcpy(&value,mapped+index*sizeof(UINT64),sizeof(value));
  return static_cast<double>(value)*1000.0/static_cast<double>(frequency);
 }
 ~NativeGpuReadyProbe(){if(mapped&&readback){D3D12_RANGE none{0,0};readback->Unmap(0,&none);}}
};
}
