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

// Same-adapter D3D12 -> D3D11 handoff used by the Magpie consumer.
// The cross-adapter transport lands into these D3D12 textures, restores them
// to COMMON, signals a shared fence, and Magpie waits on that fence from D3D11.
class LocalInteropFrame {
public:
    LocalInteropFrame() = default;
    LocalInteropFrame(const LocalInteropFrame&) = delete;
    LocalInteropFrame& operator=(const LocalInteropFrame&) = delete;
    ~LocalInteropFrame();

    HRESULT Create(ID3D12Device* device, const Layout& layout) noexcept;
    HRESULT OpenD3D11(
        ID3D11Device5* device,
        std::array<ComPtr<ID3D11Texture2D>, 3>& textures,
        ComPtr<ID3D11Fence>& fence) const noexcept;
    HRESULT Signal(ID3D12CommandQueue* queue, uint64_t value) noexcept;

    std::array<ID3D12Resource*, 3> Resources() const noexcept;
    const std::array<ComPtr<ID3D12Resource>, 3>& TextureObjects() const noexcept { return textures_; }
    AdapterId Adapter() const noexcept { return adapter_; }

private:
    ComPtr<ID3D12Device> device_;
    std::array<ComPtr<ID3D12Resource>, 3> textures_;
    std::array<HANDLE, 3> textureHandles_{};
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceHandle_{};
    AdapterId adapter_{};
    uint64_t lastSignal_{};
};

} // namespace nb::d3d12
