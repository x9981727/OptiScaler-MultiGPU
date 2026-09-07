"""v19: feed XeFG a render cadence that excludes the previous deferred Present wait.

v18 fixes the zero-frame-time feedback loop, but telemetry still shows ~50-53
render FPS while the same scene is ~75-82 FPS with XeFG inactive. The remaining
loop is that FTInput=Input is measured after the v14 deferred Present wait, so
XeFG is told ~19-20 ms even though the render path was already running faster.

v19 learns a baseline from the time between the previous outer Present return and
the next outer Present entry while XeFG is inactive. Once XeFG is active, that
baseline is frozen against artificial downward collapse caused by deferred
Present blocking; it may only adapt slowly upward if the render workload becomes
heavier. Secondary async XeFG uses this value for Auto/Input pacing only.
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

p = 'framegen/IFGFeature_Dx12.h'
s = read(p)
s = rep(s,
        '    virtual void CaptureSDKPresentStatus() {}',
        '    virtual void CaptureSDKPresentStatus() {}\n    virtual void RecordMultiGPURenderGap(double) {}')
(root / p).write_text(s, encoding='utf-8')

p = 'framegen/xefg/XeFG_Dx12.h'
s = read(p)
s = rep(s,
        '    std::atomic<double> _inputRecordCpuMs {0};',
        '    std::atomic<double> _inputRecordCpuMs {0};\n    std::atomic<double> _unblockedRenderGapMs {0};')
s = rep(s,
        '    void CaptureSDKPresentStatus() override final;',
        '    void CaptureSDKPresentStatus() override final;\n    void RecordMultiGPURenderGap(double ms) override final;')
(root / p).write_text(s, encoding='utf-8')

p = 'wrapped/wrapped_swapchain.h'
s = read(p)
s = rep(s,
        '    int _lastAsyncEligibility = -1;',
        '    int _lastAsyncEligibility = -1;\n    double _lastOuterPresentReturnMs = 0.0;')
(root / p).write_text(s, encoding='utf-8')

p = 'wrapped/wrapped_swapchain.cpp'
s = read(p)
anchor = '''    // Previous SDK Present must finish before destination-buffer access, SDK
    // resource tagging, SetPresentId, or a TEST Present for the next application frame.
    const HRESULT previousPresent = FinishMultiGPUQueuedPresent(true);'''
insert = '''    const double v19PresentEntryMs = Util::MillisecondsNow();
    if (_lastOuterPresentReturnMs > 0.0 && _multiGpuVirtualBackbufferRequested)
    {
        const double renderGapMs = v19PresentEntryMs - _lastOuterPresentReturnMs;
        auto v19fg = State::Instance().currentFG;
        if (v19fg != nullptr && v19fg->UsesSDKPresentRates())
            v19fg->RecordMultiGPURenderGap(renderGapMs);
    }
    struct V19PresentReturnStamp
    {
        double& stamp;
        ~V19PresentReturnStamp() { stamp = Util::MillisecondsNow(); }
    } v19PresentReturnStamp {_lastOuterPresentReturnMs};

''' + anchor
s = rep(s, anchor, insert, count=2)
(root / p).write_text(s, encoding='utf-8')

p = 'framegen/xefg/XeFG_Dx12.cpp'
s = read(p)
s = rep(s,
        '#include <framegen/XeFGLatencyPolicy.h>',
        '#include <framegen/XeFGLatencyPolicy.h>\n#include <algorithm>\n#include <cmath>')

method_anchor = 'bool XeFG_Dx12::QueueXeFGPresent(std::function<HRESULT()> call)'
method = '''void XeFG_Dx12::RecordMultiGPURenderGap(double ms)
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
    // While FG is active, a much smaller gap is usually the symptom of the
    // previous Present consuming the rest of the frame. Never train the pacing
    // baseline downward from that feedback. A genuinely heavier render path can
    // still raise the baseline slowly.
    if (ms > previous * 1.10)
        _unblockedRenderGapMs.store(previous * 0.90 + ms * 0.10, std::memory_order_relaxed);
}

'''
s = rep(s, method_anchor, method + method_anchor)

old = '''    const auto frameTime = MultiGPU::SelectXeFGFrameTime(requestedFrameTime,
        IsMultiGPUActive() && MultiGPU::XeFGPresentScope::Allowed(), sourceFrameMs, applicationPresentMs);
    constData.frameRenderTime = frameTime.sdkMs;
    if (IsMultiGPUActive())
        _frameTimeTelemetry.Record(frameTime, sourceFrameMs, applicationPresentMs);'''
new = '''    const bool secondaryAsyncFrame = IsMultiGPUActive() && MultiGPU::XeFGPresentScope::Allowed();
    auto frameTime = MultiGPU::SelectXeFGFrameTime(requestedFrameTime,
        secondaryAsyncFrame, sourceFrameMs, applicationPresentMs);
    const double unblockedRenderGap = _unblockedRenderGapMs.load(std::memory_order_relaxed);
    const bool inputMode = !requestedFrameTime.has_value() || requestedFrameTime.value() == 0;
    if (secondaryAsyncFrame && inputMode && std::isfinite(unblockedRenderGap) && unblockedRenderGap >= 1.0)
    {
        const float gapMs = static_cast<float>(unblockedRenderGap);
        if (!(frameTime.sdkMs > 0.0f) || gapMs < frameTime.sdkMs)
            frameTime.sdkMs = gapMs;
    }
    constData.frameRenderTime = frameTime.sdkMs;
    if (IsMultiGPUActive())
        _frameTimeTelemetry.Record(frameTime, sourceFrameMs, applicationPresentMs);'''
s = rep(s, old, new)

log_anchor = '            LOG_INFO("MultiGPU v14 present: asyncSetting={}, queued={}, completed={}, nextPresentWaitCPU={:.3f} ms, waitCalls={}, pendingResult={:X}",'
log_insert = '''            LOG_INFO("MultiGPU v19 frameTime: unblockedRenderGap={:.3f} ms", _unblockedRenderGapMs.load(std::memory_order_relaxed));
''' + log_anchor
s = rep(s, log_anchor, log_insert)
(root / p).write_text(s, encoding='utf-8')

print('v19 applied: secondary XeFG Input pacing uses a pre-block render cadence baseline')
