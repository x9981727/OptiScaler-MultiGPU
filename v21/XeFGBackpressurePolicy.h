#pragma once

namespace MultiGPU
{
// When the secondary XeFG Present worker is still busy, preserving render-GPU
// cadence is more important than forcing every source frame through FG. Only
// immediate (SyncInterval == 0) normal presents are eligible for shedding.
// Limit shedding to one consecutive source frame; if the worker is still busy
// on the following Present, fall back to the old bounded wait instead of letting
// source-frame continuity collapse under severe secondary-GPU overload.
inline bool CanDropBusyXeFGSource(bool asyncEligible, unsigned syncInterval,
                                  unsigned flags, bool previousPending,
                                  unsigned consecutiveDrops)
{
    constexpr unsigned Test = 0x1;
    constexpr unsigned DoNotSequence = 0x2;
    return asyncEligible && previousPending && consecutiveDrops == 0 &&
           syncInterval == 0 && (flags & (Test | DoNotSequence)) == 0;
}
}
