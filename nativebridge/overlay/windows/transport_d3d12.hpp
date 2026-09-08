// SPDX-License-Identifier: MIT
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "native_bridge/contract.hpp"
namespace nb::d3d12 {
using Microsoft::WRL::ComPtr;
struct Handles {
    HANDLE heap{};
    std::array<HANDLE,kSlotCount> ready{},done{};
    Handles()=default;
    Handles(const Handles&)=delete; Handles& operator=(const Handles&)=delete;
    ~Handles();
};
class Endpoint {
public:
    HRESULT CreateProducer(ID3D12Device*, Extent, Format color) noexcept;
    HRESULT OpenConsumer(ID3D12Device*, Extent, Format color, const Handles&) noexcept;
    const Handles& ExportHandles() const noexcept { return handles_; }
    const Layout& GetLayout() const noexcept { return layout_; }
    AdapterId Adapter() const noexcept;
    HRESULT CreateLocalTextures(std::array<ComPtr<ID3D12Resource>,3>&) const noexcept;

    // All-plane helpers for callers that have a coherent full set at once.
    HRESULT RecordWrite(uint32_t slot, ID3D12GraphicsCommandList*,
        const std::array<ID3D12Resource*,3>&, const std::array<D3D12_RESOURCE_STATES,3>&) noexcept;
    HRESULT RecordRead(uint32_t slot, ID3D12GraphicsCommandList*,
        const std::array<ID3D12Resource*,3>&, const std::array<D3D12_RESOURCE_STATES,3>&) noexcept;

    // Streamline can expose eOnlyValidNow resources in separate tag calls. Capture
    // each canonical plane immediately on the game-provided command list rather
    // than retaining the engine resource until the other planes arrive.
    HRESULT RecordWritePlane(uint32_t slot, uint32_t plane, ID3D12GraphicsCommandList*,
        ID3D12Resource*, D3D12_RESOURCE_STATES) noexcept;
    HRESULT RecordReadPlane(uint32_t slot, uint32_t plane, ID3D12GraphicsCommandList*,
        ID3D12Resource*, D3D12_RESOURCE_STATES) noexcept;

    HRESULT SignalReady(ID3D12CommandQueue*,uint32_t slot,uint64_t serial) noexcept;
    HRESULT SignalDone(ID3D12CommandQueue*,uint32_t slot,uint64_t serial) noexcept;
    HRESULT PollReady(uint32_t slot,uint64_t serial) const noexcept;
    HRESULT PollDone(uint32_t slot,uint64_t serial) const noexcept;
private:
    HRESULT Setup(ID3D12Device*,Extent,Format) noexcept;
    HRESULT CreateBuffers() noexcept;
    HRESULT ValidatePlane(bool write,uint32_t,uint32_t,ID3D12GraphicsCommandList*,ID3D12Resource*) noexcept;
    HRESULT RecordPlane(bool write,uint32_t,uint32_t,ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES) noexcept;
    HRESULT Record(bool write,uint32_t,ID3D12GraphicsCommandList*,
        const std::array<ID3D12Resource*,3>&,const std::array<D3D12_RESOURCE_STATES,3>&) noexcept;
    bool producer_{};
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12Heap> heap_;
    std::array<ComPtr<ID3D12Resource>,kSlotCount> buffers_;
    std::array<ComPtr<ID3D12Fence>,kSlotCount> ready_,done_;
    Handles handles_;
    Layout layout_{};
    std::array<uint64_t,kSlotCount> lastReady_{},lastDone_{};
};
}
