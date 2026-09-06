#pragma once

#include "QueueIdle_Dx12.h"
#include <array>
#include <chrono>
#include <mutex>

namespace MultiGPU
{
// Dedicated to XeFG input import + SDK tagging. Backbuffer copies use their
// existing independent pool, so importing a frame never resets that frame's
// just-submitted allocator. Reuse waits only for an older use of the ring slot.
class XeFGTransferPool
{
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 completedAfter = 0;
    };
    static constexpr UINT SlotCount = 4;
    std::array<Slot, SlotCount> _slots;
    Microsoft::WRL::ComPtr<ID3D12Device> _device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> _queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> _fence;
    std::mutex _mutex;
    UINT64 _serial = 0;
    int _recording = -1;
    HRESULT _poisoned = S_OK;
    double _reuseWaitMs = 0.0;

    HRESULT WaitUnlocked(UINT slot, DWORD timeoutMs)
    {
        if (FAILED(_poisoned))
            return _poisoned;
        const auto value = _slots[slot].completedAfter;
        if (value == 0 || _fence == nullptr)
            return S_OK;
        const auto started = std::chrono::steady_clock::now();
        const HRESULT hr = WaitForFenceValue(_fence.Get(), value, timeoutMs);
        _reuseWaitMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started).count();
        return hr;
    }

  public:
    ~XeFGTransferPool() { Drain(); }
    XeFGTransferPool() = default;
    XeFGTransferPool(const XeFGTransferPool&) = delete;
    XeFGTransferPool& operator=(const XeFGTransferPool&) = delete;

    // Called before the producer overwrites a cross-adapter source slot, as
    // well as before resetting its consumer allocator. Never wait after submit.
    HRESULT WaitForSlot(UINT frameSlot, DWORD timeoutMs = 5000)
    {
        std::lock_guard lock(_mutex);
        return WaitUnlocked(frameSlot % SlotCount, timeoutMs);
    }

    HRESULT Begin(ID3D12CommandQueue* queue, UINT frameSlot, ID3D12GraphicsCommandList** output)
    {
        if (output == nullptr)
            return E_POINTER;
        *output = nullptr;
        if (queue == nullptr)
            return E_POINTER;
        std::lock_guard lock(_mutex);
        if (FAILED(_poisoned))
            return _poisoned;
        if (_recording >= 0 || (_queue != nullptr && _queue.Get() != queue))
            return DXGI_ERROR_INVALID_CALL;
        if (_queue == nullptr)
        {
            HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&_device));
            if (FAILED(hr))
                return hr;
            hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
            if (FAILED(hr))
                return hr;
            _queue = queue;
        }
        const UINT index = frameSlot % SlotCount;
        HRESULT hr = WaitUnlocked(index, 5000);
        if (FAILED(hr))
            return hr;
        auto& slot = _slots[index];
        if (slot.allocator == nullptr)
        {
            hr = _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
            if (FAILED(hr))
                return hr;
        }
        if (slot.list == nullptr)
        {
            hr = _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&slot.list));
            if (FAILED(hr))
                return hr;
        }
        else
        {
            hr = slot.allocator->Reset();
            if (FAILED(hr))
                return hr;
            hr = slot.list->Reset(slot.allocator.Get(), nullptr);
            if (FAILED(hr))
                return hr;
        }
        _recording = static_cast<int>(index);
        *output = slot.list.Get();
        return S_OK;
    }

    HRESULT Submit(UINT frameSlot)
    {
        std::lock_guard lock(_mutex);
        const UINT index = frameSlot % SlotCount;
        if (_recording != static_cast<int>(index))
            return DXGI_ERROR_INVALID_CALL;
        auto& slot = _slots[index];
        const HRESULT hr = slot.list->Close();
        _recording = -1;
        if (FAILED(hr))
        {
            _poisoned = hr;
            return hr;
        }
        ID3D12CommandList* lists[] = { slot.list.Get() };
        _queue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++_serial;
        const HRESULT signaled = _queue->Signal(_fence.Get(), value);
        if (FAILED(signaled))
        {
            // Submitted work must never be mistaken for an unused allocator.
            _poisoned = signaled;
            return signaled;
        }
        slot.completedAfter = value;
        return S_OK;
    }

    void Abort(UINT frameSlot)
    {
        std::lock_guard lock(_mutex);
        const UINT index = frameSlot % SlotCount;
        if (_recording == static_cast<int>(index))
        {
            const HRESULT hr = _slots[index].list->Close();
            if (FAILED(hr))
                _poisoned = hr;
            _recording = -1;
        }
    }

    HRESULT Drain()
    {
        std::lock_guard lock(_mutex);
        return _queue != nullptr ? WaitForQueueIdle(_queue.Get()) : S_OK;
    }

    double TakeReuseWaitMs()
    {
        std::lock_guard lock(_mutex);
        const double result = _reuseWaitMs;
        _reuseWaitMs = 0.0;
        return result;
    }
};
} // namespace MultiGPU
