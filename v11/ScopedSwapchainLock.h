#pragma once

#include <cstdint>

namespace MultiGPU
{
// Wrapper -> SDK hook calls can enter the same swapchain mutex recursively.
// Reuse only a lock held through this scope on this thread, never an owner ID
// observed on a different thread. Different mutexes may be nested as well.
template <typename Mutex> class ScopedSwapchainLock
{
    inline static thread_local ScopedSwapchainLock* _top = nullptr;
    ScopedSwapchainLock* _previous = nullptr;
    Mutex* _mutex;
    std::uint32_t _owner;
    bool _owns = false;

  public:
    ScopedSwapchainLock(Mutex* mutex, std::uint32_t owner) : _mutex(mutex), _owner(owner)
    {
        bool heldHere = false;
        for (auto scope = _top; scope != nullptr; scope = scope->_previous)
            if (mutex != nullptr && scope->_mutex == mutex)
                heldHere = true;
        if (mutex != nullptr && !heldHere)
        {
            mutex->lock(owner);
            _owns = true;
        }
        _previous = _top;
        _top = this;
    }

    ~ScopedSwapchainLock()
    {
        _top = _previous;
        if (_owns)
            _mutex->unlockThis(_owner);
    }
    ScopedSwapchainLock(const ScopedSwapchainLock&) = delete;
    ScopedSwapchainLock& operator=(const ScopedSwapchainLock&) = delete;
};
} // namespace MultiGPU
