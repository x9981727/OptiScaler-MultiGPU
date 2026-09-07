#pragma once
#include <charconv>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>

namespace MultiGPU
{
// An existing canonical key, including auto/empty/invalid, takes precedence.
// This lets a saved auto value clear a stale legacy FTSource override.
inline std::optional<int> ReadFrameTimeSetting(const char* canonical, const char* legacy, bool xefg)
{
    const char* selected = canonical != nullptr ? canonical : legacy;
    if (selected == nullptr) return std::nullopt;
    std::string_view value(selected);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
    if (value.empty()) return std::nullopt;
    int parsed = -1;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed < 0 || parsed > (xefg ? 2 : 1))
        return std::nullopt;
    return parsed;
}

struct XeFGFrameTimeDecision
{
    int requested = -1; // -1 = auto; 0 = input; 1 = application Present interval; 2 = unavailable.
    int source = 0;
    float sdkMs = 0;
    bool invalid = false;
};

inline XeFGFrameTimeDecision SelectXeFGFrameTime(std::optional<int> configured, bool secondaryAsync,
                                                float inputMs, float presentMs)
{
    if (configured && (*configured < 0 || *configured > 2)) configured.reset();
    XeFGFrameTimeDecision out;
    out.requested = configured.value_or(-1);
    // The optional input-duration hint has not been validated against the
    // cadence of this two-device asynchronous pipeline. Auto omits that hint
    // using the SDK-documented unavailable sentinel for this path only.
    out.source = configured.value_or(secondaryAsync ? 2 : 0);
    const float selected = out.source == 0 ? inputMs : out.source == 1 ? presentMs : 0.0f;
    out.invalid = out.source != 2 && (!std::isfinite(selected) || selected < 0.0f);
    out.sdkMs = out.invalid ? 0.0f : selected;
    return out;
}

struct XeFGFrameTimeSnapshot
{
    std::uint64_t frames = 0, inputFrames = 0, presentFrames = 0, zeroFrames = 0, invalidFrames = 0;
    std::uint64_t inputSamples = 0, presentSamples = 0;
    double sdkSum = 0, inputSum = 0, presentSum = 0;
    int requested = -1, source = -1;
    double SdkMean() const { return frames ? sdkSum / frames : -1; }
    double InputMean() const { return inputSamples ? inputSum / inputSamples : -1; }
    double PresentMean() const { return presentSamples ? presentSum / presentSamples : -1; }
};

class XeFGFrameTimeTelemetry
{
    std::mutex _mutex;
    XeFGFrameTimeSnapshot _data;
public:
    void Record(const XeFGFrameTimeDecision& value, float inputMs, float presentMs)
    {
        std::lock_guard lock(_mutex);
        ++_data.frames;
        _data.requested = value.requested; _data.source = value.source;
        _data.sdkSum += value.sdkMs;
        _data.inputFrames += value.source == 0; _data.presentFrames += value.source == 1;
        _data.zeroFrames += value.source == 2; _data.invalidFrames += value.invalid;
        if (std::isfinite(inputMs) && inputMs >= 0) { _data.inputSum += inputMs; ++_data.inputSamples; }
        if (std::isfinite(presentMs) && presentMs >= 0) { _data.presentSum += presentMs; ++_data.presentSamples; }
    }
    XeFGFrameTimeSnapshot Take()
    {
        std::lock_guard lock(_mutex);
        auto result = _data; _data = {}; return result;
    }
};
}
