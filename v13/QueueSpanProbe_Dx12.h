#pragma once
#include "QueueIdle_Dx12.h"
#include <array>
#include <cstring>
#include <mutex>

namespace MultiGPU
{
// Diagnostic only. Brackets a queue span with timestamps, never waits in Begin,
// End or Take. Busy slots are skipped. The span includes any idle gaps or waits
// between the markers; it must not be described as pure shader execution time.
class QueueSpanProbe
{
    static constexpr UINT Count = 4;
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator[2];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list[2];
        UINT64 fenceValue = 0;
    };
    std::array<Slot, Count> _slots;
    Microsoft::WRL::ComPtr<ID3D12Device> _device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> _queue;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> _queries;
    Microsoft::WRL::ComPtr<ID3D12Resource> _readback;
    Microsoft::WRL::ComPtr<ID3D12Fence> _fence;
    std::mutex _mutex;
    UINT _next = 0;
    int _active = -1;
    UINT64 _serial = 0, _frequency = 0;
    HRESULT _error = S_OK;
    double _sumMs = 0;
    UINT64 _samples = 0, _skipped = 0;

    HRESULT Initialize(ID3D12CommandQueue* queue)
    {
        if (queue == nullptr) return E_POINTER;
        if (_queue != nullptr) return _queue.Get() == queue ? S_OK : E_INVALIDARG;
        HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&_device));
        if (FAILED(hr)) return hr;
        hr = queue->GetTimestampFrequency(&_frequency);
        if (FAILED(hr) || _frequency == 0) return FAILED(hr) ? hr : E_FAIL;
        D3D12_QUERY_HEAP_DESC q {}; q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; q.Count = Count * 2;
        hr = _device->CreateQueryHeap(&q, IID_PPV_ARGS(&_queries));
        if (FAILED(hr)) return hr;
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = Count * 2 * sizeof(UINT64); desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_readback));
        if (FAILED(hr)) return hr;
        hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
        if (FAILED(hr)) return hr;
        _queue = queue;
        return S_OK;
    }
    HRESULT Prepare(Slot& slot, UINT side)
    {
        auto& allocator = slot.allocator[side]; auto& list = slot.list[side];
        HRESULT hr;
        if (allocator == nullptr)
        {
            hr = _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
            if (FAILED(hr)) return hr;
        }
        if (list == nullptr)
            return _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&list));
        hr = allocator->Reset();
        return FAILED(hr) ? hr : list->Reset(allocator.Get(), nullptr);
    }
    HRESULT Submit(Slot& slot, UINT side)
    {
        const auto hr = slot.list[side]->Close();
        if (FAILED(hr)) return hr;
        ID3D12CommandList* lists[] = {slot.list[side].Get()};
        _queue->ExecuteCommandLists(1, lists);
        return S_OK;
    }
    void Collect()
    {
        if (_fence == nullptr || FAILED(_error)) return;
        const auto completed = _fence->GetCompletedValue();
        if (completed == UINT64_MAX) { _error = DXGI_ERROR_DEVICE_REMOVED; return; }
        for (UINT i = 0; i < Count; ++i)
        {
            auto& slot = _slots[i];
            if (slot.fenceValue == 0 || completed < slot.fenceValue) continue;
            D3D12_RANGE range { i * 2 * sizeof(UINT64), (i * 2 + 2) * sizeof(UINT64) };
            void* data = nullptr;
            const auto hr = _readback->Map(0, &range, &data);
            if (FAILED(hr)) { _error = hr; return; }
            UINT64 ticks[2];
            std::memcpy(ticks, static_cast<unsigned char*>(data) + range.Begin, sizeof(ticks));
            D3D12_RANGE noWrite {0, 0}; _readback->Unmap(0, &noWrite);
            if (ticks[1] >= ticks[0])
            {
                _sumMs += 1000.0 * static_cast<double>(ticks[1] - ticks[0]) / _frequency;
                ++_samples;
            }
            slot.fenceValue = 0;
        }
    }
  public:
    struct Snapshot { double meanMs = -1; UINT64 samples = 0, skipped = 0; HRESULT error = S_OK; };
    QueueSpanProbe() = default;
    QueueSpanProbe(const QueueSpanProbe&) = delete;
    QueueSpanProbe& operator=(const QueueSpanProbe&) = delete;
    ~QueueSpanProbe() { Drain(); }
    bool Begin(ID3D12CommandQueue* queue)
    {
        std::lock_guard lock(_mutex);
        if (_active >= 0 || FAILED(_error)) { ++_skipped; return false; }
        _error = Initialize(queue);
        if (FAILED(_error)) return false;
        Collect();
        if (FAILED(_error)) return false;
        const UINT index = _next++ % Count;
        auto& slot = _slots[index];
        if (slot.fenceValue != 0) { ++_skipped; return false; }
        _error = Prepare(slot, 0);
        if (FAILED(_error)) return false;
        slot.list[0]->EndQuery(_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2);
        _error = Submit(slot, 0);
        if (FAILED(_error)) return false;
        _active = static_cast<int>(index);
        return true;
    }
    void End()
    {
        std::lock_guard lock(_mutex);
        if (_active < 0 || FAILED(_error)) return;
        const UINT index = static_cast<UINT>(_active); _active = -1;
        auto& slot = _slots[index];
        _error = Prepare(slot, 1);
        if (FAILED(_error)) return;
        slot.list[1]->EndQuery(_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2 + 1);
        slot.list[1]->ResolveQueryData(_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2, 2,
                                        _readback.Get(), index * 2 * sizeof(UINT64));
        _error = Submit(slot, 1);
        if (FAILED(_error)) return;
        slot.fenceValue = ++_serial;
        _error = _queue->Signal(_fence.Get(), slot.fenceValue);
    }
    Snapshot Take()
    {
        std::lock_guard lock(_mutex); Collect();
        const Snapshot result {_samples != 0 ? _sumMs / _samples : -1.0, _samples, _skipped, _error};
        _sumMs = 0; _samples = _skipped = 0;
        return result;
    }
    HRESULT Drain()
    {
        std::lock_guard lock(_mutex);
        return _queue != nullptr ? WaitForQueueIdle(_queue.Get()) : S_OK;
    }
};
}
