// SPDX-License-Identifier: MIT
#include "runtime_session.hpp"
#include <cstdint>
#include <vector>

namespace nb::session {
namespace {
HRESULT InvalidData() noexcept { return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
HRESULT NotSupported() noexcept { return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); }
bool ValidProducerConfig(const ProducerConfig& c) noexcept {
    return c.instance && c.session && c.generation && c.viewport != AnyViewport &&
        valid_extent(c.extent) && valid_color_format(c.colorFormat);
}
bool ValidConsumerConfig(const ConsumerConfig& c) noexcept {
    return c.instance && (c.colorFormat == Format::Unknown || valid_color_format(c.colorFormat));
}
void RevokeAll(const ipc::Channel& channel, const std::vector<HANDLE>& remote) noexcept {
    for (HANDLE h : remote) if (h) (void)channel.RevokeUnsent(h);
}
HRESULT DuplicateEndpointHandles(const ipc::Channel& channel, const d3d12::Handles& source,
    HandleSet& out, std::vector<HANDLE>& remote) noexcept {
    auto duplicate = [&](HANDLE local, uint64_t& encoded) -> HRESULT {
        HANDLE peer = nullptr; const HRESULT hr = channel.DuplicateToPeer(local, peer);
        if (FAILED(hr)) return hr; remote.push_back(peer);
        encoded = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(peer)); return S_OK;
    };
    HRESULT hr = duplicate(source.heap, out.heap); if (FAILED(hr)) return hr;
    for (size_t i = 0; i < kSlotCount; ++i) {
        hr = duplicate(source.ready[i], out.ready[i]); if (FAILED(hr)) return hr;
        hr = duplicate(source.done[i], out.done[i]); if (FAILED(hr)) return hr;
    }
    return S_OK;
}
HANDLE DecodeHandle(uint64_t value) noexcept { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)); }
} // namespace

