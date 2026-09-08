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
// textures. The consumer D3D12 device opens NT handles to those textures and
// writes the cross-adapter frame into them, then signals a fence created by the
// D3D11 device. This is the same direction Magpie already uses for its existing
// D3D11/D3D12 guidance interop.
class LocalInteropFrame {
public:
    LocalInteropFrame() = default;
    LocalInteropFrame(const LocalInteropFrame&) = delete;
    LocalInteropFrame& operator=(const LocalInteropFrame&) = delete;

    HRESULT Create(ID3D11Device5* device11, ID3D12Device* device12, const Layout& layout) noexcept;
    HRESULT Signal(ID3D12CommandQueue* queue, uint64_t value) noexcept;

    std::array<ID3D12Resource*, 3> Resources12() const noexcept;
    const std::array<ComPtr<ID3D11Texture2D>, 3>& Textures11() const noexcept { return textures11_; }
    ID3D11Fence* Fence11() const noexcept { return fence11_.Get(); }
    AdapterId Adapter() const noexcept { return adapter_; }

private:
    ComPtr<ID3D12Device> device12_;
    std::array<ComPtr<ID3D11Texture2D>, 3> textures11_;
    std::array<ComPtr<ID3D12Resource>, 3> textures12_;
    ComPtr<ID3D11Fence> fence11_;
    ComPtr<ID3D12Fence> fence12_;
    AdapterId adapter_{};
    uint64_t lastSignal_{};
};

} // namespace nb::d3d12
