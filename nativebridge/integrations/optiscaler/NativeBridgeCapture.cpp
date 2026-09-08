// SPDX-License-Identifier: MIT
#include "pch.h"
#include "NativeBridgeCapture.h"
#include "State.h"
#include "native_bridge/contract.hpp"
#include "native_bridge/windows/runtime_session.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

namespace NativeBridgeCapture {
namespace {
constexpr uint64_t kPipeInstance = 1;
constexpr DWORD kAcceptPollMs = 250;
constexpr DWORD kControlPollMs = 5;
constexpr DWORD kControlSendMs = 100;
constexpr uint8_t kAllPlanes = 0x7;
constexpr size_t kCameraCache = 8;

uint64_t NowNs() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t NewSessionId() noexcept {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    uint64_t value = (static_cast<uint64_t>(qpc.QuadPart) << 17) ^
        (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^ NowNs();
    return value ? value : 1;
}

void CopyMatrix(nb::Matrix& dst, const sl::float4x4& src) noexcept {
    static_assert(sizeof(dst.m) == sizeof(src));
    std::memcpy(dst.m, &src, sizeof(dst.m));
}

bool SlBoolValid(sl::Boolean value) noexcept {
    return value == sl::Boolean::eFalse || value == sl::Boolean::eTrue;
}

bool Reasonable(float value) noexcept {
    return std::isfinite(value) && std::abs(value) < 1.0e20f;
}

nb::Format ColorFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return nb::Format::Rgba8;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return nb::Format::Bgra8;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return nb::Format::Rgba16F;
    default: return nb::Format::Unknown;
    }
}

struct CameraSample {
    uint64_t frame{};
    uint64_t viewport{};
    nb::Camera camera{};
    float motionScale[2]{};
    bool valid{};
};

struct FrameSlot {
    bool active{};
    bool signaled{};
    uint64_t frame{};
    uint64_t viewport{};
    nb::Token token{};
    uint8_t planes{};
};

struct Outbound {
    nb::Token token{};
    nb::Packet packet{};
};

struct ProducerState {
    std::mutex renderMutex;
    std::array<FrameSlot, nb::kSlotCount> slots{};
    std::array<CameraSample, kCameraCache> cameras{};
    size_t nextCamera{};
    uint64_t nextSerial{1};
    uint64_t lastPublishedFrame{};
    bool haveLastPublished{};

    std::mutex outboundMutex;
    std::condition_variable outboundCv;
    std::deque<Outbound> outbound;

    nb::session::ProducerSession session;
    nb::Policy policy{};
    nb::Extent extent{};
    nb::Format colorFormat{nb::Format::Unknown};
    uint64_t viewport{};
    uint64_t generation{1};

