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

    Check(CanDropBusyXeFGSource(true, 0, 0, true), "immediate busy present should drop");
    Check(CanDropBusyXeFGSource(true, 0, AllowTearing, true), "tearing immediate busy present should drop");
    Check(!CanDropBusyXeFGSource(false, 0, 0, true), "ineligible path dropped");
    Check(!CanDropBusyXeFGSource(true, 1, 0, true), "vsync present dropped");
    Check(!CanDropBusyXeFGSource(true, 0, 0, false), "idle worker dropped source");
    Check(!CanDropBusyXeFGSource(true, 0, Test, true), "test present dropped");
    Check(!CanDropBusyXeFGSource(true, 0, DoNotSequence, true), "do-not-sequence present dropped");

    std::cout << "PASS: v21 backpressure only sheds immediate normal source frames while the XeFG worker is busy\n";
}
