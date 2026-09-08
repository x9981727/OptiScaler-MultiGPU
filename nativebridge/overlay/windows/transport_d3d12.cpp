// SPDX-License-Identifier: MIT
#include "transport_d3d12.hpp"
#include <limits>
namespace nb::d3d12 {
Handles::~Handles(){if(heap)CloseHandle(heap);for(auto h:ready)if(h)CloseHandle(h);for(auto h:done)if(h)CloseHandle(h);}
static D3D12_RESOURCE_DESC buffer_desc(uint64_t n) noexcept {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=n;d.Height=1;
    d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags=D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;return d;
}
static D3D12_RESOURCE_DESC texture_desc(const Footprint& f) noexcept {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width=f.extent.width;d.Height=f.extent.height;d.DepthOrArraySize=1;d.MipLevels=1;
    d.Format=static_cast<DXGI_FORMAT>(f.format);d.SampleDesc.Count=1;
    d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;return d;
}
static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};return b;
}
HRESULT Endpoint::Setup(ID3D12Device* d,Extent e,Format c) noexcept {
    if(!d||device_)return E_INVALIDARG;
    auto layout=make_layout(e,c);if(!layout)return E_INVALIDARG;
    device_=d;layout_=*layout;
    for(const auto& p:layout_.planes){const auto desc=texture_desc(p);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT out{};UINT rows{};UINT64 rowBytes{},bytes{};
        d->GetCopyableFootprints(&desc,0,1,p.offset,&out,&rows,&rowBytes,&bytes);
        if(out.Offset!=p.offset||out.Footprint.RowPitch!=p.rowPitch||rows!=p.extent.height||rowBytes!=p.rowBytes)
            return DXGI_ERROR_UNSUPPORTED;
    }
    const auto desc=buffer_desc(layout_.bufferBytes);
    const auto allocation=d->GetResourceAllocationInfo(0,1,&desc);
    if(allocation.SizeInBytes==UINT64_MAX||!allocation.Alignment ||
       allocation.SizeInBytes>layout_.slotStride||layout_.slotStride%allocation.Alignment)
        return DXGI_ERROR_UNSUPPORTED;
    return S_OK;
}
HRESULT Endpoint::CreateBuffers() noexcept {
    const auto desc=buffer_desc(layout_.bufferBytes);
    for(uint32_t i=0;i<kSlotCount;++i){const HRESULT hr=device_->CreatePlacedResource(heap_.Get(),
        layout_.slotStride*i,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(buffers_[i].ReleaseAndGetAddressOf()));
        if(FAILED(hr))return hr;}
    return S_OK;
}
HRESULT Endpoint::CreateProducer(ID3D12Device* d,Extent e,Format c) noexcept {
    HRESULT hr=Setup(d,e,c);if(FAILED(hr))return hr;producer_=true;
    D3D12_HEAP_DESC desc{};desc.SizeInBytes=layout_.heapBytes;desc.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;
    desc.Flags=D3D12_HEAP_FLAG_SHARED|D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    hr=d->CreateHeap(&desc,IID_PPV_ARGS(heap_.ReleaseAndGetAddressOf()));if(FAILED(hr))return hr;
    hr=CreateBuffers();if(FAILED(hr))return hr;
    hr=d->CreateSharedHandle(heap_.Get(),nullptr,GENERIC_ALL,nullptr,&handles_.heap);if(FAILED(hr))return hr;
    for(uint32_t i=0;i<kSlotCount;++i){
        constexpr auto flags=D3D12_FENCE_FLAG_SHARED|D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER;
        hr=d->CreateFence(0,flags,IID_PPV_ARGS(ready_[i].ReleaseAndGetAddressOf()));if(FAILED(hr))return hr;
        hr=d->CreateFence(0,flags,IID_PPV_ARGS(done_[i].ReleaseAndGetAddressOf()));if(FAILED(hr))return hr;
        hr=d->CreateSharedHandle(ready_[i].Get(),nullptr,GENERIC_ALL,nullptr,&handles_.ready[i]);if(FAILED(hr))return hr;
        hr=d->CreateSharedHandle(done_[i].Get(),nullptr,GENERIC_ALL,nullptr,&handles_.done[i]);if(FAILED(hr))return hr;
    }
    return S_OK;
}
HRESULT Endpoint::OpenConsumer(ID3D12Device* d,Extent e,Format c,const Handles& h) noexcept {
    HRESULT hr=Setup(d,e,c);if(FAILED(hr))return hr;
    if(!h.heap)return E_INVALIDARG;
    hr=d->OpenSharedHandle(h.heap,IID_PPV_ARGS(heap_.ReleaseAndGetAddressOf()));if(FAILED(hr))return hr;
    if(heap_->GetDesc().SizeInBytes<layout_.heapBytes)return E_INVALIDARG;
    hr=CreateBuffers();if(FAILED(hr))return hr;
    for(uint32_t i=0;i<kSlotCount;++i){
        if(!h.ready[i]||!h.done[i])return E_INVALIDARG;
        hr=d->OpenSharedHandle(h.ready[i],IID_PPV_ARGS(ready_[i].ReleaseAndGetAddressOf()));if(FAILED(hr))return hr;
        hr=d->OpenSharedHandle(h.done[i],IID_PPV_ARGS(done_[i].ReleaseAndGetAddressOf()));if(FAILED(hr))return hr;
    }
    return S_OK;
}
AdapterId Endpoint::Adapter() const noexcept {if(!device_)return {};const LUID id=device_->GetAdapterLuid();return {id.LowPart,id.HighPart};}
HRESULT Endpoint::CreateLocalTextures(std::array<ComPtr<ID3D12Resource>,3>& out) const noexcept {
    if(!device_)return E_UNEXPECTED;
    D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;
    for(size_t i=0;i<3;++i){const auto desc=texture_desc(layout_.planes[i]);
        const HRESULT hr=device_->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(out[i].ReleaseAndGetAddressOf()));if(FAILED(hr))return hr;}
    return S_OK;
}
HRESULT Endpoint::ValidatePlane(bool write,uint32_t slot,uint32_t plane,ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* resource) noexcept {
    if(!device_||slot>=kSlotCount||plane>=layout_.planes.size()||!cmd||!resource||!buffers_[slot]||producer_!=write)
        return E_INVALIDARG;
    if(cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
    ComPtr<ID3D12Device> commandOwner;
    HRESULT hr=cmd->GetDevice(IID_PPV_ARGS(commandOwner.GetAddressOf()));if(FAILED(hr))return hr;
    if(commandOwner.Get()!=device_.Get())return E_INVALIDARG;
    const auto d=resource->GetDesc();const auto& p=layout_.planes[plane];
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width!=p.extent.width||d.Height!=p.extent.height||
       d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||d.Format!=static_cast<DXGI_FORMAT>(p.format))
        return DXGI_ERROR_UNSUPPORTED;
    ComPtr<ID3D12Device> owner;hr=resource->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()));if(FAILED(hr))return hr;
    return owner.Get()==device_.Get()?S_OK:E_INVALIDARG;
}
HRESULT Endpoint::RecordPlane(bool write,uint32_t slot,uint32_t plane,ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* resource,D3D12_RESOURCE_STATES state) noexcept {
    HRESULT hr=ValidatePlane(write,slot,plane,cmd,resource);if(FAILED(hr))return hr;
    const auto sharedState=write?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COPY_SOURCE;
    const auto localState=write?D3D12_RESOURCE_STATE_COPY_SOURCE:D3D12_RESOURCE_STATE_COPY_DEST;
    auto b=transition(buffers_[slot].Get(),D3D12_RESOURCE_STATE_COMMON,sharedState);cmd->ResourceBarrier(1,&b);
    if(state!=localState){b=transition(resource,state,localState);cmd->ResourceBarrier(1,&b);}
    const auto& p=layout_.planes[plane];
    D3D12_TEXTURE_COPY_LOCATION local{};local.pResource=resource;local.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION shared{};shared.pResource=buffers_[slot].Get();shared.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    shared.PlacedFootprint.Offset=p.offset;
    shared.PlacedFootprint.Footprint={static_cast<DXGI_FORMAT>(p.format),p.extent.width,p.extent.height,1,p.rowPitch};
    if(write)cmd->CopyTextureRegion(&shared,0,0,0,&local,nullptr);else cmd->CopyTextureRegion(&local,0,0,0,&shared,nullptr);
    if(state!=localState){b=transition(resource,localState,state);cmd->ResourceBarrier(1,&b);}
    b=transition(buffers_[slot].Get(),sharedState,D3D12_RESOURCE_STATE_COMMON);cmd->ResourceBarrier(1,&b);
    return S_OK;
}
HRESULT Endpoint::Record(bool write,uint32_t slot,ID3D12GraphicsCommandList* cmd,
    const std::array<ID3D12Resource*,3>& resources,const std::array<D3D12_RESOURCE_STATES,3>& states) noexcept {
    // Validate all planes before touching the caller command list.
    for(uint32_t i=0;i<3;++i){const HRESULT hr=ValidatePlane(write,slot,i,cmd,resources[i]);if(FAILED(hr))return hr;}
    for(uint32_t i=0;i<3;++i){const HRESULT hr=RecordPlane(write,slot,i,cmd,resources[i],states[i]);if(FAILED(hr))return hr;}
    return S_OK;
}
HRESULT Endpoint::RecordWrite(uint32_t s,ID3D12GraphicsCommandList* c,const std::array<ID3D12Resource*,3>&r,
    const std::array<D3D12_RESOURCE_STATES,3>&t) noexcept{return Record(true,s,c,r,t);}
