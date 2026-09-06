#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>

namespace MultiGPU
{
// SDK framesPresented counts frames queued for presentation, not independently
// measured monitor scanouts. Never infer successful interpolation from Active.
class XeFGPresentationStats
{
  public:
    struct Snapshot
    {
        double renderFps = 0;
        double queuedFps = 0;
        bool outputKnown = false;
        std::uint64_t renderFrames = 0;
        std::uint64_t queuedFrames = 0;
    };

  private:
    mutable std::mutex _mutex;
    double _previous = -1;
    double _elapsed = 0;
    double _published = -1;
    std::uint64_t _rendered = 0;
    std::uint64_t _queued = 0;
    bool _allKnown = true;
    bool _enabled = false;
    Snapshot _snapshot;

    void ResetUnlocked(double now, bool enabled)
    {
        _previous = now;
        _elapsed = 0;
        _published = -1;
        _rendered = _queued = 0;
        _allKnown = true;
        _enabled = enabled;
        _snapshot = {};
    }

  public:
    // Returns true once a fresh one-second measurement window is available.
    bool Record(double nowMs, std::uint32_t queuedFrames, bool known, bool enabled)
    {
        std::lock_guard lock(_mutex);
        if (!std::isfinite(nowMs))
            return false;
        if (_previous < 0 || nowMs <= _previous || nowMs - _previous > 2000 || enabled != _enabled)
        {
            ResetUnlocked(nowMs, enabled);
            return false;
        }
        _elapsed += nowMs - _previous;
        _previous = nowMs;
        ++_rendered;
        _queued += known ? queuedFrames : 0;
        _allKnown = _allKnown && known;
        if (_elapsed < 1000)
            return false;
        _snapshot = {1000.0 * _rendered / _elapsed, 1000.0 * _queued / _elapsed,
                     _allKnown, _rendered, _queued};
        _published = nowMs;
        _elapsed = 0;
        _rendered = _queued = 0;
        _allKnown = true;
        return true;
    }

    bool Read(double nowMs, Snapshot& snapshot) const
    {
        std::lock_guard lock(_mutex);
        if (_published < 0 || nowMs < _published || nowMs - _published > 2500)
            return false;
        snapshot = _snapshot;
        return true;
    }
};
} // namespace MultiGPU
