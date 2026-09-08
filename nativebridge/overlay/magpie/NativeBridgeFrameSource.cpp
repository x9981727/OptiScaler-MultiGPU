#include "pch.h"
#include "NativeBridgeFrameSource.h"
#include "DeviceResources.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include <chrono>
#include <vector>

namespace Magpie {
namespace {
constexpr uint64_t kPipeInstance = 1;
constexpr DWORD kConnectTimeoutMs = 5000;
constexpr DWORD kControlTimeoutMs = 5000;
constexpr DWORD kReceivePollMs = 50;
constexpr uint64_t kReadyTimeoutNs = 500'000'000ull;

nb::AdapterId GetAdapterId(IDXGIAdapter4* adapter) noexcept {
    if (!adapter) return {};
    DXGI_ADAPTER_DESC3 desc{};
    if (FAILED(adapter->GetDesc3(&desc))) return {};
    return {desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart};
}

HANDLE ToHandle(uint64_t value) noexcept {
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
}

bool IsZeroAdapter(nb::AdapterId id) noexcept {
    return id.low == 0 && id.high == 0;
}
}

uint64_t NativeBridgeFrameSource::_NowNs() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

NativeBridgeFrameSource::~NativeBridgeFrameSource() noexcept {
    if (_receiver.joinable()) _receiver.request_stop();
    if (_sender.joinable()) _sender.request_stop();
    _releaseCv.notify_all();
    if (_frameEvent) SetEvent(_frameEvent);
    if (_receiver.joinable()) _receiver.join();
    if (_sender.joinable()) _sender.join();
    _channel.Close();
    if (_frameEvent) {
        CloseHandle(_frameEvent);
        _frameEvent = nullptr;
    }
}

void NativeBridgeFrameSource::_SetAsyncError(HRESULT hr) noexcept {
    if (SUCCEEDED(hr)) hr = E_FAIL;
    HRESULT expected = S_OK;
    _asyncError.compare_exchange_strong(expected, hr, std::memory_order_acq_rel);
    if (_frameEvent) SetEvent(_frameEvent);
    _releaseCv.notify_all();
}

bool NativeBridgeFrameSource::_Handshake() noexcept {
    const HWND sourceWindow = ScalingWindow::Get().SrcTracker().Handle();
    DWORD sourcePid = 0;
    GetWindowThreadProcessId(sourceWindow, &sourcePid);
    if (!sourcePid || sourcePid == GetCurrentProcessId()) {
        _captureErrorContext = "NativeBridge source process";
        _captureErrorCode = E_INVALIDARG;
        return false;
    }

    _processingAdapter = GetAdapterId(_deviceResources->GetGraphicsAdapter());
    if (IsZeroAdapter(_processingAdapter)) {
        _captureErrorContext = "NativeBridge processing adapter LUID";
        _captureErrorCode = E_FAIL;
        return false;
    }

    HRESULT hr = _channel.Connect(sourcePid, kPipeInstance, kConnectTimeoutMs);
    if (FAILED(hr)) {
        _captureErrorContext = "NativeBridge connect/authenticate";
        _captureErrorCode = hr;
        return false;
    }

    nb::session::Hello request{};
    request.role = nb::session::Role::ConsumerRequest;
    request.viewport = nb::session::AnyViewport;
    request.processingAdapter = _processingAdapter;
    request.colorFormat = nb::Format::Unknown;
    nb::ipc::Message message{};
    if (nb::session::MakeHelloMessage(request, message) != nb::session::Result::Ok ||
        FAILED(hr = _channel.Send(message, kControlTimeoutMs))) {
        _captureErrorContext = "NativeBridge hello request";
        _captureErrorCode = FAILED(hr) ? hr : E_INVALIDARG;
        return false;
    }

    if (FAILED(hr = _channel.Receive(message, kControlTimeoutMs))) {
        _captureErrorContext = "NativeBridge hello response";
        _captureErrorCode = hr;
        return false;
    }
    nb::session::Hello accepted{};
    if (nb::session::ParseHelloMessage(message, accepted) != nb::session::Result::Ok ||
        accepted.role != nb::session::Role::ProducerAccept ||
        accepted.processingAdapter != _processingAdapter ||
        accepted.renderAdapter == _processingAdapter ||
        !nb::valid_extent(accepted.extent) ||
        !nb::valid_color_format(accepted.colorFormat)) {
        _captureErrorContext = "NativeBridge hello validation";
        _captureErrorCode = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return false;
    }

    if (FAILED(hr = _channel.Receive(message, kControlTimeoutMs))) {
        _captureErrorContext = "NativeBridge handle receive";
        _captureErrorCode = hr;
        return false;
    }
    nb::session::HandleSet peerHandles{};
    if (nb::session::ParseHandleMessage(message, peerHandles) != nb::session::Result::Ok ||
        peerHandles.session != accepted.session ||
        peerHandles.generation != accepted.generation ||
        peerHandles.renderAdapter != accepted.renderAdapter ||
        peerHandles.processingAdapter != accepted.processingAdapter ||
        peerHandles.extent != accepted.extent ||
        peerHandles.colorFormat != accepted.colorFormat) {
        _captureErrorContext = "NativeBridge handle validation";
        _captureErrorCode = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return false;
    }

    hr = D3D12CreateDevice(
        _deviceResources->GetGraphicsAdapter(), D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(_device12.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        _captureErrorContext = "NativeBridge D3D12 device";
        _captureErrorCode = hr;
        return false;
    }

    nb::d3d12::Handles handles{};
    handles.heap = ToHandle(peerHandles.heap);
    for (size_t i = 0; i < nb::kSlotCount; ++i) {
        handles.ready[i] = ToHandle(peerHandles.ready[i]);
        handles.done[i] = ToHandle(peerHandles.done[i]);
    }
    hr = _consumer.OpenConsumer(
        _device12.Get(), accepted.extent, accepted.colorFormat, handles);
    if (FAILED(hr)) {
        _captureErrorContext = "NativeBridge open cross-adapter transport";
        _captureErrorCode = hr;
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = _device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(_queue12.ReleaseAndGetAddressOf()));
    if (SUCCEEDED(hr)) hr = _device12->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(_allocator12.ReleaseAndGetAddressOf()));
    if (SUCCEEDED(hr)) hr = _device12->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocator12.Get(), nullptr,
        IID_PPV_ARGS(_list12.ReleaseAndGetAddressOf()));
    if (SUCCEEDED(hr)) hr = _list12->Close();
    if (FAILED(hr)) {
        _captureErrorContext = "NativeBridge D3D12 command queue";
        _captureErrorCode = hr;
        return false;
    }