HRESULT Endpoint::RecordRead(uint32_t s,ID3D12GraphicsCommandList* c,const std::array<ID3D12Resource*,3>&r,
    const std::array<D3D12_RESOURCE_STATES,3>&t) noexcept{return Record(false,s,c,r,t);}
HRESULT Endpoint::RecordWritePlane(uint32_t s,uint32_t p,ID3D12GraphicsCommandList* c,ID3D12Resource* r,
    D3D12_RESOURCE_STATES t) noexcept{return RecordPlane(true,s,p,c,r,t);}
HRESULT Endpoint::RecordReadPlane(uint32_t s,uint32_t p,ID3D12GraphicsCommandList* c,ID3D12Resource* r,
    D3D12_RESOURCE_STATES t) noexcept{return RecordPlane(false,s,p,c,r,t);}
static HRESULT signal(ID3D12Device* device,ID3D12CommandQueue* q,ID3D12Fence* f,uint64_t value,uint64_t& last) noexcept {
    if(!q||!f||!value||value==UINT64_MAX||value<=last)return E_INVALIDARG;
    if(q->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
    ComPtr<ID3D12Device> owner;HRESULT hr=q->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()));if(FAILED(hr))return hr;
    if(owner.Get()!=device)return E_INVALIDARG;
    hr=q->Signal(f,value);if(SUCCEEDED(hr))last=value;return hr;
}
HRESULT Endpoint::SignalReady(ID3D12CommandQueue* q,uint32_t s,uint64_t n) noexcept {
    if(!producer_||s>=kSlotCount)return E_INVALIDARG;return signal(device_.Get(),q,ready_[s].Get(),n,lastReady_[s]);}
HRESULT Endpoint::SignalDone(ID3D12CommandQueue* q,uint32_t s,uint64_t n) noexcept {
    if(producer_||s>=kSlotCount)return E_INVALIDARG;return signal(device_.Get(),q,done_[s].Get(),n,lastDone_[s]);}
static HRESULT poll(ID3D12Fence* f,uint64_t n) noexcept {
    if(!f||!n||n==UINT64_MAX)return E_INVALIDARG;const auto value=f->GetCompletedValue();
    if(value==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
    return value>=n ? S_OK:DXGI_ERROR_WAS_STILL_DRAWING;
}
HRESULT Endpoint::PollReady(uint32_t s,uint64_t n) const noexcept {return s<kSlotCount?poll(ready_[s].Get(),n):E_INVALIDARG;}
HRESULT Endpoint::PollDone(uint32_t s,uint64_t n) const noexcept {return s<kSlotCount?poll(done_[s].Get(),n):E_INVALIDARG;}
}
