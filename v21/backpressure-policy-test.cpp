#include "XeFGBackpressurePolicy.h"
#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

int main()
{
    using MultiGPU::CanDropBusyXeFGSource;
    constexpr unsigned AllowTearing = 0x200;
    constexpr unsigned Test = 0x1;
    constexpr unsigned DoNotSequence = 0x2;

    Check(CanDropBusyXeFGSource(true, 0, 0, true, 0), "immediate busy present should drop");
    Check(CanDropBusyXeFGSource(true, 0, AllowTearing, true, 0), "tearing immediate busy present should drop");
    Check(!CanDropBusyXeFGSource(false, 0, 0, true, 0), "ineligible path dropped");
    Check(!CanDropBusyXeFGSource(true, 1, 0, true, 0), "vsync present dropped");
    Check(!CanDropBusyXeFGSource(true, 0, 0, false, 0), "idle worker dropped source");
    Check(!CanDropBusyXeFGSource(true, 0, Test, true, 0), "test present dropped");
    Check(!CanDropBusyXeFGSource(true, 0, DoNotSequence, true, 0), "do-not-sequence present dropped");
    Check(!CanDropBusyXeFGSource(true, 0, 0, true, 1), "second consecutive source was shed");

    std::cout << "PASS: v21 backpressure sheds at most one consecutive immediate source frame while the XeFG worker is busy\n";
}
