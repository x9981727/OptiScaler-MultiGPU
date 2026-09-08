#pragma once
#include "FrameSourceBase.h"
#include "FrameGuidanceTypes.h"
#include "native_bridge/contract.hpp"
#include "native_bridge/ledger.hpp"
#include "native_bridge/windows/ipc_pipe.hpp"
#include "native_bridge/windows/local_interop.hpp"
#include "native_bridge/windows/session_protocol.hpp"
#include "native_bridge/windows/transport_d3d12.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Magpie {

class NativeBridgeFrameSource final : public FrameSourceBase {
public:
    NativeBridgeFrameSource() noexcept = default;
    ~NativeBridgeFrameSource() noexcept override;

    bool Start() noexcept override;
    const char* Name() const noexcept override { return "NativeBridge v2"; }
    FrameSourceWaitType WaitType() const noexcept override { return FrameSourceWaitType::WaitForEvent; }
    HANDLE FrameArrivedEvent() const noexcept override { return _frameEvent; }
    bool UsesNativeGuidance() const noexcept override { return true; }
    bool GetNativeGuidance(FrameGuidanceFrameId frameId, FrameGuidanceView& output) const noexcept override;

protected:
    bool _Initialize() noexcept override;
    FrameSourceState _Update() noexcept override;

private:
    struct PendingFrame {
        nb::Token token{};
        nb::Packet packet{};
        bool historyReset{};
        nb::Error status{nb::Error::None};
    };

    bool _Handshake() noexcept;
    bool _CreatePublishedTextures() noexcept;
    void _ReceiveLoop(std::stop_token stop) noexcept;
    void _SendLoop(std::stop_token stop) noexcept;
    void _SetAsyncError(HRESULT hr) noexcept;
    void _QueueRelease(nb::Token token) noexcept;
    bool _RetireWithoutCopy(const PendingFrame& frame) noexcept;
    static uint64_t _NowNs() noexcept;

    nb::ipc::Channel _channel;
    nb::d3d12::Endpoint _consumer;
    nb::d3d12::LocalInteropFrame _local;
    Microsoft::WRL::ComPtr<ID3D12Device> _device12;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> _queue12;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _allocator12;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _list12;
    winrt::com_ptr<ID3D11Texture2D> _depth;
    winrt::com_ptr<ID3D11Texture2D> _motion;
    winrt::com_ptr<ID3D11Texture2D> _confidence;
    HANDLE _frameEvent{};

    std::mutex _pendingMutex;
    std::deque<PendingFrame> _pending;
    std::mutex _releaseMutex;
    std::condition_variable _releaseCv;
    std::deque<nb::Token> _releases;
    std::jthread _receiver;
    std::jthread _sender;
    std::atomic<HRESULT> _asyncError{S_OK};
    std::atomic<bool> _started{false};

    nb::Policy _policy{};
    nb::History _history;
    uint64_t _session{};
    uint64_t _generation{};
    uint64_t _viewport{};
    nb::AdapterId _renderAdapter{};
    nb::AdapterId _processingAdapter{};
    nb::Extent _extent{};
    nb::Format _colorFormat{nb::Format::Rgba8};
    uint64_t _lastLocalSerial{};

    nb::Packet _currentPacket{};
    uint64_t _currentSerial{};
    bool _currentReset{};
    bool _currentValid{};
};

} // namespace Magpie
