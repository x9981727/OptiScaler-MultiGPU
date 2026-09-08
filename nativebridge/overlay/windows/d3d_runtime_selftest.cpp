// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "transport_d3d12.hpp"
#include "local_interop.hpp"

using Microsoft::WRL::ComPtr;
using nb::d3d12::Endpoint;
using nb::d3d12::Handles;
using nb::d3d12::LocalInteropFrame;

namespace {
constexpr uint64_t kSerial = 7;
constexpr nb::Extent kExtent{8, 4};

bool DuplicateOne(HANDLE source, HANDLE& target) {
    return source && DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &target,
        0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
}
bool DuplicateHandles(const Handles& source, Handles& target) {
    if (!DuplicateOne(source.heap, target.heap)) return false;
    for (size_t i = 0; i < nb::kSlotCount; ++i) {
        if (!DuplicateOne(source.ready[i], target.ready[i]) ||
            !DuplicateOne(source.done[i], target.done[i])) return false;
    }
    return true;
}

HRESULT CreateQueue(ID3D12Device* device, ComPtr<ID3D12CommandQueue>& queue) {
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    return device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.ReleaseAndGetAddressOf()));
}

HRESULT CreateList(ID3D12Device* device, ComPtr<ID3D12CommandAllocator>& allocator,
                   ComPtr<ID3D12GraphicsCommandList>& list) {
    HRESULT hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return hr;
    return device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(list.ReleaseAndGetAddressOf()));
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return b;
}

HRESULT UploadTexture(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                      ID3D12Resource* texture, size_t plane,
                      ComPtr<ID3D12Resource>& upload) {
    const auto td = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0, totalBytes = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    if (!rows || !rowBytes || !totalBytes) return E_FAIL;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = totalBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(upload.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return hr;

    uint8_t* mapped = nullptr;
    hr = upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr)) return hr;
    for (UINT y = 0; y < rows; ++y) {
        uint8_t* row = mapped + footprint.Offset + uint64_t(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < td.Width; ++x) {
            if (plane == 0) {
                const uint8_t px[4]{uint8_t(0x11 + x), uint8_t(0x22 + y), 0x33, 0x44};
                std::memcpy(row + x * 4, px, 4);
            } else if (plane == 1) {
                const float depth = 0.25f + float(x + y) / 64.0f;
                std::memcpy(row + x * 4, &depth, 4);
            } else {
                const uint16_t mv[2]{0x3c00u, 0xc000u};
                std::memcpy(row + x * 4, mv, 4);
            }
        }
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    auto b = Transition(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &b);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &b);
    return S_OK;
}

HRESULT WaitReady(const Endpoint& endpoint, uint32_t slot, uint64_t serial, bool done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const HRESULT hr = done ? endpoint.PollDone(slot, serial) : endpoint.PollReady(slot, serial);
        if (hr == S_OK) return S_OK;
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING) return hr;
        if (std::chrono::steady_clock::now() >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(1);
    }
}

