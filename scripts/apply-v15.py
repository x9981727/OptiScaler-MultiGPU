"""Fix frame-time setting persistence and scoped secondary XeFG pacing metadata."""
from pathlib import Path
kit = Path(__file__).resolve().parents[1]
root = kit/'upstream/OptiScaler'
changes = {}
def rep(s, old, new):
    if s.count(old) != 1:
        raise RuntimeError(f'v15 anchor count {s.count(old)} != 1: {old[:120]}')
    return s.replace(old, new)

p = 'Config.cpp'
s = (root/p).read_text(encoding='utf-8-sig')
s = rep(s, '#include <SimpleIni.h>', '#include <SimpleIni.h>\n#include <framegen/XeFGFrameTime.h>')
old = '''            auto ftInput = readInt("FrameGen", "FTSource");
            if (ftInput.has_value() && ftInput.value() >= 0 &&
                ftInput.value() <= (FGOutput.value_or_default() == FGOutput::XeFG ? 2 : 1))
            {
                FTInput.set_from_config(static_cast<FrameTimeSource>(ftInput.value()));
            }'''
new = '''            const auto ftInput = MultiGPU::ReadFrameTimeSetting(
                ini.GetValue("FrameGen", "FTInput", nullptr), ini.GetValue("FrameGen", "FTSource", nullptr),
                FGOutput.value_or_default() == FGOutput::XeFG);
            FTInput.set_from_config(ftInput.has_value()
                ? std::optional<FrameTimeSource>(static_cast<FrameTimeSource>(*ftInput)) : std::nullopt);
            LOG_INFO("MultiGPU v15 config: FTInput={}, legacyUsed={}", ftInput.value_or(-1),
                     ini.GetValue("FrameGen", "FTInput", nullptr) == nullptr &&
                     ini.GetValue("FrameGen", "FTSource", nullptr) != nullptr);'''
s = rep(s, old, new)
changes[p] = s

p = 'framegen/xefg/XeFG_Dx12.h'
s = (root/p).read_text(encoding='utf-8-sig')
s = rep(s, '#include <framegen/DeferredPresent.h>', '#include <framegen/DeferredPresent.h>\n#include <framegen/XeFGFrameTime.h>')
s = rep(s, '    UINT64 _deferredQueued = 0, _deferredCompleted = 0;',
        '    UINT64 _deferredQueued = 0, _deferredCompleted = 0;\n    MultiGPU::XeFGFrameTimeTelemetry _frameTimeTelemetry;')
changes[p] = s

p = 'framegen/xefg/XeFG_Dx12.cpp'
s = (root/p).read_text(encoding='utf-8-sig')
s = rep(s, '#include "XeFG_Dx12.h"', '#include "XeFG_Dx12.h"\n#include <framegen/XeFGPresentPolicy.h>')
start = s.index('    switch (Config::Instance()->FTInput.value_or_default())')
end = s.index('    LOG_DEBUG("Reset: {}, Opti FT:', start)
assert 'constData.frameRenderTime = 0.0f;' in s[start:end]
new = '''    static_assert(static_cast<int>(FrameTimeSource::Input) == 0 &&
                  static_cast<int>(FrameTimeSource::Opti) == 1 &&
                  static_cast<int>(FrameTimeSource::Zero) == 2);
    std::optional<int> requestedFrameTime;
    if (Config::Instance()->FTInput.has_value())
        requestedFrameTime = static_cast<int>(Config::Instance()->FTInput.value());
    const float sourceFrameMs = static_cast<float>(_ftDelta[fIndex]);
    const float applicationPresentMs = static_cast<float>(state.lastFGFrameTime);
    const auto frameTime = MultiGPU::SelectXeFGFrameTime(requestedFrameTime,
        IsMultiGPUActive() && MultiGPU::XeFGPresentScope::Allowed(), sourceFrameMs, applicationPresentMs);
    constData.frameRenderTime = frameTime.sdkMs;
    if (IsMultiGPUActive())
        _frameTimeTelemetry.Record(frameTime, sourceFrameMs, applicationPresentMs);

'''
s = s[:start] + new + s[end:]
anchor = '            _deferredQueued = _deferredCompleted = 0;'
s = rep(s, anchor, anchor + '''
            const auto frameTime = _frameTimeTelemetry.Take();
            LOG_INFO("MultiGPU v15 frameTime: requested={}, lastSource={}, frames={}, inputFrames={}, presentFrames={}, zeroFrames={}, invalidFrames={}, sdkFrameMs={:.3f}, inputFrameMs={:.3f}, applicationPresentMs={:.3f}",
                     frameTime.requested, frameTime.source, frameTime.frames, frameTime.inputFrames,
                     frameTime.presentFrames, frameTime.zeroFrames, frameTime.invalidFrames,
                     frameTime.SdkMean(), frameTime.InputMean(), frameTime.PresentMean());''')
changes[p] = s
changes['framegen/XeFGFrameTime.h'] = (kit/'v15/XeFGFrameTime.h').read_text()
# Validate all anchors before writing any source file.
for p, s in changes.items(): (root/p).write_text(s, encoding='utf-8')
print('v15 applied: canonical FTInput reload, legacy compatibility, scoped unknown frame-time pacing and diagnostics')