    std::jthread control;
    std::atomic<bool> listening{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> published{};
    std::atomic<uint64_t> rejected{};

    ~ProducerState() {
        if (control.joinable()) {
            control.request_stop();
            outboundCv.notify_all();
            control.join();
        }
        session.Close();
    }
};

ProducerState& S() {
    static ProducerState state;
    return state;
}

bool EnvEnabled() noexcept {
    static const bool enabled = [] {
        wchar_t value[16]{};
        const DWORD n = GetEnvironmentVariableW(L"MAGPIE_NATIVE_BRIDGE", value, static_cast<DWORD>(std::size(value)));
        if (!n || n >= std::size(value)) return false;
        return value[0] != L'0';
    }();
    return enabled;
}

CameraSample BuildCamera(const sl::Constants& values, uint64_t frame, uint64_t viewport) noexcept {
    CameraSample sample{};
    sample.frame = frame;
    sample.viewport = viewport;
    sample.camera.origin = nb::Origin::Streamline;
    CopyMatrix(sample.camera.viewToClip, values.cameraViewToClip);
    CopyMatrix(sample.camera.clipToView, values.clipToCameraView);
    CopyMatrix(sample.camera.clipToPrevious, values.clipToPrevClip);
    CopyMatrix(sample.camera.previousToClip, values.prevClipToClip);
    CopyMatrix(sample.camera.clipToLens, values.clipToLensClip);

    sample.camera.position[0] = values.cameraPos.x;
    sample.camera.position[1] = values.cameraPos.y;
    sample.camera.position[2] = values.cameraPos.z;
    sample.camera.up[0] = values.cameraUp.x;
    sample.camera.up[1] = values.cameraUp.y;
    sample.camera.up[2] = values.cameraUp.z;
    sample.camera.right[0] = values.cameraRight.x;
    sample.camera.right[1] = values.cameraRight.y;
    sample.camera.right[2] = values.cameraRight.z;
    sample.camera.forward[0] = values.cameraFwd.x;
    sample.camera.forward[1] = values.cameraFwd.y;
    sample.camera.forward[2] = values.cameraFwd.z;
    sample.camera.jitter[0] = values.jitterOffset.x;
    sample.camera.jitter[1] = values.jitterOffset.y;
    sample.motionScale[0] = values.mvecScale.x;
    sample.motionScale[1] = values.mvecScale.y;
    sample.camera.invalidMotionValue = values.motionVectorsInvalidValue;

    sample.valid = SlBoolValid(values.depthInverted) &&
        SlBoolValid(values.cameraMotionIncluded) && SlBoolValid(values.motionVectors3D) &&
        SlBoolValid(values.reset) && SlBoolValid(values.orthographicProjection) &&
        SlBoolValid(values.motionVectorsDilated) && SlBoolValid(values.motionVectorsJittered);
    if (!sample.valid) return sample;

    if (values.depthInverted == sl::Boolean::eTrue) sample.camera.flags |= nb::DepthInverted;
    if (values.cameraMotionIncluded == sl::Boolean::eTrue) sample.camera.flags |= nb::CameraMotionIncluded;
    if (values.motionVectors3D == sl::Boolean::eTrue) sample.camera.flags |= nb::Motion3D;
    if (values.reset == sl::Boolean::eTrue) sample.camera.flags |= nb::Reset;
    if (values.orthographicProjection == sl::Boolean::eTrue) sample.camera.flags |= nb::Orthographic;
    if (values.motionVectorsDilated == sl::Boolean::eTrue) sample.camera.flags |= nb::MotionDilated;
    if (values.motionVectorsJittered == sl::Boolean::eTrue) sample.camera.flags |= nb::MotionJittered;

    float nearPlane = values.cameraNear;
    float farPlane = values.cameraFar;
    float fov = values.cameraFOV;
    float projection[4][4]{};
    std::memcpy(projection, &values.cameraViewToClip, sizeof(projection));
    const double b = projection[1][1];
    const double c = projection[2][2];
    const double d = projection[3][2];
    const double e = projection[2][3];
    if (!Reasonable(nearPlane) || nearPlane <= 0.0f || !Reasonable(farPlane) || farPlane < 0.0f) {
        if (std::isfinite(c) && std::isfinite(d) && std::abs(c) > 1e-12) {
            double derivedNear = e < 0.0 ? d / c : -d / c;
            double derivedFar = e < 0.0 ? d / (c + 1.0) : -d / (c - 1.0);
            if (values.depthInverted == sl::Boolean::eTrue) std::swap(derivedNear, derivedFar);
            if (std::isfinite(derivedNear) && derivedNear > 0.0) nearPlane = static_cast<float>(derivedNear);
            if (std::isfinite(derivedFar) && derivedFar >= 0.0) farPlane = static_cast<float>(derivedFar);
        }
    }
    if (!Reasonable(fov) || fov <= 0.0f || fov >= 3.14159265f) {
        if (std::isfinite(b) && std::abs(b) > 1e-12)
            fov = static_cast<float>(2.0 * std::atan(1.0 / std::abs(b)));
    }
    sample.camera.nearPlane = nearPlane;
    sample.camera.farPlane = farPlane;
    sample.camera.verticalFov = fov;
    sample.camera.aspect = values.cameraAspectRatio;
    if (farPlane == 0.0f) sample.camera.flags |= nb::InfiniteFar;
    return sample;
}

std::optional<CameraSample> FindCameraLocked(ProducerState& state, uint64_t frame, uint64_t viewport) {
    for (const CameraSample& sample : state.cameras)
        if (sample.valid && sample.frame == frame && sample.viewport == viewport) return sample;
    return std::nullopt;
}

void ControlLoop(std::stop_token stop) noexcept {
    ProducerState& state = S();
    while (!stop.stop_requested()) {
        const HRESULT accept = state.session.Accept(kAcceptPollMs);
        if (accept == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) continue;
        if (FAILED(accept)) {
            state.failed.store(true, std::memory_order_release);
            return;
        }
        {
            std::scoped_lock lock(state.renderMutex);
            state.policy = state.session.GetPolicy();
        }
        state.connected.store(true, std::memory_order_release);
        break;
    }

    while (!stop.stop_requested() && state.connected.load(std::memory_order_acquire)) {
        Outbound item{};
        bool haveItem = false;
        {
            std::unique_lock lock(state.outboundMutex);
            if (state.outbound.empty())
                state.outboundCv.wait_for(lock, std::chrono::milliseconds(kControlPollMs));
            if (!state.outbound.empty()) {
                item = state.outbound.front();
                state.outbound.pop_front();
                haveItem = true;
            }
        }
        if (haveItem) {
            const HRESULT sent = state.session.SendFrame(item.token, item.packet, item.packet.timestampNs, kControlSendMs);
            if (FAILED(sent)) {
                state.failed.store(true, std::memory_order_release);
                state.connected.store(false, std::memory_order_release);
                return;
            }
        }
        nb::Token release{};
        const HRESULT received = state.session.ReceiveRelease(release, kControlPollMs);
        if (received != HRESULT_FROM_WIN32(ERROR_TIMEOUT) && FAILED(received)) {
            state.failed.store(true, std::memory_order_release);
            state.connected.store(false, std::memory_order_release);
            return;
        }
    }
}

bool StartProducerLocked(ProducerState& state, ID3D12Resource* color, uint64_t viewport) noexcept {
    if (state.listening.load(std::memory_order_acquire)) return true;
    if (!color || state.failed.load(std::memory_order_acquire)) return false;
    const D3D12_RESOURCE_DESC desc = color->GetDesc();
    const nb::Format format = ColorFormat(desc.Format);
    if (format == nb::Format::Unknown || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Width == 0 || desc.Width > nb::kMaxDimension || desc.Height == 0 || desc.Height > nb::kMaxDimension ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(color->GetDevice(IID_PPV_ARGS(device.ReleaseAndGetAddressOf())))) return false;

    nb::session::ProducerConfig config{};
    config.instance = kPipeInstance;
    config.session = NewSessionId();
    config.generation = state.generation;
    config.viewport = viewport;
    config.extent = {static_cast<uint32_t>(desc.Width), desc.Height};
    config.colorFormat = format;
    config.requireDifferentAdapters = true;
    const HRESULT hr = state.session.Listen(device.Get(), config);
    if (FAILED(hr)) return false;
    state.extent = config.extent;
    state.colorFormat = format;
    state.viewport = viewport;
    state.listening.store(true, std::memory_order_release);
    try {
        state.control = std::jthread(ControlLoop);
    } catch (...) {
        state.failed.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

FrameSlot* FindOrAllocateSlotLocked(ProducerState& state, uint64_t frame, uint64_t viewport) noexcept {
    for (FrameSlot& slot : state.slots)
        if (slot.active && slot.frame == frame && slot.viewport == viewport) return &slot;

    for (uint32_t i = 0; i < state.slots.size(); ++i) {
        FrameSlot& slot = state.slots[i];
        bool reusable = !slot.active;
        if (!reusable && slot.signaled)
            reusable = state.session.Transport().PollDone(slot.token.slot, slot.token.serial) == S_OK;
        if (!reusable) continue;
        if (state.nextSerial == 0 || state.nextSerial == UINT64_MAX) {
            state.failed.store(true, std::memory_order_release);
            return nullptr;
        }
        slot = {};
        slot.active = true;
        slot.frame = frame;
        slot.viewport = viewport;
        slot.token = {i, state.nextSerial++};
        return &slot;
    }
    return nullptr;
}

bool FullExtent(const sl::ResourceTag& tag, const D3D12_RESOURCE_DESC& desc, nb::Extent expected) noexcept {
    const uint32_t width = static_cast<uint32_t>(desc.Width);
    const uint32_t height = desc.Height;
    if (width != expected.width || height != expected.height) return false;
    if (!tag.extent) return true;
    return tag.extent.left == 0 && tag.extent.top == 0 &&
        tag.extent.width == expected.width && tag.extent.height == expected.height;
}

int PlaneForTag(sl::BufferType type) noexcept {
    if (type == sl::kBufferTypeHUDLessColor) return 0;
    if (type == sl::kBufferTypeDepth || type == sl::kBufferTypeHiResDepth) return 1;
    if (type == sl::kBufferTypeMotionVectors) return 2;
    return -1;
}

bool CanonicalFormat(int plane, DXGI_FORMAT format, nb::Format colorFormat) noexcept {
    if (plane == 0) return static_cast<uint32_t>(format) == static_cast<uint32_t>(colorFormat);
    if (plane == 1) return format == DXGI_FORMAT_R32_FLOAT;
    if (plane == 2) return format == DXGI_FORMAT_R16G16_FLOAT;
    return false;
}

void RejectSlotLocked(ProducerState& state, FrameSlot& slot) noexcept {
    slot = {};
    state.rejected.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

void OnConstants(const sl::Constants& values, uint32_t frame, uint64_t viewport) noexcept {
    if (!EnvEnabled()) return;
    ProducerState& state = S();
    CameraSample sample = BuildCamera(values, frame, viewport);
    std::scoped_lock lock(state.renderMutex);
    state.cameras[state.nextCamera++ % state.cameras.size()] = sample;
}

void OnTags(uint32_t frame, uint64_t viewport, const sl::ResourceTag* tags,
            uint32_t count, ID3D12GraphicsCommandList* gameCommandList) noexcept {
    if (!EnvEnabled() || !tags || !count) return;
    ProducerState& state = S();
    std::scoped_lock lock(state.renderMutex);

    if (!state.listening.load(std::memory_order_acquire)) {
        for (uint32_t i = 0; i < count; ++i) {
            if (tags[i].type != sl::kBufferTypeHUDLessColor || !tags[i].resource || !tags[i].resource->native) continue;
            auto* resource = static_cast<ID3D12Resource*>(tags[i].resource->native);
            if (StartProducerLocked(state, resource, viewport)) break;
        }
    }
    if (!state.connected.load(std::memory_order_acquire) || viewport != state.viewport) return;

    FrameSlot* slot = FindOrAllocateSlotLocked(state, frame, viewport);
    if (!slot) {
        state.rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const sl::ResourceTag& tag = tags[i];
        const int plane = PlaneForTag(tag.type);
        if (plane < 0 || !tag.resource || !tag.resource->native) continue;
        if (!gameCommandList) {
            // Without the game's command list the resource lifetime/state cannot be
            // proven safe. Do not retain the engine pointer and guess later.
            RejectSlotLocked(state, *slot);
            return;
        }
        auto* resource = static_cast<ID3D12Resource*>(tag.resource->native);
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        if (!FullExtent(tag, desc, state.extent) || !CanonicalFormat(plane, desc.Format, state.colorFormat) ||
            desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
            desc.MipLevels != 1 || desc.SampleDesc.Count != 1) {
            RejectSlotLocked(state, *slot);
            return;
        }
        const HRESULT copied = state.session.Transport().RecordWritePlane(
            slot->token.slot, static_cast<uint32_t>(plane), gameCommandList, resource,
            static_cast<D3D12_RESOURCE_STATES>(tag.resource->state));
        if (FAILED(copied)) {
            RejectSlotLocked(state, *slot);
            return;
        }
        slot->planes |= static_cast<uint8_t>(1u << plane);
    }
}

void OnPresentStart(uint32_t frame, ID3D12CommandQueue* gameQueue) noexcept {
    if (!EnvEnabled()) return;
    ProducerState& state = S();
    if (!state.connected.load(std::memory_order_acquire) || !gameQueue) return;

    Outbound outbound{};
    {
        std::scoped_lock lock(state.renderMutex);
        FrameSlot* slot = nullptr;
        for (FrameSlot& candidate : state.slots)
            if (candidate.active && candidate.frame == frame && candidate.viewport == state.viewport) { slot = &candidate; break; }
        if (!slot) return;
        if (slot->planes != kAllPlanes) {
            RejectSlotLocked(state, *slot);
            return;
        }
        const auto cameraSample = FindCameraLocked(state, frame, state.viewport);
        if (!cameraSample || !cameraSample->valid) {
            RejectSlotLocked(state, *slot);
            return;
        }

        nb::Packet packet{};
        packet.key = {state.policy.session, state.policy.generation, frame, state.policy.viewport};
        packet.previousFrame = state.haveLastPublished ? state.lastPublishedFrame : 0;
        packet.timestampNs = NowNs();
        packet.renderAdapter = state.policy.renderAdapter;
        packet.processingAdapter = state.policy.processingAdapter;
        const nb::Rect full{0,0,state.extent.width,state.extent.height};
        packet.color = {packet.key, state.extent, full, state.colorFormat, 1};
        packet.depth = {packet.key, state.extent, full, nb::Format::DepthR32, 1};
        packet.motion = {packet.key, state.extent, full, nb::Format::MotionRg16, 1};
        packet.camera = cameraSample->camera;
        packet.camera.key = packet.key;
        if (!Reasonable(packet.camera.aspect) || packet.camera.aspect <= 0.0f)
            packet.camera.aspect = static_cast<float>(state.extent.width) / state.extent.height;
        packet.camera.rawMotionToPixels[0] = cameraSample->motionScale[0] * state.extent.width;
        packet.camera.rawMotionToPixels[1] = cameraSample->motionScale[1] * state.extent.height;
        if (state.haveLastPublished && frame != state.lastPublishedFrame + 1)
            packet.camera.flags |= nb::Reset;

        const nb::Error valid = nb::validate(packet, state.policy, packet.timestampNs);
        if (valid != nb::Error::None) {
            RejectSlotLocked(state, *slot);
            return;
        }
        const HRESULT signaled = state.session.Transport().SignalReady(
            gameQueue, slot->token.slot, slot->token.serial);
        if (FAILED(signaled)) {
            RejectSlotLocked(state, *slot);
            state.failed.store(true, std::memory_order_release);
            return;
        }
        slot->signaled = true;
        outbound = {slot->token, packet};
        state.lastPublishedFrame = frame;
        state.haveLastPublished = true;
        state.published.fetch_add(1, std::memory_order_relaxed);
    }

    {
        std::scoped_lock lock(state.outboundMutex);
        if (state.outbound.size() >= nb::kSlotCount) {
            state.failed.store(true, std::memory_order_release);
            state.connected.store(false, std::memory_order_release);
            return;
        }
        state.outbound.push_back(outbound);
    }
    state.outboundCv.notify_one();
}

bool Enabled() noexcept { return EnvEnabled(); }
bool Connected() noexcept { return S().connected.load(std::memory_order_acquire); }
uint64_t PublishedFrames() noexcept { return S().published.load(std::memory_order_relaxed); }
uint64_t RejectedFrames() noexcept { return S().rejected.load(std::memory_order_relaxed); }

} // namespace NativeBridgeCapture