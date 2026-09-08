// SPDX-License-Identifier: MIT
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <cstdint>
#include "ipc_pipe.hpp"
#include "session_protocol.hpp"
#include "transport_d3d12.hpp"

namespace nb::session {
struct ProducerConfig {
    uint64_t instance{}, session{}, generation{}, viewport{};
    Extent extent{}; Format colorFormat{Format::Rgba8};
    bool requireDifferentAdapters{true};
};
struct ConsumerConfig {
    uint64_t instance{}, viewport{AnyViewport}; Format colorFormat{Format::Rgba8};
    bool requireDifferentAdapters{true};
};
class ProducerSession final {
public:
    HRESULT Listen(ID3D12Device*, const ProducerConfig&) noexcept;
    HRESULT Accept(DWORD timeoutMs) noexcept;
    bool Connected() const noexcept { return handshaken_ && channel_.Connected(); }
    DWORD PeerPid() const noexcept { return channel_.PeerPid(); }
    const Policy& GetPolicy() const noexcept { return policy_; }
    const d3d12::Endpoint& Endpoint() const noexcept { return endpoint_; }
    d3d12::Endpoint& Transport() noexcept { return endpoint_; }
    HRESULT RecordWrite(uint32_t, ID3D12GraphicsCommandList*, const std::array<ID3D12Resource*,3>&,
        const std::array<D3D12_RESOURCE_STATES,3>&) noexcept;
    HRESULT RecordWritePlane(uint32_t,uint32_t,ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES) noexcept;
    HRESULT SignalReady(ID3D12CommandQueue*, Token) noexcept;
    HRESULT PollDone(Token) const noexcept;
    HRESULT SendFrame(Token,const Packet&,uint64_t nowNs,DWORD timeoutMs) noexcept;
    HRESULT ReceiveRelease(Token&,DWORD timeoutMs) noexcept;
    HRESULT SendStop(DWORD timeoutMs) noexcept;
    void Close() noexcept;
private:
    ipc::Channel channel_; d3d12::Endpoint endpoint_; ProducerConfig config_{}; Policy policy_{};
    bool listening_{}, handshaken_{};
};
class ConsumerSession final {
public:
    HRESULT Connect(DWORD producerPid, ID3D12Device* processingDevice,const ConsumerConfig&,DWORD timeoutMs) noexcept;
    bool Connected() const noexcept { return handshaken_ && channel_.Connected(); }
    DWORD PeerPid() const noexcept { return channel_.PeerPid(); }
    const Policy& GetPolicy() const noexcept { return policy_; }
    const d3d12::Endpoint& Endpoint() const noexcept { return endpoint_; }
    d3d12::Endpoint& Transport() noexcept { return endpoint_; }
    HRESULT ReceiveFrame(Token&,Packet&,uint64_t nowNs,DWORD timeoutMs) noexcept;
    HRESULT RecordRead(uint32_t,ID3D12GraphicsCommandList*,const std::array<ID3D12Resource*,3>&,
        const std::array<D3D12_RESOURCE_STATES,3>&) noexcept;
    HRESULT RecordReadPlane(uint32_t,uint32_t,ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES) noexcept;
    HRESULT PollReady(Token) const noexcept;
    HRESULT SignalDone(ID3D12CommandQueue*,Token) noexcept;
    HRESULT SendRelease(Token,DWORD timeoutMs) noexcept;
    HRESULT ReceiveStop(DWORD timeoutMs) noexcept;
    void Close() noexcept;
private:
    ipc::Channel channel_; d3d12::Endpoint endpoint_; ConsumerConfig config_{}; Policy policy_{};
    bool handshaken_{};
};
} // namespace nb::session