bool VerifyD3D11(ID3D11Device5* device, ID3D11DeviceContext4* context,
                 const std::array<ComPtr<ID3D11Texture2D>, 3>& textures,
                 ID3D11Fence* fence) {
    context->Wait(fence, kSerial);
    std::array<ComPtr<ID3D11Texture2D>, 3> staging;
    for (size_t i = 0; i < textures.size(); ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        textures[i]->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging[i].ReleaseAndGetAddressOf()))) return false;
        context->CopyResource(staging[i].Get(), textures[i].Get());
    }
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(staging[0].Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    const uint8_t expectedColor[4]{0x11, 0x22, 0x33, 0x44};
    const bool colorOk = std::memcmp(map.pData, expectedColor, 4) == 0;
    context->Unmap(staging[0].Get(), 0);

    if (FAILED(context->Map(staging[1].Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    float depth = 0.0f;
    std::memcpy(&depth, map.pData, sizeof(depth));
    const bool depthOk = depth == 0.25f;
    context->Unmap(staging[1].Get(), 0);

    if (FAILED(context->Map(staging[2].Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    uint16_t mv[2]{};
    std::memcpy(mv, map.pData, sizeof(mv));
    const bool motionOk = mv[0] == 0x3c00u && mv[1] == 0xc000u;
    context->Unmap(staging[2].Get(), 0);
    return colorOk && depthOk && motionOk;
}

int RunOnAdapter(IDXGIAdapter1* adapter) {
    ComPtr<ID3D12Device> producerDevice, consumerDevice;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(producerDevice.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return 77;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(consumerDevice.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return 77;

    Endpoint producer, consumer;
    hr = producer.CreateProducer(producerDevice.Get(), kExtent, nb::Format::Rgba8);
    if (hr == DXGI_ERROR_UNSUPPORTED || hr == E_NOTIMPL) return 77;
    if (FAILED(hr)) return 10;
    Handles duplicated;
    if (!DuplicateHandles(producer.ExportHandles(), duplicated)) return 11;
    hr = consumer.OpenConsumer(consumerDevice.Get(), kExtent, nb::Format::Rgba8, duplicated);
    if (FAILED(hr)) return 12;

    ComPtr<ID3D12CommandQueue> producerQueue, consumerQueue;
    if (FAILED(CreateQueue(producerDevice.Get(), producerQueue))) return 13;
    if (FAILED(CreateQueue(consumerDevice.Get(), consumerQueue))) return 14;

    std::array<ComPtr<ID3D12Resource>, 3> sourceTextures;
    if (FAILED(producer.CreateLocalTextures(sourceTextures))) return 15;
    ComPtr<ID3D12CommandAllocator> producerAllocator;
    ComPtr<ID3D12GraphicsCommandList> producerList;
    if (FAILED(CreateList(producerDevice.Get(), producerAllocator, producerList))) return 16;
    std::array<ComPtr<ID3D12Resource>, 3> uploads;
    for (size_t i = 0; i < sourceTextures.size(); ++i)
        if (FAILED(UploadTexture(producerDevice.Get(), producerList.Get(), sourceTextures[i].Get(), i, uploads[i]))) return 17;
    std::array<ID3D12Resource*, 3> sourcePtrs{
        sourceTextures[0].Get(), sourceTextures[1].Get(), sourceTextures[2].Get()};
    const std::array<D3D12_RESOURCE_STATES, 3> states{
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    if (FAILED(producer.RecordWrite(0, producerList.Get(), sourcePtrs, states))) return 18;
    if (FAILED(producerList->Close())) return 19;
    ID3D12CommandList* producerLists[]{producerList.Get()};
    producerQueue->ExecuteCommandLists(1, producerLists);
    if (FAILED(producer.SignalReady(producerQueue.Get(), 0, kSerial))) return 20;
    if (FAILED(WaitReady(consumer, 0, kSerial, false))) return 21;

    LocalInteropFrame local;
    if (FAILED(local.Create(consumerDevice.Get(), consumer.GetLayout()))) return 22;
    ComPtr<ID3D12CommandAllocator> consumerAllocator;
    ComPtr<ID3D12GraphicsCommandList> consumerList;
    if (FAILED(CreateList(consumerDevice.Get(), consumerAllocator, consumerList))) return 23;
    auto localPtrs = local.Resources();
    if (FAILED(consumer.RecordRead(0, consumerList.Get(), localPtrs, states))) return 24;
    if (FAILED(consumerList->Close())) return 25;
    ID3D12CommandList* consumerLists[]{consumerList.Get()};
    consumerQueue->ExecuteCommandLists(1, consumerLists);
    if (FAILED(consumer.SignalDone(consumerQueue.Get(), 0, kSerial))) return 26;
    if (FAILED(local.Signal(consumerQueue.Get(), kSerial))) return 27;
    if (FAILED(WaitReady(producer, 0, kSerial, true))) return 28;

    D3D_FEATURE_LEVEL level{};
    ComPtr<ID3D11Device> base11;
    ComPtr<ID3D11DeviceContext> baseContext;
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, base11.ReleaseAndGetAddressOf(), &level,
        baseContext.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return 29;
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    if (FAILED(base11.As(&device11)) || FAILED(baseContext.As(&context11))) return 30;
    std::array<ComPtr<ID3D11Texture2D>, 3> opened;
    ComPtr<ID3D11Fence> openedFence;
    hr = local.OpenD3D11(device11.Get(), opened, openedFence);
    if (FAILED(hr)) {
        std::cerr << "OpenD3D11 failed hr=0x" << std::hex << uint32_t(hr) << std::dec << "\n";
        return 31;
    }
    if (!VerifyD3D11(device11.Get(), context11.Get(), opened, openedFence.Get())) return 32;

    std::cout << "PASS d3d12-shared-heap -> local-d3d12 -> d3d11 shared textures/fence\n";
    return 0;
}
} // namespace

int main() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) return 1;
    bool foundD3D12 = false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumHr = factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf());
        if (enumHr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enumHr)) return 2;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        const int rc = RunOnAdapter(adapter.Get());
        if (rc == 77) continue;
        foundD3D12 = true;
        if (rc != 0) {
            std::wcerr << L"Adapter failed: " << desc.Description << L" stage=" << rc << L"\n";
            return rc;
        }
        return 0;
    }
    if (!foundD3D12) {
        std::cout << "SKIP no adapter supports the NativeBridge cross-adapter heap contract\n";
        return 77;
    }
    return 3;
}
