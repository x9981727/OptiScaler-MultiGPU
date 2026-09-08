// SPDX-License-Identifier: MIT
#include "session_protocol.hpp"
#include <algorithm>
#include <bit>
#include <cstring>

namespace nb::session {
namespace {
struct Writer {
    std::byte* p;
    void u32(uint32_t v){for(unsigned i=0;i<4;++i)*p++=std::byte((v>>(8*i))&255);}
    void u64(uint64_t v){u32(uint32_t(v));u32(uint32_t(v>>32));}
    void adapter(AdapterId a){u32(a.low);u32(std::bit_cast<uint32_t>(a.high));}
};
struct Reader {
    const std::byte* p;
    uint32_t u32(){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(std::to_integer<unsigned>(*p++))<<(8*i);return v;}
    uint64_t u64(){const auto lo=u32();return uint64_t(lo)|(uint64_t(u32())<<32);}
    AdapterId adapter(){AdapterId a;a.low=u32();a.high=std::bit_cast<int32_t>(u32());return a;}
};
bool validAdapter(AdapterId a){return a.low!=0 || a.high!=0;}
bool allZero(const std::byte* begin,const std::byte* end){return std::all_of(begin,end,[](std::byte b){return b==std::byte{};});}
}
Result MakeHelloMessage(const Hello& h, ipc::Message& out) noexcept {
    if((h.role!=Role::ConsumerRequest && h.role!=Role::ProducerAccept) || !validAdapter(h.processingAdapter))
        return Result::Values;
    if(h.role==Role::ConsumerRequest) {
        if(h.session || h.generation || validAdapter(h.renderAdapter) ||
           (h.colorFormat!=Format::Unknown && !valid_color_format(h.colorFormat))) return Result::Values;
    } else if(!h.session || !h.generation || !validAdapter(h.renderAdapter) || !valid_extent(h.extent) ||
              h.viewport==AnyViewport || !valid_color_format(h.colorFormat))
        return Result::Values;
    ipc::Message m{};m.kind=ipc::Kind::Hello;m.length=uint32_t(HelloBytes);Writer w{m.payload.data()};
    w.u32(HelloMagic);w.u32(ProtocolVersion);w.u32(uint32_t(h.role));w.u32(h.flags);
    w.u64(h.session);w.u64(h.generation);w.u64(h.viewport);w.adapter(h.renderAdapter);w.adapter(h.processingAdapter);
    w.u32(h.extent.width);w.u32(h.extent.height);w.u32(uint32_t(h.colorFormat));w.u32(kSlotCount);out=m;return Result::Ok;
}
Result ParseHelloMessage(const ipc::Message& m, Hello& out) noexcept {
    if(m.kind!=ipc::Kind::Hello)return Result::Kind;if(m.length!=HelloBytes)return Result::Length;
    Reader r{m.payload.data()};if(r.u32()!=HelloMagic||r.u32()!=ProtocolVersion)return Result::Header;
    Hello h;h.role=Role(r.u32());h.flags=r.u32();h.session=r.u64();h.generation=r.u64();h.viewport=r.u64();
    h.renderAdapter=r.adapter();h.processingAdapter=r.adapter();h.extent={r.u32(),r.u32()};h.colorFormat=Format(r.u32());
    if(r.u32()!=kSlotCount)return Result::Header;
    if(!allZero(m.payload.data()+72,m.payload.data()+HelloBytes))return Result::Reserved;
    ipc::Message encoded;if(MakeHelloMessage(h,encoded)!=Result::Ok)return Result::Values;out=h;return Result::Ok;
}
Result MakeHandleMessage(const HandleSet& h, ipc::Message& out) noexcept {
    if(!h.session||!h.generation||!validAdapter(h.renderAdapter)||!validAdapter(h.processingAdapter)||
       !valid_extent(h.extent)||!valid_color_format(h.colorFormat)||!h.heap)return Result::Values;
    for(size_t i=0;i<kSlotCount;++i)if(!h.ready[i]||!h.done[i])return Result::Values;
    ipc::Message m{};m.kind=ipc::Kind::Handles;m.length=uint32_t(HandleBytes);Writer w{m.payload.data()};
    w.u32(HandleMagic);w.u32(ProtocolVersion);w.u64(h.session);w.u64(h.generation);
    w.adapter(h.renderAdapter);w.adapter(h.processingAdapter);w.u32(h.extent.width);w.u32(h.extent.height);
    w.u32(uint32_t(h.colorFormat));w.u32(kSlotCount);w.u64(h.heap);
    for(auto v:h.ready)w.u64(v);for(auto v:h.done)w.u64(v);out=m;return Result::Ok;
}
Result ParseHandleMessage(const ipc::Message& m, HandleSet& out) noexcept {
    if(m.kind!=ipc::Kind::Handles)return Result::Kind;if(m.length!=HandleBytes)return Result::Length;
    Reader r{m.payload.data()};if(r.u32()!=HandleMagic||r.u32()!=ProtocolVersion)return Result::Header;
    HandleSet h;h.session=r.u64();h.generation=r.u64();h.renderAdapter=r.adapter();h.processingAdapter=r.adapter();
    h.extent={r.u32(),r.u32()};h.colorFormat=Format(r.u32());if(r.u32()!=kSlotCount)return Result::Header;h.heap=r.u64();
    for(auto& v:h.ready)v=r.u64();for(auto& v:h.done)v=r.u64();
    ipc::Message encoded;if(MakeHandleMessage(h,encoded)!=Result::Ok)return Result::Values;out=h;return Result::Ok;
}
Result MakeFrameMessage(Token t,const Packet& p,ipc::Message& out) noexcept {
    if(t.slot>=kSlotCount||!t.serial)return Result::Values;wire::Bytes bytes;if(!wire::encode(p,bytes))return Result::Wire;
    ipc::Message m{};m.kind=ipc::Kind::Frame;m.sequence=t.serial;m.slot=t.slot;m.length=uint32_t(bytes.size());
    std::copy(bytes.begin(),bytes.end(),m.payload.begin());out=m;return Result::Ok;
}
Result ParseFrameMessage(const ipc::Message& m,Token& token,Packet& packet) noexcept {
    if(m.kind!=ipc::Kind::Frame)return Result::Kind;if(m.length!=wire::PacketBytes)return Result::Length;
    if(m.slot>=kSlotCount||!m.sequence)return Result::Values;Packet p;
    if(wire::decode(std::span(m.payload).first(m.length),p)!=wire::Result::Ok)return Result::Wire;
    token={m.slot,m.sequence};packet=p;return Result::Ok;
}
Result MakeReleaseMessage(Token t,ipc::Message& out) noexcept {
    if(t.slot>=kSlotCount||!t.serial)return Result::Values;ipc::Message m{};m.kind=ipc::Kind::Release;m.sequence=t.serial;m.slot=t.slot;out=m;return Result::Ok;
}
Result ParseReleaseMessage(const ipc::Message& m,Token& out) noexcept {
    if(m.kind!=ipc::Kind::Release)return Result::Kind;if(m.length)return Result::Length;if(m.slot>=kSlotCount||!m.sequence)return Result::Values;
    out={m.slot,m.sequence};return Result::Ok;
}
}