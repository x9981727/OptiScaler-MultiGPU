#pragma once
// Isolated prototype, not an installed game patch. Every native output is copied
// without format conversion, and only the display queue accesses real DXGI buffers.
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <utility>
namespace NativeGateLab {
class NativeFrameStage {
    using MicrosoftPtr=Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Slot {
        MicrosoftPtr source,target;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 retired=0;
    };
    std::vector<Slot> slots;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> output;
    Microsoft::WRL::ComPtr<ID3D12Fence> copied;
    HANDLE event=nullptr;
    UINT64 last=0;
    static void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,
                        D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource=texture;b.Transition.StateBefore=from;b.Transition.StateAfter=to;
        b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1,&b);
    }
public:
    HRESULT Initialize(ID3D12Device* dev,ID3D12CommandQueue* queue,
                       const std::vector<MicrosoftPtr>& realBuffers) {
        if(!dev || !queue || realBuffers.size()<2 || realBuffers.size()>8) return E_INVALIDARG;
        output=queue; HRESULT hr=dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&copied));
        if(FAILED(hr))return hr;
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event)return HRESULT_FROM_WIN32(GetLastError());
        slots.resize(realBuffers.size());
        for(size_t i=0;i<slots.size();++i) {
            Slot& s=slots[i];s.target=realBuffers[i];
            auto desc=s.target->GetDesc();
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count!=1 ||
               desc.MipLevels!=1 || desc.DepthOrArraySize!=1) return E_INVALIDARG;
            // Retain size, format, sample count and resource flags. No resampling.
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            heap.CreationNodeMask=heap.VisibleNodeMask=1;
            hr=dev->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                    D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&s.source));
            if(FAILED(hr))return hr;
            hr=dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator));
            if(FAILED(hr))return hr;
            hr=dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list));
            if(FAILED(hr))return hr;
            hr=s.list->Close();if(FAILED(hr))return hr;
        }
        return S_OK;
    }
    HRESULT GetBuffer(UINT index,REFIID iid,void** result) {
        if(!result)return E_POINTER;*result=nullptr;
        if(index>=slots.size())return E_INVALIDARG;
        return slots[index].source->QueryInterface(iid,result);
    }
    HRESULT Prepare(UINT index) {
        if(index>=slots.size())return E_INVALIDARG;
        auto& s=slots[index];
        if(s.retired && copied->GetCompletedValue()<s.retired) {
            HRESULT hr=copied->SetEventOnCompletion(s.retired,event);if(FAILED(hr))return hr;
            if(WaitForSingleObject(event,1500)!=WAIT_OBJECT_0)return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
        HRESULT hr=s.allocator->Reset();if(FAILED(hr))return hr;
        return s.list->Reset(s.allocator.Get(),nullptr);
    }
    HRESULT Copy(UINT index,UINT64 serial) {
        if(index>=slots.size() || serial<=last)return E_INVALIDARG;
        Slot& s=slots[index];
        Barrier(s.list.Get(),s.source.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(s.list.Get(),s.target.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
        s.list->CopyResource(s.target.Get(),s.source.Get());
        Barrier(s.list.Get(),s.target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
        Barrier(s.list.Get(),s.source.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        HRESULT hr=s.list->Close();if(FAILED(hr))return hr;
        ID3D12CommandList* lists[]={s.list.Get()};output->ExecuteCommandLists(1,lists);
        hr=output->Signal(copied.Get(),serial);if(FAILED(hr))return hr;
        s.retired=serial;last=serial;return S_OK;
    }
    ID3D12Fence* CopyFence() const { return copied.Get(); }
    HRESULT Drain() {
        if(!output || !copied || !event)return S_OK;
        HRESULT hr=output->Signal(copied.Get(),++last);if(FAILED(hr))return hr;
        hr=copied->SetEventOnCompletion(last,event);if(FAILED(hr))return hr;
        return WaitForSingleObject(event,1500)==WAIT_OBJECT_0 ? S_OK:HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    ~NativeFrameStage(){if(event)CloseHandle(event);}
};
}
