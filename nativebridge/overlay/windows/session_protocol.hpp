// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "ipc_pipe.hpp"
#include "transport_d3d12.hpp"
#include "native_bridge/wire.hpp"

namespace nb::session {
inline constexpr uint32_t HelloMagic = 0x3248424e; // NBH2
inline constexpr uint32_t HandleMagic = 0x324d424e; // NBM2
inline constexpr uint32_t ProtocolVersion = 1;
inline constexpr size_t HelloBytes = 80;
inline constexpr size_t HandleBytes = 112;

enum class Role : uint32_t { ConsumerRequest = 1, ProducerAccept = 2 };
struct Hello {
    Role role{};
    uint32_t flags{};
    uint64_t session{}, generation{}, viewport{};
    AdapterId renderAdapter{}, processingAdapter{};
    Extent extent{};
    Format colorFormat{Format::Rgba8};
};
struct HandleSet {
    uint64_t session{}, generation{};
    AdapterId renderAdapter{}, processingAdapter{};
    Extent extent{};
    Format colorFormat{Format::Rgba8};
    uint64_t heap{};
    std::array<uint64_t,kSlotCount> ready{}, done{};
};

enum class Result { Ok, Kind, Length, Header, Reserved, Values, Wire };
Result MakeHelloMessage(const Hello&, ipc::Message&) noexcept;
Result ParseHelloMessage(const ipc::Message&, Hello&) noexcept;
Result MakeHandleMessage(const HandleSet&, ipc::Message&) noexcept;
Result ParseHandleMessage(const ipc::Message&, HandleSet&) noexcept;
Result MakeFrameMessage(Token, const Packet&, ipc::Message&) noexcept;
Result ParseFrameMessage(const ipc::Message&, Token&, Packet&) noexcept;
Result MakeReleaseMessage(Token, ipc::Message&) noexcept;
Result ParseReleaseMessage(const ipc::Message&, Token&) noexcept;
}
