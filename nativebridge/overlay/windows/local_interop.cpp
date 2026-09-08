// SPDX-License-Identifier: MIT
#include "local_interop.hpp"
#include <dxgi1_6.h>

namespace nb::d3d12 {
namespace {
struct ScopedHandle {
    HANDLE value{};
    ~ScopedHandle(){ if(value) CloseHandle(value); }
};

AdapterId D3D11AdapterId(ID3D11Device5* device) noexcept {
    if (!device) return {};
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf())))) return {};
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) return {};
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return {};
    return {desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart};
}

AdapterId D3D12AdapterId(ID3D12Device* device) noexcept {
    if (!device) return {};
    const LUID luid = device->GetAdapterLuid();
    return {luid.LowPart, luid.HighPart};
}

HRESULT CreateSharedTexturePair(
    ID3D11Device5* device11,
    ID3D12Device* device12,
    const Footprint& plane,
    ComPtr<ID3D11Texture2D>& texture11,
    ComPtr<ID3D12Resource>& texture12) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = plane.extent.width;
    desc.Height = plane.extent.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(plane.format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = device11->CreateTexture2D(&desc, nullptr, texture11.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIResource1> dxgi;
    hr = texture11.As(&dxgi);
    if (FAILED(hr)) return hr;
    ScopedHandle handle;
    hr = dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle.value);
    if (FAILED(hr)) return hr;
    return device12->OpenSharedHandle(handle.value, IID_PPV_ARGS(texture12.ReleaseAndGetAddressOf()));
}
} // namespace

HRESULT LocalInteropFrame::Create(
    ID3D11Device5* device11,
    ID3D12Device* device12,
    const Layout& layout) noexcept {
    if (!device11 || !device12 || device12_) return E_INVALIDARG;
    const AdapterId adapter11 = D3D11AdapterId(device11);
    const AdapterId adapter12 = D3D12AdapterId(device12);
    if ((adapter11.low == 0 && adapter11.high == 0) || adapter11 != adapter12) return E_INVALIDARG;
    adapter_ = adapter12;
    device12_ = device12;

    for (size_t i = 0; i < textures11_.size(); ++i) {
        HRESULT hr = CreateSharedTexturePair(
            device11, device12, layout.planes[i], textures11_[i], textures12_[i]);
        if (FAILED(hr)) return hr;
    }

    HRESULT hr = device11->CreateFence(
        0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence11_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return hr;
    ScopedHandle fenceHandle;
    hr = fence11_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle.value);
    if (FAILED(hr)) return hr;
    return device12->OpenSharedHandle(
        fenceHandle.value, IID_PPV_ARGS(fence12_.ReleaseAndGetAddressOf()));
}

HRESULT LocalInteropFrame::Signal(ID3D12CommandQueue* queue, uint64_t value) noexcept {
    if (!queue || !device12_ || !fence12_ || !value || value == UINT64_MAX || value <= lastSignal_)
        return E_INVALIDARG;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    ComPtr<ID3D12Device> owner;
    HRESULT hr = queue->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()));
    if (FAILED(hr)) return hr;
    if (owner.Get() != device12_.Get()) return E_INVALIDARG;
    hr = queue->Signal(fence12_.Get(), value);
    if (SUCCEEDED(hr)) lastSignal_ = value;
    return hr;
}

std::array<ID3D12Resource*, 3> LocalInteropFrame::Resources12() const noexcept {
    return {textures12_[0].Get(), textures12_[1].Get(), textures12_[2].Get()};
}

} // namespace nb::d3d12