    hr = _local.Create(
        _deviceResources->GetD3DDevice(), _device12.Get(), _consumer.GetLayout());
    if (FAILED(hr)) {
        _captureErrorContext = "NativeBridge D3D11/D3D12 local interop";
        _captureErrorCode = hr;
        return false;
    }

    _session = accepted.session;
    _generation = accepted.generation;
    _viewport = accepted.viewport;
    _renderAdapter = accepted.renderAdapter;
    _extent = accepted.extent;
    _colorFormat = accepted.colorFormat;
    _policy = {
        .session = _session,
        .generation = _generation,
        .viewport = _viewport,
        .renderAdapter = _renderAdapter,
        .processingAdapter = _processingAdapter,
        .maxAgeNs = 100'000'000,
        .requireDifferentAdapters = true
    };
    _captureSequence = _generation;
    return true;
}

bool NativeBridgeFrameSource::_CreatePublishedTextures() noexcept {
    if (!_extent.width || !_extent.height) return false;
    const size_t pixels = size_t(_extent.width) * size_t(_extent.height);
    if (pixels > size_t(nb::kMaxDimension) * size_t(nb::kMaxDimension)) return false;
    try {
        ID3D11Device5* device = _deviceResources->GetD3DDevice();
        for (size_t i = 0; i < 3; ++i) {
            D3D11_TEXTURE2D_DESC desc{};
            _local.Textures11()[i]->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ID3D11Texture2D** target = i == 0 ? _output.put() :
                i == 1 ? _depth.put() : _motion.put();
            const HRESULT hr = device->CreateTexture2D(&desc, nullptr, target);
            if (FAILED(hr)) {
                _captureErrorContext = "NativeBridge published texture";
                _captureErrorCode = hr;
                return false;
            }
        }

        std::vector<uint8_t> confidence(pixels, 0xff);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = _extent.width;
        desc.Height = _extent.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = confidence.data();
        data.SysMemPitch = _extent.width;
        const HRESULT hr = device->CreateTexture2D(&desc, &data, _confidence.put());
        if (FAILED(hr)) {
            _captureErrorContext = "NativeBridge confidence texture";
            _captureErrorCode = hr;
            return false;
        }
        return true;
    } catch (...) {
        _captureErrorContext = "NativeBridge published texture allocation";
        _captureErrorCode = E_OUTOFMEMORY;
        return false;
    }
}

bool NativeBridgeFrameSource::_Initialize() noexcept {
    _frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!_frameEvent) {
        _captureErrorContext = "NativeBridge frame event";
        _captureErrorCode = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    if (!_Handshake() || !_CreatePublishedTextures()) return false;
    Logger::Get().Info(fmt::format(
        "NativeBridge connected: session={} generation={} viewport={} size={}x{} renderLuid={:08X}:{:08X} processingLuid={:08X}:{:08X}",
        _session, _generation, _viewport, _extent.width, _extent.height,
        uint32_t(_renderAdapter.high), _renderAdapter.low,
        uint32_t(_processingAdapter.high), _processingAdapter.low));
    return true;
}

