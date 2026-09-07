#pragma once
#include <atomic>
#include <cstdint>

namespace MultiGPU
{
// Only the outer render-adapter virtual swapchain can authorize an SDK handoff.
// This state never leaks to the worker or to unrelated swapchains/threads.
class XeFGPresentScope
{
    inline static thread_local bool _allowed = false;
    bool _previous;
  public:
    explicit XeFGPresentScope(bool allowed) : _previous(_allowed) { _allowed = allowed; }
    ~XeFGPresentScope() { _allowed = _previous; }
    XeFGPresentScope(const XeFGPresentScope&) = delete;
    static bool Allowed() { return _allowed; }
};

inline bool CanDeferXeFGPresent(bool enabled, bool active, bool windowed, bool separateWindowThread,
                               unsigned buffers, unsigned syncInterval, unsigned flags, bool partialUpdate)
{
    constexpr unsigned AllowTearing = 0x200; // DXGI_PRESENT_ALLOW_TEARING
    return enabled && active && windowed && separateWindowThread && buffers >= 2 &&
           syncInterval <= 4 && (flags & ~AllowTearing) == 0 && !partialUpdate;
}

// Render-side buffer ownership is independent of the SDK's current index.
// Advance only for an accepted normal Present, never a TEST or failed Present.
class VirtualBackbufferCursor
{
    std::atomic<unsigned> _index {0};
    unsigned _count = 0; // Reset/Advance are protected by the swapchain mutation lock.
  public:
    void Reset(unsigned count) { _count = count; _index.store(0); }
    unsigned Current() const { return _index.load(); }
    void Advance(bool accepted, unsigned flags)
    {
        constexpr unsigned Test = 0x1, DoNotSequence = 0x2;
        if (accepted && _count != 0 && (flags & (Test | DoNotSequence)) == 0)
            _index.store((_index.load() + 1) % _count);
    }
};
}
