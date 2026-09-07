#pragma once

namespace MultiGPU
{
// When the secondary XeFG Present worker is still busy, preserving render-GPU
// cadence is more important than forcing every source frame through FG.  Only
// immediate (SyncInterval == 0) normal presents are eligible for shedding.
inline bool CanDropBusyXeFGSource(bool asyncEligible, unsigned syncInterval,
                                  unsigned flags, bool previousPending)
{
    constexpr unsigned Test = 0x1;
    constexpr unsigned DoNotSequence = 0x2;
    return asyncEligible && previousPending && syncInterval == 0 &&
           (flags & (Test | DoNotSequence)) == 0;
}
}
