"""v20: freeze the pre-FG render cadence while secondary XeFG is active.

v19 allowed the baseline to rise slowly during active XeFG when a larger gap was
observed. Real-game telemetry shows those larger gaps are still contaminated by
the deferred secondary Present wait: the same scene learns ~13.6 ms with XeFG
off, then drifts to ~15.9 ms after activation while PresentWait remains ~13 ms.
That reintroduces a pacing feedback loop and leaves the render rate near 59 FPS.

v20 only trains the unblocked-render baseline while XeFG is inactive. While FG
is active the last clean pre-activation baseline is held constant. This is a
small, reversible pacing change; explicit Present/Zero frame-time modes remain
untouched.
"""
from pathlib import Path
import subprocess
import sys

kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream' / 'OptiScaler'


def read(path):
    return (root / path).read_text(encoding='utf-8-sig')


def rep(text, old, new, count=1):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'v20 anchor count {actual} != {count}: {old[:140]}')
    return text.replace(old, new)

p = 'framegen/xefg/XeFG_Dx12.cpp'
s = read(p)
old = '''void XeFG_Dx12::RecordMultiGPURenderGap(double ms)
{
    if (!std::isfinite(ms) || ms < 1.0 || ms > 50.0)
        return;
    const double previous = _unblockedRenderGapMs.load(std::memory_order_relaxed);
    if (previous <= 0.0 || !IsActive())
    {
        const double smoothed = previous > 0.0 ? previous * 0.80 + ms * 0.20 : ms;
        _unblockedRenderGapMs.store(smoothed, std::memory_order_relaxed);
        return;
    }
    if (ms > previous * 1.10)
        _unblockedRenderGapMs.store(previous * 0.90 + ms * 0.10, std::memory_order_relaxed);
}
'''
new = '''void XeFG_Dx12::RecordMultiGPURenderGap(double ms)
{
    if (!std::isfinite(ms) || ms < 1.0 || ms > 50.0)
        return;

    // v20: once secondary XeFG is active, every outer-Present gap can contain
    // deferred SDK Present pressure. Do not let that feedback retrain the clean
    // render cadence. Training resumes automatically after XeFG is disabled.
    if (IsActive())
        return;

    const double previous = _unblockedRenderGapMs.load(std::memory_order_relaxed);
    const double smoothed = previous > 0.0 ? previous * 0.80 + ms * 0.20 : ms;
    _unblockedRenderGapMs.store(smoothed, std::memory_order_relaxed);
}
'''
s = rep(s, old, new)

anchor = '            LOG_INFO("MultiGPU v19 frameTime: unblockedRenderGap={:.3f} ms", _unblockedRenderGapMs.load(std::memory_order_relaxed));'
replacement = anchor + '\n            LOG_INFO("MultiGPU v20 frameTime: pre-FG baseline frozen while active={}", IsActive());'
s = rep(s, anchor, replacement)
(root / p).write_text(s, encoding='utf-8')

print('v20 applied: pre-FG render cadence is frozen for the lifetime of active secondary XeFG')
subprocess.run([sys.executable, str(kit / 'scripts' / 'apply-v21.py')], check=True)
subprocess.run([sys.executable, str(kit / 'scripts' / 'apply-v22.py')], check=True)