HRESULT ProducerSession::Listen(ID3D12Device* device, const ProducerConfig& config) noexcept {
    if (!device || listening_ || handshaken_ || !ValidProducerConfig(config)) return E_INVALIDARG;
    const HRESULT transport = endpoint_.CreateProducer(device, config.extent, config.colorFormat);
    if (FAILED(transport)) return transport;
    const HRESULT pipe = channel_.Listen(config.instance); if (FAILED(pipe)) return pipe;
    config_ = config; listening_ = true; return S_OK;
}
HRESULT ProducerSession::Accept(DWORD timeoutMs) noexcept {
    if (!listening_ || handshaken_ || !timeoutMs || timeoutMs == INFINITE) return E_INVALIDARG;
    HRESULT hr = channel_.Accept(timeoutMs); if (FAILED(hr)) return hr;
    ipc::Message requestMessage{}; hr = channel_.Receive(requestMessage, timeoutMs); if (FAILED(hr)) return hr;
    Hello request{};
    if (ParseHelloMessage(requestMessage, request) != Result::Ok || request.role != Role::ConsumerRequest) return InvalidData();
    if ((request.viewport != AnyViewport && request.viewport != config_.viewport) ||
        (request.colorFormat != Format::Unknown && request.colorFormat != config_.colorFormat)) return InvalidData();
    const AdapterId renderAdapter = endpoint_.Adapter();
    if (config_.requireDifferentAdapters && request.processingAdapter == renderAdapter) return NotSupported();
    Hello accept{}; accept.role = Role::ProducerAccept; accept.session = config_.session; accept.generation = config_.generation;
    accept.viewport = config_.viewport; accept.renderAdapter = renderAdapter; accept.processingAdapter = request.processingAdapter;
    accept.extent = config_.extent; accept.colorFormat = config_.colorFormat;
    HandleSet handles{}; handles.session=accept.session; handles.generation=accept.generation;
    handles.renderAdapter=accept.renderAdapter; handles.processingAdapter=accept.processingAdapter;
    handles.extent=accept.extent; handles.colorFormat=accept.colorFormat;
    std::vector<HANDLE> duplicated; duplicated.reserve(1 + 2 * kSlotCount);
    hr = DuplicateEndpointHandles(channel_, endpoint_.ExportHandles(), handles, duplicated);
    if (FAILED(hr)) { RevokeAll(channel_, duplicated); return hr; }
    ipc::Message acceptMessage{}, handlesMessage{};
    if (MakeHelloMessage(accept, acceptMessage) != Result::Ok || MakeHandleMessage(handles, handlesMessage) != Result::Ok) {
        RevokeAll(channel_, duplicated); return E_UNEXPECTED;
    }
    hr = channel_.Send(acceptMessage, timeoutMs); if (SUCCEEDED(hr)) hr = channel_.Send(handlesMessage, timeoutMs);
    if (FAILED(hr)) { RevokeAll(channel_, duplicated); return hr; }
    policy_.session=accept.session; policy_.generation=accept.generation; policy_.viewport=accept.viewport;
    policy_.renderAdapter=accept.renderAdapter; policy_.processingAdapter=accept.processingAdapter;
    policy_.requireDifferentAdapters=config_.requireDifferentAdapters; handshaken_=true; return S_OK;
}
HRESULT ProducerSession::RecordWrite(uint32_t slot, ID3D12GraphicsCommandList* commandList,
    const std::array<ID3D12Resource*,3>& resources,const std::array<D3D12_RESOURCE_STATES,3>& states) noexcept {
    if (!Connected()) return E_UNEXPECTED; return endpoint_.RecordWrite(slot, commandList, resources, states);
}
HRESULT ProducerSession::RecordWritePlane(uint32_t slot,uint32_t plane,ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* resource,D3D12_RESOURCE_STATES state) noexcept {
    if (!Connected()) return E_UNEXPECTED; return endpoint_.RecordWritePlane(slot,plane,commandList,resource,state);
}
HRESULT ProducerSession::SignalReady(ID3D12CommandQueue* queue, Token token) noexcept {
    if (!Connected() || token.slot >= kSlotCount || !token.serial) return E_INVALIDARG;
    return endpoint_.SignalReady(queue, token.slot, token.serial);
}
HRESULT ProducerSession::PollDone(Token token) const noexcept {
    if (!Connected() || token.slot >= kSlotCount || !token.serial) return E_INVALIDARG;
    return endpoint_.PollDone(token.slot, token.serial);
}
HRESULT ProducerSession::SendFrame(Token token, const Packet& packet, uint64_t nowNs, DWORD timeoutMs) noexcept {
    if (!Connected() || !timeoutMs || timeoutMs == INFINITE || token.slot >= kSlotCount || !token.serial) return E_INVALIDARG;
    if (validate(packet, policy_, nowNs) != Error::None) return InvalidData();
    ipc::Message message{}; if (MakeFrameMessage(token, packet, message) != Result::Ok) return InvalidData();
    return channel_.Send(message, timeoutMs);
}
HRESULT ProducerSession::ReceiveRelease(Token& token, DWORD timeoutMs) noexcept {
    if (!Connected() || !timeoutMs || timeoutMs == INFINITE) return E_INVALIDARG;
    ipc::Message message{}; const HRESULT hr = channel_.Receive(message, timeoutMs); if (FAILED(hr)) return hr;
    return ParseReleaseMessage(message, token) == Result::Ok ? S_OK : InvalidData();
}
HRESULT ProducerSession::SendStop(DWORD timeoutMs) noexcept {
    if (!Connected() || !timeoutMs || timeoutMs == INFINITE) return E_INVALIDARG;
    ipc::Message message{}; message.kind = ipc::Kind::Stop; return channel_.Send(message, timeoutMs);
}
void ProducerSession::Close() noexcept { channel_.Close(); listening_=false; handshaken_=false; }

