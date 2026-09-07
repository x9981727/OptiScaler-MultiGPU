#pragma once

namespace MultiGPU
{
// v16 is deliberately scoped to the already-bounded secondary XeFG Present path.
// Single-GPU XeFG and synchronous dual-GPU fallback retain upstream XeLL pacing.
inline bool ShouldBypassSecondaryXeFGPacing(bool multiGpuActive, bool asyncPresentEnabled)
{
    return multiGpuActive && asyncPresentEnabled;
}
}
