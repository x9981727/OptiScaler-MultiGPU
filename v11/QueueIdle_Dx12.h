#pragma once

#include <cstdint>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace MultiGPU
{
// Poll only during resize/teardown, with a finite timeout. This avoids leaving
// an event registration alive after an early return or a failed GPU wait.
inline HRESULT WaitForFenceValue(ID3D12Fence* fence, UINT64 value, DWORD timeoutMs)
{
    if (fence == nullptr)
        return E_POINTER;
    const ULONGLONG start = GetTickCount64();
    for (;;)
    {
        const UINT64 completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX)
            return DXGI_ERROR_DEVICE_REMOVED;
        if (completed >= value)
            return S_OK;
        if (GetTickCount64() - start >= timeoutMs)
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(1);
    }
}

inline HRESULT WaitForQueueIdle(ID3D12CommandQueue* queue, DWORD timeoutMs = 5000)
{
    if (queue == nullptr)
        return E_POINTER;
    // The fence must belong to the queue's device, not a global game device.
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    HRESULT result = queue->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(result))
        return result;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result))
        return result;
    result = queue->Signal(fence.Get(), 1);
    if (FAILED(result))
        return result;
    return WaitForFenceValue(fence.Get(), 1, timeoutMs);
}
} // namespace MultiGPU
