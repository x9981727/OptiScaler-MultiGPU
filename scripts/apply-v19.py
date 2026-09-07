"""v19: prevent secondary XeFG Present pacing from feeding back its own wait time.

v18 proved that a real Input frame time is much better than Zero, but once XeFG
is active the source interval itself grows because the next outer Present waits
for the previous secondary SDK Present. Feeding that inflated interval back to
XeFG creates a self-reinforcing ~20 ms cadence. v19 snapshots the recent native
pre-enable input cadence and keeps using that value for secondary async XeFG
when FTInput is Auto/Input. Explicit Opti/Zero modes remain untouched.
"""
from pathlib import Path

kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream' / 'OptiScaler'


def read(path):
    return (root / path).read_text(encoding='utf-8-sig')


def rep(text, old, new, count=1):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'v19 anchor count {actual} != {count}: {old[:140]}')
    return text.replace(old, new)

# Store a frozen native cadence plus the live raw cadence for diagnostics.
p = 'framegen/xefg/XeFG_Dx12.h'
s = read(p)
s = rep(s, '    MultiGPU::XeFGFrameTimeTelemetry _frameTimeTelemetry;', '''    MultiGPU::XeFGFrameTimeTelemetry _frameTimeTelemetry;
    std::atomic<double> _v19HeldInputFrameMs {0.0};
    std::atomic<double> _v19RawInputFrameMs {0.0};''')
(root / p).write_text(s, encoding='utf-8')

p = 'framegen/xefg/XeFG_Dx12.cpp'
s = read(p)
s = rep(s, '#include <framegen/XeFGLatencyPolicy.h>', '#include <framegen/XeFGLatencyPolicy.h>\n#include <algorithm>\n#include <cmath>')

# At the exact moment XeFG is enabled, the buffer ring still contains recent
# native/no-FG frame-time samples. Average valid slots and cap only pathological
# menu/loading values. 10 ms means we never ask this 6600 XT experiment to pace
# above 100 source FPS solely because of a stale menu sample.
anchor = '    auto currentFeature = State::Instance().currentFeature;'
insert = '''    if (IsMultiGPUActive() && Config::Instance()->FGXeFGAsyncPresent.value_or_default())
    {
        double sumMs = 0.0;
        unsigned samples = 0;
        for (unsigned i = 0; i < BUFFER_COUNT; ++i)
        {
            const double ms = _ftDelta[i];
            if (std::isfinite(ms) && ms >= 4.0 && ms <= 50.0)
            {
                sumMs += ms;
                ++samples;
            }
        }
        if (samples != 0)
        {
            const double heldMs = std::clamp(sumMs / static_cast<double>(samples), 10.0, 50.0);
            _v19HeldInputFrameMs.store(heldMs, std::memory_order_relaxed);
            LOG_INFO("MultiGPU v19: captured pre-FG input cadence {:.3f} ms ({:.1f} FPS) from {} slots",
                     heldMs, 1000.0 / heldMs, samples);
        }
        else
        {
            LOG_WARN("MultiGPU v19: no valid pre-FG frame-time samples; falling back to live Input cadence");
        }
    }

'''
s = rep(s, anchor, insert + anchor)

old = '''    const float sourceFrameMs = static_cast<float>(_ftDelta[fIndex]);
    const float applicationPresentMs = static_cast<float>(state.lastFGFrameTime);
    const auto frameTime = MultiGPU::SelectXeFGFrameTime(requestedFrameTime,
        IsMultiGPUActive() && MultiGPU::XeFGPresentScope::Allowed(), sourceFrameMs, applicationPresentMs);'''
new = '''    const float rawSourceFrameMs = static_cast<float>(_ftDelta[fIndex]);
    _v19RawInputFrameMs.store(rawSourceFrameMs, std::memory_order_relaxed);
    float sourceFrameMs = rawSourceFrameMs;
    const bool secondaryAsync = IsMultiGPUActive() && MultiGPU::XeFGPresentScope::Allowed() &&
                                Config::Instance()->FGXeFGAsyncPresent.value_or_default();
    const bool inputCadenceRequested = !requestedFrameTime.has_value() || requestedFrameTime.value() == 0;
    const double heldInputMs = _v19HeldInputFrameMs.load(std::memory_order_relaxed);
    if (secondaryAsync && inputCadenceRequested && std::isfinite(heldInputMs) && heldInputMs > 0.0)
        sourceFrameMs = static_cast<float>(heldInputMs);
    const float applicationPresentMs = static_cast<float>(state.lastFGFrameTime);
    const auto frameTime = MultiGPU::SelectXeFGFrameTime(requestedFrameTime,
        secondaryAsync, sourceFrameMs, applicationPresentMs);'''
s = rep(s, old, new)

# Log once per existing one-second telemetry batch. This makes it obvious
# whether the feedback loop is actually broken: raw may rise while held stays
# near the no-FG cadence.
anchor = '''                LOG_INFO("MultiGPU v17 latency: armed={}, externalSleepBypass={}",
                         latencyPolicy.armLatencyReduction, latencyPolicy.bypassExternalSleep);
            }'''
replacement = anchor + '''
            {
                const double heldMs = _v19HeldInputFrameMs.load(std::memory_order_relaxed);
                const double rawMs = _v19RawInputFrameMs.load(std::memory_order_relaxed);
                LOG_INFO("MultiGPU v19 cadence: heldInputMs={:.3f}, rawInputMs={:.3f}, heldFPS={:.1f}",
                         heldMs, rawMs, heldMs > 0.0 ? 1000.0 / heldMs : 0.0);
            }'''
s = rep(s, anchor, replacement)

(root / p).write_text(s, encoding='utf-8')
print('v19 applied: secondary async XeFG holds the native pre-enable Input cadence to break Present feedback pacing')