HRESULT ConsumerSession::Connect(DWORD producerPid, ID3D12Device* processingDevice,
    const ConsumerConfig& config, DWORD timeoutMs) noexcept {
    if (!producerPid || !processingDevice || handshaken_ || !ValidConsumerConfig(config) || !timeoutMs || timeoutMs == INFINITE)
        return E_INVALIDARG;
    HRESULT hr = channel_.Connect(producerPid, config.instance, timeoutMs); if (FAILED(hr)) return hr;
    const LUID luid = processingDevice->GetAdapterLuid(); const AdapterId processingAdapter{luid.LowPart, luid.HighPart};
    Hello request{}; request.role=Role::ConsumerRequest; request.viewport=config.viewport;
    request.processingAdapter=processingAdapter; request.colorFormat=config.colorFormat;
    ipc::Message requestMessage{}; if (MakeHelloMessage(request, requestMessage) != Result::Ok) return E_UNEXPECTED;
    hr = channel_.Send(requestMessage, timeoutMs); if (FAILED(hr)) return hr;
    ipc::Message acceptMessage{}, handlesMessage{}; hr=channel_.Receive(acceptMessage,timeoutMs);
    if (SUCCEEDED(hr)) hr=channel_.Receive(handlesMessage,timeoutMs); if (FAILED(hr)) return hr;
    Hello accept{}; HandleSet set{};
    if (ParseHelloMessage(acceptMessage,accept)!=Result::Ok || accept.role!=Role::ProducerAccept ||
        ParseHandleMessage(handlesMessage,set)!=Result::Ok) return InvalidData();
    if ((config.viewport != AnyViewport && accept.viewport != config.viewport) ||
        (config.colorFormat != Format::Unknown && accept.colorFormat != config.colorFormat) ||
        accept.processingAdapter!=processingAdapter ||
        set.session!=accept.session || set.generation!=accept.generation || set.renderAdapter!=accept.renderAdapter ||
        set.processingAdapter!=accept.processingAdapter || set.extent!=accept.extent || set.colorFormat!=accept.colorFormat)
        return InvalidData();
    if (config.requireDifferentAdapters && accept.renderAdapter==processingAdapter) return NotSupported();
    d3d12::Handles local{}; local.heap=DecodeHandle(set.heap);
    for(size_t i=0;i<kSlotCount;++i){local.ready[i]=DecodeHandle(set.ready[i]);local.done[i]=DecodeHandle(set.done[i]);}
    hr=endpoint_.OpenConsumer(processingDevice,accept.extent,accept.colorFormat,local); if(FAILED(hr)) return hr;
    config_=config; policy_.session=accept.session; policy_.generation=accept.generation; policy_.viewport=accept.viewport;
    policy_.renderAdapter=accept.renderAdapter; policy_.processingAdapter=accept.processingAdapter;
    policy_.requireDifferentAdapters=config.requireDifferentAdapters; handshaken_=true; return S_OK;
}
HRESULT ConsumerSession::ReceiveFrame(Token& token, Packet& packet, uint64_t nowNs, DWORD timeoutMs) noexcept {
    if (!Connected() || !timeoutMs || timeoutMs == INFINITE) return E_INVALIDARG;
    ipc::Message message{}; const HRESULT hr=channel_.Receive(message,timeoutMs); if(FAILED(hr)) return hr;
    Packet parsed{}; Token parsedToken{}; if(ParseFrameMessage(message,parsedToken,parsed)!=Result::Ok) return InvalidData();
    if(validate(parsed,policy_,nowNs)!=Error::None) return InvalidData(); token=parsedToken; packet=parsed; return S_OK;
}
HRESULT ConsumerSession::RecordRead(uint32_t slot, ID3D12GraphicsCommandList* commandList,
    const std::array<ID3D12Resource*,3>& resources,const std::array<D3D12_RESOURCE_STATES,3>& states) noexcept {
    if(!Connected()) return E_UNEXPECTED; return endpoint_.RecordRead(slot,commandList,resources,states);
}
HRESULT ConsumerSession::RecordReadPlane(uint32_t slot,uint32_t plane,ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* resource,D3D12_RESOURCE_STATES state) noexcept {
    if(!Connected()) return E_UNEXPECTED; return endpoint_.RecordReadPlane(slot,plane,commandList,resource,state);
}
HRESULT ConsumerSession::PollReady(Token token) const noexcept {
    if(!Connected()||token.slot>=kSlotCount||!token.serial)return E_INVALIDARG;return endpoint_.PollReady(token.slot,token.serial);
}
HRESULT ConsumerSession::SignalDone(ID3D12CommandQueue* queue, Token token) noexcept {
    if(!Connected()||token.slot>=kSlotCount||!token.serial)return E_INVALIDARG;return endpoint_.SignalDone(queue,token.slot,token.serial);
}
HRESULT ConsumerSession::SendRelease(Token token, DWORD timeoutMs) noexcept {
    if(!Connected()||!timeoutMs||timeoutMs==INFINITE||token.slot>=kSlotCount||!token.serial)return E_INVALIDARG;
    ipc::Message message{};if(MakeReleaseMessage(token,message)!=Result::Ok)return InvalidData();return channel_.Send(message,timeoutMs);
}
HRESULT ConsumerSession::ReceiveStop(DWORD timeoutMs) noexcept {
    if(!Connected()||!timeoutMs||timeoutMs==INFINITE)return E_INVALIDARG;
    ipc::Message message{};const HRESULT hr=channel_.Receive(message,timeoutMs);if(FAILED(hr))return hr;
    return message.kind==ipc::Kind::Stop&&message.length==0?S_OK:InvalidData();
}
void ConsumerSession::Close() noexcept { channel_.Close(); handshaken_=false; }

} // namespace nb::session