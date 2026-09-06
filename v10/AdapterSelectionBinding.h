#pragma once

#include <cstdint>
#include <optional>

namespace MultiGPU
{
struct AdapterSelection
{
    bool enabled = false;
    std::optional<std::uint64_t> luid;

    bool operator==(const AdapterSelection& other) const
    {
        // An unused saved adapter does not change single-GPU execution.
        return enabled == other.enabled && (!enabled || luid == other.luid);
    }
};

// The SDK context and its swapchain own one adapter selection for their lifetime.
// Toggling FG or rebuilding frame resources does not begin a new lifetime.
class AdapterSelectionBinding
{
  private:
    std::optional<AdapterSelection> _bound;

  public:
    bool Bind(const AdapterSelection& selection)
    {
        if (_bound.has_value())
            return false;
        _bound = selection;
        return true;
    }

    bool IsBound() const { return _bound.has_value(); }

    bool RequiresRestart(const AdapterSelection& requested) const
    {
        return _bound.has_value() && !(*_bound == requested);
    }

    void Reset() { _bound.reset(); }
};
} // namespace MultiGPU