bool NativeBridgeFrameSource::Start() noexcept {
    bool expected = false;
    if (!_started.compare_exchange_strong(expected, true)) return false;
    try {
        _receiver = std::jthread([this](std::stop_token stop) { _ReceiveLoop(stop); });
        _sender = std::jthread([this](std::stop_token stop) { _SendLoop(stop); });
        return true;
    } catch (...) {
        _SetAsyncError(E_OUTOFMEMORY);
        return false;
    }
}

void NativeBridgeFrameSource::_ReceiveLoop(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
        {
            std::lock_guard lock(_pendingMutex);
            if (_pending.size() >= nb::kSlotCount) {
                Sleep(1);
                continue;
            }
        }
        nb::ipc::Message message{};
        const HRESULT receive = _channel.Receive(message, kReceivePollMs);
        if (receive == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) continue;
        if (FAILED(receive)) {
            _SetAsyncError(receive);
            return;
        }
        if (message.kind == nb::ipc::Kind::Stop) {
            _SetAsyncError(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE));
            return;
        }
        nb::Token token{};
        nb::Packet packet{};
        if (nb::session::ParseFrameMessage(message, token, packet) != nb::session::Result::Ok) {
            _SetAsyncError(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            return;
        }

        const uint64_t waitStart = _NowNs();
        for (;;) {
            const HRESULT ready = _consumer.PollReady(token.slot, token.serial);
            if (ready == S_OK) break;
            if (ready != DXGI_ERROR_WAS_STILL_DRAWING) {
                _SetAsyncError(ready);
                return;
            }
            if (_NowNs() - waitStart >= kReadyTimeoutNs) {
                _SetAsyncError(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                return;
            }
            if (stop.stop_requested()) return;
            Sleep(1);
        }

        const nb::Error status = nb::validate(packet, _policy, _NowNs());
        const bool reset = status == nb::Error::None ? _history.accept(packet).reset : true;
        {
            std::lock_guard lock(_pendingMutex);
            if (_pending.size() >= nb::kSlotCount) {
                _SetAsyncError(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
                return;
            }
            _pending.push_back(PendingFrame{token, packet, reset, status});
        }
        SetEvent(_frameEvent);
    }
}

void NativeBridgeFrameSource::_QueueRelease(nb::Token token) noexcept {
    {
        std::lock_guard lock(_releaseMutex);
        _releases.push_back(token);
    }
    _releaseCv.notify_one();
}

void NativeBridgeFrameSource::_SendLoop(std::stop_token stop) noexcept {
    for (;;) {
        nb::Token token{};
        {
            std::unique_lock lock(_releaseMutex);
            _releaseCv.wait_for(lock, std::chrono::milliseconds(20), [&] {
                return !_releases.empty() || stop.stop_requested();
            });
            if (_releases.empty()) {
                if (stop.stop_requested()) return;
                continue;
            }
            token = _releases.front();
            _releases.pop_front();
        }
        nb::ipc::Message message{};
        if (nb::session::MakeReleaseMessage(token, message) != nb::session::Result::Ok) {
            _SetAsyncError(E_INVALIDARG);
            return;
        }
        const HRESULT hr = _channel.Send(message, kControlTimeoutMs);
        if (FAILED(hr)) {
            _SetAsyncError(hr);
            return;
        }
    }
}

bool NativeBridgeFrameSource::_RetireWithoutCopy(const PendingFrame& frame) noexcept {
    const HRESULT hr = _consumer.SignalDone(
        _queue12.Get(), frame.token.slot, frame.token.serial);
    if (FAILED(hr)) return false;
    _QueueRelease(frame.token);
    return true;
}

FrameSourceState NativeBridgeFrameSource::_Update() noexcept {
    const HRESULT asyncError = _asyncError.load(std::memory_order_acquire);
    if (FAILED(asyncError)) {
        _captureErrorContext = "NativeBridge asynchronous session";
        _captureErrorCode = asyncError;
        _captureInterrupted = true;
        return FrameSourceState::Error;
    }

    PendingFrame frame{};
    {
        std::lock_guard lock(_pendingMutex);
        if (_pending.empty()) return FrameSourceState::Waiting;
        frame = _pending.front();
        _pending.pop_front();
        if (!_pending.empty()) SetEvent(_frameEvent);
    }

    if (frame.status != nb::Error::None) {
        if (!_RetireWithoutCopy(frame)) {
            _captureErrorContext = "NativeBridge reject retire";
            _captureErrorCode = E_FAIL;
            return FrameSourceState::Error;
        }
        if (frame.status == nb::Error::TooOld) return FrameSourceState::Waiting;
        _captureErrorContext = "NativeBridge packet validation";
        _captureErrorCode = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        Logger::Get().Error(fmt::format(
            "NativeBridge packet rejected: {}", nb::error_name(frame.status)));
        return FrameSourceState::Error;
    }

    HRESULT hr = S_OK;
    if (_lastLocalSerial) hr = _local.WaitConsumed(_queue12.Get(), _lastLocalSerial);
    if (SUCCEEDED(hr)) hr = _allocator12->Reset();
    if (SUCCEEDED(hr)) hr = _list12->Reset(_allocator12.Get(), nullptr);
    const std::array<D3D12_RESOURCE_STATES, 3> states{
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COMMON
    };
    auto resources = _local.Resources12();
    if (SUCCEEDED(hr)) hr = _consumer.RecordRead(
        frame.token.slot, _list12.Get(), resources, states);
    if (SUCCEEDED(hr)) hr = _list12->Close();
    if (FAILED(hr)) {
        _captureErrorContext = "NativeBridge consumer copy record";
        _captureErrorCode = hr;
        return FrameSourceState::Error;
    }

    ID3D12CommandList* lists[]{_list12.Get()};
    _queue12->ExecuteCommandLists(1, lists);
    hr = _consumer.SignalDone(_queue12.Get(), frame.token.slot, frame.token.serial);
    if (SUCCEEDED(hr)) hr = _local.SignalReady(_queue12.Get(), frame.token.serial);
    ID3D11DeviceContext4* d3dDC = _deviceResources->GetD3DDC();
    if (SUCCEEDED(hr)) hr = d3dDC->Wait(_local.ReadyFence11(), frame.token.serial);
    if (SUCCEEDED(hr)) {
        d3dDC->CopyResource(_output.get(), _local.Textures11()[0].Get());
        d3dDC->CopyResource(_depth.get(), _local.Textures11()[1].Get());
        d3dDC->CopyResource(_motion.get(), _local.Textures11()[2].Get());
        hr = d3dDC->Signal(_local.ConsumedFence11(), frame.token.serial);
        d3dDC->Flush();
    }
    if (FAILED(hr)) {
        _captureErrorContext = "NativeBridge consumer synchronization";
        _captureErrorCode = hr;
        return FrameSourceState::Error;
    }

    _lastLocalSerial = frame.token.serial;
    _currentPacket = frame.packet;
    _currentSerial = frame.token.serial;
    _currentReset = frame.historyReset;
    _currentValid = true;
    _captureInterrupted = frame.historyReset && _currentPacket.previousFrame != 0;
    _captureTimestamp100ns = static_cast<int64_t>(_currentPacket.timestampNs / 100);
    _captureErrorCode = S_OK;
    _QueueRelease(frame.token);
    return FrameSourceState::NewFrame;
}

bool NativeBridgeFrameSource::GetNativeGuidance(
    FrameGuidanceFrameId frameId,
    FrameGuidanceView& output) const noexcept {
    if (!_currentValid || !_depth || !_motion || !_confidence || !frameId) return false;
    const FrameGuidanceExtent extent{_extent.width, _extent.height};
    const FrameGuidanceRegion region = FrameGuidanceRegion::Full(extent);

    auto fill = [&](FrameGuidanceResource& resource, ID3D11Texture2D* texture,
                    DXGI_FORMAT format, bool native) {
        resource.texture = texture;
        resource.format = format;
        resource.metadata.frameId = frameId;
        resource.metadata.sourceExtent = extent;
        resource.metadata.validRegion = region;
        resource.metadata.sync = {};
        resource.metadata.resetReason = _currentReset ?
            FrameGuidanceResetReason::CaptureInterrupted : FrameGuidanceResetReason::None;
        resource.metadata.valid = true;
        resource.metadata.isZero = false;
        resource.metadata.requiresHistoryReset = _currentReset;
        resource.metadata.nativeKey = _currentPacket.key;
        resource.metadata.nativeResource = native;
    };

    FrameGuidanceView view{};
    fill(view.depth, _depth.get(), DXGI_FORMAT_R32_FLOAT, true);
    fill(view.motion, _motion.get(), DXGI_FORMAT_R16G16_FLOAT, true);
    fill(view.confidence, _confidence.get(), DXGI_FORMAT_R8_UNORM, false);
    view.motionDirection = FrameGuidanceMotionDirection::CurrentToPrevious;
    view.motionUnit = FrameGuidanceMotionUnit::SourcePixels;
    view.requiresHistoryReset = _currentReset;
    view.nativeRequired = true;
    view.nativeColorKey = _currentPacket.key;
    view.nativeCamera = _currentPacket.camera;
    if (!view.IsNativeFor(frameId, extent)) return false;
    output = view;
    return true;
}

} // namespace Magpie