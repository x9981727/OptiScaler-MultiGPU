#pragma once
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace MultiGPU
{
enum class XeFGCpuPhase { Sleep, ProxyPresent, Bridge, FGSlot, RenderSlot, Overlay, Locks, Count };
class XeFGTimingCounters
{
  public:
    struct Value
    {
        double totalMs = 0;
        std::uint64_t calls = 0;
        double Mean() const { return calls != 0 ? totalMs / calls : -1.0; }
    };
    using Snapshot = std::array<Value, static_cast<std::size_t>(XeFGCpuPhase::Count)>;
  private:
    std::mutex _mutex;
    Snapshot _values {};
  public:
    void Add(XeFGCpuPhase phase, double ms)
    {
        const auto i = static_cast<std::size_t>(phase);
        if (i >= _values.size() || !std::isfinite(ms) || ms < 0)
            return;
        std::lock_guard lock(_mutex);
        _values[i].totalMs += ms;
        ++_values[i].calls;
    }
    Snapshot Take()
    {
        std::lock_guard lock(_mutex);
        const auto result = _values;
        _values = {};
        return result;
    }
};
}
