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
    uint64_t instance{};
    uint64_t session{};
    uint64_t generation{};
    uint64_t viewport{};
    Extent extent{};
    Format colorFormat{Format::Rgba8};
    bool requireDifferentAdapters{true};
};

struct ConsumerConfig {
    uint64_t instance{};
    uint64_t viewport{};
    Format colorFormat{Format::Rgba8};
    bool requireDifferentAdapters{true};
};

class ProducerSession final {
public:
    HRESULT Listen(ID3D12Device* device, const ProducerConfig& config) noexcept;
    HRESULT Accept(DWORD timeoutMs) noexcept;
    bool Connected() const noexcept { return handshaken_ && channel_.Connected(); }
    DWORD PeerPid() const noexcept { return channel_.PeerPid(); }
    const Policy& GetPolicy() const noexcept { return policy_; }
    const d3d12::Endpoint& Endpoint() const noexcept { return endpoint_; }

    HRESULT RecordWrite(uint32_t slot, ID3D12GraphicsCommandList* commandList,
        const std::array<ID3D12Resource*,3>& resources,
        const std::array<D3D12_RESOURCE_STATES,3>& states) noexcept;
    HRESULT SignalReady(ID3D12CommandQueue* queue, Token token) noexcept;
    HRESULT PollDone(Token token) const noexcept;
    HRESULT SendFrame(Token token, const Packet& packet, uint64_t nowNs, DWORD timeoutMs) noexcept;
    HRESULT ReceiveRelease(Token& token, DWORD timeoutMs) noexcept;
    HRESULT SendStop(DWORD timeoutMs) noexcept;
    void Close() noexcept;

private:
    ipc::Channel channel_;
    d3d12::Endpoint endpoint_;
    ProducerConfig config_{};
    Policy policy_{};
    bool listening_{};
    bool handshaken_{};
};

class ConsumerSession final {
public:
    HRESULT Connect(DWORD producerPid, ID3D12Device* processingDevice,
        const ConsumerConfig& config, DWORD timeoutMs) noexcept;
    bool Connected() const noexcept { return handshaken_ && channel_.Connected(); }
    DWORD PeerPid() const noexcept { return channel_.PeerPid(); }
    const Policy& GetPolicy() const noexcept { return policy_; }
    const d3d12::Endpoint& Endpoint() const noexcept { return endpoint_; }

    HRESULT ReceiveFrame(Token& token, Packet& packet, uint64_t nowNs, DWORD timeoutMs) noexcept;
    HRESULT RecordRead(uint32_t slot, ID3D12GraphicsCommandList* commandList,
        const std::array<ID3D12Resource*,3>& resources,
        const std::array<D3D12_RESOURCE_STATES,3>& states) noexcept;
    HRESULT PollReady(Token token) const noexcept;
    HRESULT SignalDone(ID3D12CommandQueue* queue, Token token) noexcept;
    HRESULT SendRelease(Token token, DWORD timeoutMs) noexcept;
    HRESULT ReceiveStop(DWORD timeoutMs) noexcept;
    void Close() noexcept;

private:
    ipc::Channel channel_;
    d3d12::Endpoint endpoint_;
    ConsumerConfig config_{};
    Policy policy_{};
    bool handshaken_{};
};

} // namespace nb::session
