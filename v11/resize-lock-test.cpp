#include "ScopedSwapchainLock.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>

using namespace std::chrono_literals;
static void require(bool ok, const char* why)
{
    if (!ok) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
struct TestMutex
{
    std::mutex mutex;
    std::atomic<int> locks {0}, unlocks {0};
    unsigned owner = 0;
    void lock(unsigned id) { mutex.lock(); owner=id; ++locks; }
    void unlockThis(unsigned id)
    {
        require(owner == id, "unlock belongs to the acquiring scope");
        owner=0; ++unlocks; mutex.unlock();
    }
};
using Lock = MultiGPU::ScopedSwapchainLock<TestMutex>;
int main()
{
    TestMutex fg, local;
    for (unsigned hookId : {6677U, 6678U})
    {
        const int before = fg.locks;
        {
            Lock outer(&fg, 3);
            {
                Lock inner(&fg, hookId); // wrapper ResizeBuffers -> FG SDK hook
                Lock different(&local, 1);
                Lock reenter(&fg, 2);
                require(fg.locks == before+1 && fg.owner == 3, "nested resize does not reacquire FG mutex");
            }
            require(fg.owner == 3, "inner return does not unlock the outer resize");
        }
        require(fg.locks == fg.unlocks, "both resize APIs release the outer mutex");
    }
    try { Lock outer(&fg, 3); Lock inner(&fg, 6677); throw std::runtime_error("SDK failure"); }
    catch (const std::runtime_error&) {}
    require(fg.locks == fg.unlocks, "failure/unwind releases the acquired lock");

    std::promise<void> attempted, acquired;
    auto started = attempted.get_future(); auto entered = acquired.get_future();
    std::future<void> worker;
    {
        Lock outer(&fg, 3);
        worker = std::async(std::launch::async, [&] {
            attempted.set_value();
            Lock otherThread(&fg, 3); // Same owner number must NOT bypass the lock.
            acquired.set_value();
        });
        started.wait();
        require(entered.wait_for(40ms) == std::future_status::timeout, "another thread waits even with the same owner ID");
    }
    require(entered.wait_for(2s) == std::future_status::ready, "waiting presenter resumes after resize");
    worker.get();
    { Lock disabled(nullptr, 3); Lock regular(&fg, 2); }
    require(fg.locks == fg.unlocks && local.locks == local.unlocks, "no leaked lock after all cases");
    std::cout << "PASS: nested resize locks, cross-thread exclusion and failure cleanup\n";
}
