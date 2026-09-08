// SPDX-License-Identifier: MIT
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "native_bridge/contract.hpp"

namespace nb::d3d12 {
using Microsoft::WRL::ComPtr;

// Same-adapter handoff for the Magpie consumer. Magpie owns ordinary D3D11
// textures. D3D12 opens NT handles to them and writes cross-adapter input into
// those textures. readyFence orders D3D12->D3D11; consumedFence orders the
// D3D11 staging copy back to the next D3D12 write, preventing overwrite races.
class LocalInteropFrame {
public:
    LocalInteropFrame() = default;
    LocalInteropFrame(const LocalInteropFrame&) = delete;
    LocalInteropFrame& operator=(const LocalInteropFrame&) = delete;

    HRESULT Create(ID3D11Device5* device11, ID3D12Device* device12, const Layout& layout) noexcept;
    HRESULT WaitConsumed(ID3D12CommandQueue* queue, uint64_t value) noexcept;
    HRESULT SignalReady(ID3D12CommandQueue* queue, uint64_t value) noexcept;

    std::array<ID3D12Resource*, 3> Resources12() const noexcept;
    const std::array<ComPtr<ID3D11Texture2D>, 3>& Textures11() const noexcept { return textures11_; }
    ID3D11Fence* ReadyFence11() const noexcept { return ready11_.Get(); }
    ID3D11Fence* ConsumedFence11() const noexcept { return consumed11_.Get(); }
    AdapterId Adapter() const noexcept { return adapter_; }

private:
    HRESULT _ValidateQueue(ID3D12CommandQueue* queue) const noexcept;

    ComPtr<ID3D12Device> device12_;
    std::array<ComPtr<ID3D11Texture2D>, 3> textures11_;
    std::array<ComPtr<ID3D12Resource>, 3> textures12_;
    ComPtr<ID3D11Fence> ready11_;
    ComPtr<ID3D12Fence> ready12_;
    ComPtr<ID3D11Fence> consumed11_;
    ComPtr<ID3D12Fence> consumed12_;
    AdapterId adapter_{};
    uint64_t lastReady_{};
    uint64_t lastWaitedConsumed_{};
};

} // namespace nb::d3d12
