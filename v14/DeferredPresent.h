#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace MultiGPU
{
// Exactly one uncollected operation. A completed operation still owns the slot
// until Take(), so neither its HRESULT nor SDK status can be silently overwritten.
class DeferredPresent
{
    std::mutex _mutex;
    std::condition_variable _changed;
    std::thread _thread;
    std::function<std::int32_t()> _call;
    bool _pending = false, _ready = false, _done = false, _stop = false;
    std::int32_t _result = 0;

    void Run()
    {
        std::unique_lock lock(_mutex);
        for (;;)
        {
            _changed.wait(lock, [&] { return _ready || _stop; });
            if (!_ready) return;
            auto call = std::move(_call);
            _call = {};
            _ready = false;
            lock.unlock();
            std::int32_t result;
            try { result = call(); }
            catch (...) { result = static_cast<std::int32_t>(0x80004005u); } // E_FAIL; C++ exceptions only
            // Release captured COM references before advertising completion.
            call = {};
            lock.lock();
            _result = result;
            _done = true;
            _changed.notify_all();
        }
    }
  public:
    DeferredPresent() = default;
    DeferredPresent(const DeferredPresent&) = delete;
    DeferredPresent& operator=(const DeferredPresent&) = delete;
    ~DeferredPresent() { Stop(); }

    bool Submit(std::function<std::int32_t()> call)
    {
        if (!call) return false;
        std::lock_guard lock(_mutex);
        if (_pending || _stop) return false;
        if (!_thread.joinable())
        {
            try { _thread = std::thread([this] { Run(); }); }
            catch (...) { return false; }
        }
        _call = std::move(call);
        _pending = _ready = true;
        _done = false;
        _changed.notify_all();
        return true;
    }
    bool Pending()
    {
        std::lock_guard lock(_mutex);
        return _pending;
    }
    bool ReadyWithin(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [&] { return !_pending || _done; });
    }
    std::optional<std::int32_t> Take()
    {
        std::lock_guard lock(_mutex);
        if (!_pending || !_done) return std::nullopt;
        _pending = _done = false;
        return _result;
    }
    void Stop()
    {
        {
            std::lock_guard lock(_mutex);
            _stop = true;
            _changed.notify_all();
        }
        if (_thread.joinable()) _thread.join();
    }
};
}
