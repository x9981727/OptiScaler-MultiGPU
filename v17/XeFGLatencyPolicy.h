#pragma once

#include <framegen/XeFGNoWaitPolicy.h>

namespace MultiGPU
{
struct XeFGLatencyPolicy
{
    bool armLatencyReduction = true;
    bool bypassExternalSleep = false;
};

inline XeFGLatencyPolicy SelectXeFGLatencyPolicy(bool multiGpuActive, bool asyncPresentEnabled)
{
    XeFGLatencyPolicy policy;
    // XeFG requires latency reduction to remain enabled even when we intentionally
    // bypass the CPU sleep/pacing call on the secondary asynchronous path.
    policy.armLatencyReduction = true;
    policy.bypassExternalSleep = ShouldBypassSecondaryXeFGPacing(multiGpuActive, asyncPresentEnabled);
    return policy;
}
}
