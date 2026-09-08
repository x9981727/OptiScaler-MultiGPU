// SPDX-License-Identifier: MIT
#include "local_interop.hpp"
#include <dxgi1_6.h>

namespace nb::d3d12 {
namespace {
D3D12_RESOURCE_DESC MakeTextureDesc(const Footprint& p) noexcept {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = p.extent.width;
    d.Height = p.extent.height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = static_cast<DXGI_FORMAT>(p.format);
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return d;
}

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
} // namespace

LocalInteropFrame::~LocalInteropFrame() {
    for (HANDLE h : textureHandles_) if (h) CloseHandle(h);
    if (fenceHandle_) CloseHandle(fenceHandle_);
}

HRESULT LocalInteropFrame::Create(ID3D12Device* device, const Layout& layout) noexcept {
    if (!device || device_) return E_INVALIDARG;
    device_ = device;
    const LUID luid = device->GetAdapterLuid();
    adapter_ = {luid.LowPart, luid.HighPart};

    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (size_t i = 0; i < textures_.size(); ++i) {
        const D3D12_RESOURCE_DESC desc = MakeTextureDesc(layout.planes[i]);
        HRESULT hr = device_->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(textures_[i].ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return hr;
        hr = device_->CreateSharedHandle(
            textures_[i].Get(), nullptr, GENERIC_ALL, nullptr, &textureHandles_[i]);
        if (FAILED(hr)) return hr;
    }

    HRESULT hr = device_->CreateFence(
        0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return hr;
    return device_->CreateSharedHandle(fence_.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle_);
}

HRESULT LocalInteropFrame::OpenD3D11(
    ID3D11Device5* device,
    std::array<ComPtr<ID3D11Texture2D>, 3>& textures,
    ComPtr<ID3D11Fence>& fence) const noexcept {
    if (!device || !device_ || !fenceHandle_ || D3D11AdapterId(device) != adapter_) return E_INVALIDARG;
    for (size_t i = 0; i < textures.size(); ++i) {
        if (!textureHandles_[i]) return E_UNEXPECTED;
        HRESULT hr = device->OpenSharedResource1(
            textureHandles_[i], IID_PPV_ARGS(textures[i].ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return hr;
    }
    return device->OpenSharedFence(fenceHandle_, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf()));
}

HRESULT LocalInteropFrame::Signal(ID3D12CommandQueue* queue, uint64_t value) noexcept {
    if (!queue || !device_ || !fence_ || !value || value == UINT64_MAX || value <= lastSignal_)
        return E_INVALIDARG;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    ComPtr<ID3D12Device> owner;
    HRESULT hr = queue->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()));
    if (FAILED(hr)) return hr;
    if (owner.Get() != device_.Get()) return E_INVALIDARG;
    hr = queue->Signal(fence_.Get(), value);
    if (SUCCEEDED(hr)) lastSignal_ = value;
    return hr;
}

std::array<ID3D12Resource*, 3> LocalInteropFrame::Resources() const noexcept {
    return {textures_[0].Get(), textures_[1].Get(), textures_[2].Get()};
}

} // namespace nb::d3d12
