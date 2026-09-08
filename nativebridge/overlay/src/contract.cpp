// SPDX-License-Identifier: MIT
#include "native_bridge/contract.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nb {
std::string_view error_name(Error e) noexcept {
    constexpr std::string_view names[] = {"ok","version","session","frame-mismatch","extent",
        "region","format","not-native","camera-source","camera-flags","matrix","projection",
        "camera-values","motion-unsupported","future-timestamp","too-old","wrong-adapter","fence",
        "slot-busy","stale-token","bad-state"};
    const auto n=static_cast<uint32_t>(e);
    return n<std::size(names) ? names[n] : "unknown";
}
bool valid_extent(Extent e) noexcept {
    return e.width && e.height && e.width<=kMaxDimension && e.height<=kMaxDimension;
}
bool valid_rect(Rect r, Extent e) noexcept {
    return r.width && r.height && r.x<=e.width && r.y<=e.height &&
        r.width<=e.width-r.x && r.height<=e.height-r.y;
}
bool finite_matrix(const Matrix& m) noexcept {
    for (float x:m.m) if (!std::isfinite(x) || std::abs(x)>1e20f) return false;
    return true;
}
bool inverse_pair(const Matrix& a,const Matrix& b,double tolerance) noexcept {
    if (!finite_matrix(a)||!finite_matrix(b)) return false;
    for (int r=0;r<4;++r) for(int c=0;c<4;++c) {
        double sum=0;
        for(int k=0;k<4;++k) sum+=double(a.m[r*4+k])*b.m[k*4+c];
        if (!std::isfinite(sum)||std::abs(sum-(r==c ? 1.0:0.0))>tolerance) return false;
    }
    return true;
}
static bool unit(const float* v) noexcept {
    double n=0; for(int i=0;i<3;++i) { if(!std::isfinite(v[i])) return false; n+=double(v[i])*v[i]; }
    return std::abs(n-1)<0.02;
}
static double dot(const float* a,const float* b) noexcept {
    return double(a[0])*b[0]+double(a[1])*b[1]+double(a[2])*b[2];
}
Error validate_camera(const Camera& c,const Key& k) noexcept {
    if(c.key!=k) return Error::FrameMismatch;
    if(c.origin!=Origin::Streamline && c.origin!=Origin::EnginePlugin && c.origin!=Origin::GameProfile)
        return Error::CameraSource;
    if(c.flags & ~kKnownCameraFlags) return Error::CameraFlags;
    if(!inverse_pair(c.viewToClip,c.clipToView) || !inverse_pair(c.clipToPrevious,c.previousToClip))
        return Error::Matrix;
    if((c.flags & LensProvided) && !finite_matrix(c.clipToLens)) return Error::Matrix;
    if(!(c.flags & Orthographic) && (std::abs(c.viewToClip.m[15])>1e-4f ||
        std::abs(c.viewToClip.m[11])<1e-6f)) return Error::Projection;
    if(!std::isfinite(c.nearPlane)||c.nearPlane<=0 ||
       !std::isfinite(c.aspect)||c.aspect<=0 ||
       !std::isfinite(c.verticalFov)||c.verticalFov<=0||c.verticalFov>=3.14159265f)
        return Error::CameraValues;
    if(c.flags & InfiniteFar) {
        if(!(c.farPlane==0 || (std::isinf(c.farPlane)&&c.farPlane>0))) return Error::CameraValues;
    } else if(!std::isfinite(c.farPlane)||c.farPlane<=c.nearPlane) return Error::CameraValues;
    if(!unit(c.up)||!unit(c.right)||!unit(c.forward) ||
       std::abs(dot(c.up,c.right))>0.02 || std::abs(dot(c.up,c.forward))>0.02 ||
       std::abs(dot(c.right,c.forward))>0.02) return Error::CameraValues;
    for(float p:c.position) if(!std::isfinite(p) || std::abs(p)>1e20f) return Error::CameraValues;
    for(float j:c.jitter) if(!std::isfinite(j)) return Error::CameraValues;
    if((c.flags & (Motion3D|MotionJittered)) || !(c.flags & CameraMotionIncluded))
        return Error::MotionUnsupported;
    for(float s:c.rawMotionToPixels) if(!std::isfinite(s)||s==0||std::abs(s)>1e10f)
        return Error::MotionUnsupported;
    return Error::None;
}
Error validate(const Packet& p,const Policy& policy,uint64_t nowNs) noexcept {
    if(p.magic!=kMagic||p.version!=kVersion) return Error::Version;
    if(!p.key.session||!p.key.generation||p.key.session!=policy.session ||
       p.key.generation!=policy.generation||p.key.viewport!=policy.viewport) return Error::Session;
    if(p.renderAdapter!=policy.renderAdapter||p.processingAdapter!=policy.processingAdapter ||
       (policy.requireDifferentAdapters&&p.renderAdapter==p.processingAdapter)) return Error::WrongAdapter;
    if(p.color.key!=p.key||p.depth.key!=p.key||p.motion.key!=p.key) return Error::FrameMismatch;
    if(!valid_extent(p.color.extent)||p.depth.extent!=p.color.extent||p.motion.extent!=p.color.extent)
        return Error::Extent;
    const auto e=p.color.extent; const Rect full{0,0,e.width,e.height};
    if(p.color.validRect!=full||p.depth.validRect!=full||p.motion.validRect!=full) return Error::Region;
    if(!valid_color_format(p.color.format) || p.depth.format!=Format::DepthR32 ||
       p.motion.format!=Format::MotionRg16) return Error::Format;
    if(p.color.nativeResource!=1||p.depth.nativeResource!=1||p.motion.nativeResource!=1) return Error::NotNative;
    const auto ce=validate_camera(p.camera,p.key); if(ce!=Error::None) return ce;
    if(p.timestampNs>nowNs) return Error::FutureTimestamp;
    if(nowNs-p.timestampNs>policy.maxAgeNs) return Error::TooOld;
    return Error::None;
}
static uint64_t align(uint64_t n,uint64_t a) noexcept { return (n+a-1)/a*a; }
std::optional<Layout> make_layout(Extent e,Format color) noexcept {
    if(!valid_extent(e)||!valid_color_format(color)) return {};
    Layout result{}; uint64_t cursor=0;
    const Format formats[]{color,Format::DepthR32,Format::MotionRg16};
    for(size_t i=0;i<3;++i) {
        auto& p=result.planes[i]; p.format=formats[i]; p.extent=e;
        const uint32_t bpp=bytes_per_pixel(p.format); if(!bpp) return {};
        const uint64_t rowBytes64=uint64_t(e.width)*bpp;
        if(rowBytes64>UINT32_MAX) return {};
        p.rowBytes=static_cast<uint32_t>(rowBytes64);
        p.rowPitch=static_cast<uint32_t>(align(p.rowBytes,256));
        p.offset=align(cursor,512); p.bytes=uint64_t(p.rowPitch)*e.height;
        cursor=p.offset+p.bytes;
    }
    result.bufferBytes=cursor;
    result.slotStride=align(cursor,65536);
    result.heapBytes=result.slotStride*kSlotCount;
    return result;
}
HistoryResult History::accept(const Packet& p) noexcept {
    bool reset=!previous_.has_value() || (p.camera.flags & Reset);
    if(previous_) {
        const auto& q=*previous_;
        reset=reset||q.key.session!=p.key.session||q.key.generation!=p.key.generation||
            q.key.viewport!=p.key.viewport||p.previousFrame!=q.key.frame||
            p.key.frame<=q.key.frame||p.color.extent!=q.color.extent||
            p.color.format!=q.color.format||p.camera.origin!=q.camera.origin ||
            ((p.camera.flags^q.camera.flags)&~uint32_t(Reset)) ||
            p.camera.rawMotionToPixels[0]!=q.camera.rawMotionToPixels[0]||
            p.camera.rawMotionToPixels[1]!=q.camera.rawMotionToPixels[1];
    }
    previous_=p; return {reset};
}
}
