// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace nb {
constexpr uint32_t kMagic = 0x3142474d;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxDimension = 16384;
constexpr uint32_t kSlotCount = 3;
struct AdapterId { uint32_t low{}; int32_t high{}; bool operator==(const AdapterId&) const = default; };
struct Key { uint64_t session{}, generation{}, frame{}, viewport{}; bool operator==(const Key&) const = default; };
struct Extent { uint32_t width{}, height{}; bool operator==(const Extent&) const = default; };
struct Rect { uint32_t x{}, y{}, width{}, height{}; bool operator==(const Rect&) const = default; };
struct Matrix { float m[16]{}; };
enum class Origin : uint32_t { Unknown, Streamline, EnginePlugin, GameProfile };
enum CameraFlags : uint32_t {
    DepthInverted=1u, InfiniteFar=2u, CameraMotionIncluded=4u,
    MotionDilated=8u, MotionJittered=16u, Orthographic=32u,
    Reset=64u, LensProvided=128u, Motion3D=256u
};
constexpr uint32_t kKnownCameraFlags = 511;
struct Camera {
    Key key{};
    Origin origin{};
    uint32_t flags{};
    Matrix viewToClip{}, clipToView{}, clipToPrevious{}, previousToClip{}, clipToLens{};
    float position[3]{}, up[3]{}, right[3]{}, forward[3]{};
    float nearPlane{}, farPlane{}, verticalFov{}, aspect{};
    float jitter[2]{}, rawMotionToPixels[2]{}, invalidMotionValue{};
};
// Values intentionally match DXGI_FORMAT for direct host validation.
// Unknown is handshake-only and is never valid in a published Packet/Layout.
enum class Format : uint32_t {
    Unknown=0, Rgba16F=10, Rgba8=28, MotionRg16=34, DepthR32=41, Bgra8=87
};
constexpr bool valid_color_format(Format f) noexcept {
    return f==Format::Rgba16F || f==Format::Rgba8 || f==Format::Bgra8;
}
constexpr uint32_t bytes_per_pixel(Format f) noexcept {
    return f==Format::Rgba16F ? 8u :
        (f==Format::Rgba8 || f==Format::Bgra8 || f==Format::DepthR32 || f==Format::MotionRg16 ? 4u : 0u);
}
struct Plane {
    Key key{}; Extent extent{}; Rect validRect{}; Format format{};
    uint32_t nativeResource{};
};
struct Packet {
    uint32_t magic=kMagic, version=kVersion;
    Key key{}; uint64_t previousFrame{}, timestampNs{};
    AdapterId renderAdapter{}, processingAdapter{};
    Plane color{}, depth{}, motion{}; Camera camera{};
};
static_assert(std::is_trivially_copyable_v<Packet>);
enum class Error : uint32_t {
    None, Version, Session, FrameMismatch, Extent, Region, Format,
    NotNative, CameraSource, CameraFlags, Matrix, Projection, CameraValues,
    MotionUnsupported, FutureTimestamp, TooOld, WrongAdapter, Fence,
    SlotBusy, StaleToken, BadState
};
std::string_view error_name(Error) noexcept;
struct Policy {
    uint64_t session{}, generation{}, viewport{};
    AdapterId renderAdapter{}, processingAdapter{};
    uint64_t maxAgeNs=100'000'000;
    bool requireDifferentAdapters=true;
};
bool valid_extent(Extent) noexcept;
bool valid_rect(Rect, Extent) noexcept;
bool finite_matrix(const Matrix&) noexcept;
bool inverse_pair(const Matrix&, const Matrix&, double tolerance=0.002) noexcept;
Error validate_camera(const Camera&, const Key&) noexcept;
Error validate(const Packet&, const Policy&, uint64_t nowNs) noexcept;
struct Footprint {
    Format format{}; Extent extent{};
    uint64_t offset{}, bytes{}; uint32_t rowPitch{}, rowBytes{};
};
struct Layout {
    std::array<Footprint,3> planes{};
    uint64_t bufferBytes{}, slotStride{}, heapBytes{};
};
std::optional<Layout> make_layout(Extent, Format color=Format::Rgba8) noexcept;
struct HistoryResult { bool reset{}; };
class History {
public:
    HistoryResult accept(const Packet&) noexcept;
    void clear() noexcept { previous_.reset(); }
private:
    std::optional<Packet> previous_;
};
} // namespace nb