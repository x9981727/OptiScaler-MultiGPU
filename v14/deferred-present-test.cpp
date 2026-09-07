#include "DeferredPresent.h"
#include "XeFGPresentPolicy.h"
#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

int main()
{
    using namespace MultiGPU;
    using namespace std::chrono_literals;
    DeferredPresent worker;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<bool> nextRenderStarted {false};
    auto owner = std::make_shared<int>(42);
    auto weak = std::weak_ptr<int>(owner);
    Check(worker.Submit([&, owner, gate] {
        entered.set_value();
        gate.wait();
        Check(nextRenderStarted.load(), "next render did not overlap pending Present");
        Check(*owner == 42, "captured data lifetime");
        return std::int32_t(7);
    }), "first submit");
    owner.reset();
    Check(entered.get_future().wait_for(2s) == std::future_status::ready, "worker entered");
    nextRenderStarted = true;
    Check(worker.Pending() && !worker.ReadyWithin(0ms), "blocked call returned early");
    Check(!worker.Take().has_value(), "unfinished result exposed");
    Check(!worker.Submit([] { return 0; }), "in-flight operation overwritten");
    Check(!weak.expired(), "capture released before completion");
    release.set_value();
    Check(worker.ReadyWithin(2s), "completion");
    Check(weak.expired(), "capture retained past completion");
    Check(!worker.Submit([] { return 0; }), "uncollected result overwritten");
    Check(worker.Take() == 7 && !worker.Take(), "result lost or duplicated");

    std::vector<int> observed;
    for (int i = 0; i < 64; ++i)
    {
        Check(worker.Submit([&, i] { observed.push_back(i); return std::int32_t(i); }), "reuse submit");
        Check(worker.ReadyWithin(2s), "reuse completion");
        Check(worker.Take() == i, "reordered result");
    }
    for (int i = 0; i < 64; ++i) Check(observed[i] == i, "callback order");
    constexpr auto removed = static_cast<std::int32_t>(0x887A0005u);
    Check(worker.Submit([&] { return removed; }), "failure submit");
    Check(worker.ReadyWithin(2s) && worker.Take() == removed, "HRESULT changed");
    Check(worker.Submit([]() -> std::int32_t { throw std::runtime_error("test"); }), "exception submit");
    Check(worker.ReadyWithin(2s) && worker.Take() == static_cast<std::int32_t>(0x80004005u), "exception result");
    std::atomic<bool> stoppedWork {false};
    Check(worker.Submit([&] { stoppedWork = true; return 0; }), "stop submit");
    worker.Stop();
    Check(stoppedWork && worker.Take() == 0, "stop discarded work");
    Check(!worker.Submit([] { return 0; }), "submit after shutdown");

    VirtualBackbufferCursor cursor;
    cursor.Reset(3);
    const unsigned sdkIndex = 0;
    cursor.Advance(true, 0);
    Check(cursor.Current() == 1 && sdkIndex == 0, "render cursor depends on pending SDK cursor");
    cursor.Advance(false, 0); cursor.Advance(true, 1); cursor.Advance(true, 2);
    Check(cursor.Current() == 1, "failed/test/non-sequencing Present advanced cursor");
    cursor.Advance(true, 0x200); cursor.Advance(true, 0);
    Check(cursor.Current() == 0, "ring wrap");
    cursor.Reset(2); cursor.Advance(true, 0); cursor.Advance(true, 0);
    Check(cursor.Current() == 0, "resize reset");

    Check(CanDeferXeFGPresent(true,true,true,true,3,0,0x200,false), "normal policy");
    Check(!CanDeferXeFGPresent(true,true,false,true,3,0,0,false), "exclusive policy");
    Check(!CanDeferXeFGPresent(true,true,true,false,3,0,0,false), "message thread policy");
    Check(!CanDeferXeFGPresent(false,true,true,true,3,0,0,false), "rollback policy");
    Check(!CanDeferXeFGPresent(true,false,true,true,3,0,0,false), "disabled FG policy");
    Check(!CanDeferXeFGPresent(true,true,true,true,1,0,0,false), "single buffer policy");
    Check(!CanDeferXeFGPresent(true,true,true,true,3,0,8,false), "DO_NOT_WAIT policy");
    Check(!CanDeferXeFGPresent(true,true,true,true,3,0,0,true), "partial update policy");
    Check(!XeFGPresentScope::Allowed(), "initial TLS state");
    {
        XeFGPresentScope outer(true);
        std::thread other([] { Check(!XeFGPresentScope::Allowed(), "authorization leaked to worker"); });
        other.join();
        { XeFGPresentScope inner(false); Check(!XeFGPresentScope::Allowed(), "nested scope"); }
        Check(XeFGPresentScope::Allowed(), "outer scope restored");
    }
    Check(!XeFGPresentScope::Allowed(), "scope leaked");
    std::cout << "PASS: deferred Present overlap, bounded ownership, HRESULT delivery, drain, virtual indices and thread scopes\n";
}
