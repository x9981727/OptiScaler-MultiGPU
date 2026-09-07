"""Restore XeLL latency-reduction arming while retaining the v16 external sleep bypass."""
from pathlib import Path

kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream' / 'OptiScaler'


def read(path):
    return (root / path).read_text(encoding='utf-8-sig')


def rep(text, old, new, count=1):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'v17 anchor count {actual} != {count}: {old[:120]}')
    return text.replace(old, new)

p = 'framegen/xefg/XeFG_Dx12.cpp'
s = read(p)
s = rep(s,
        '#include <framegen/XeFGNoWaitPolicy.h>',
        '#include <framegen/XeFGNoWaitPolicy.h>\n#include <framegen/XeFGLatencyPolicy.h>')

old = '''            xell_sleep_params_t sleepParams = {};
            const bool bypassSecondaryPacing = MultiGPU::ShouldBypassSecondaryXeFGPacing(
                IsMultiGPUActive(), Config::Instance()->FGXeFGAsyncPresent.value_or_default());
            sleepParams.bLowLatencyMode = !bypassSecondaryPacing;
            sleepParams.bLowLatencyBoost = false;
            sleepParams.minimumIntervalUs = 0;
            if (bypassSecondaryPacing)
                LOG_INFO("MultiGPU v16: secondary XeFG no-wait pacing active; XeLL sleep mode bypassed");'''

new = '''            xell_sleep_params_t sleepParams = {};
            const auto latencyPolicy = MultiGPU::SelectXeFGLatencyPolicy(
                IsMultiGPUActive(), Config::Instance()->FGXeFGAsyncPresent.value_or_default());
            sleepParams.bLowLatencyMode = latencyPolicy.armLatencyReduction;
            sleepParams.bLowLatencyBoost = false;
            sleepParams.minimumIntervalUs = 0;
            if (latencyPolicy.bypassExternalSleep)
                LOG_INFO("MultiGPU v17: XeLL latency reduction armed; external sleep pacing bypassed");'''

s = rep(s, old, new)

anchor = '''            LOG_INFO("MultiGPU v16 pacing: asyncSetting={}, multiGPUActive={}, noWait={}",
                     Config::Instance()->FGXeFGAsyncPresent.value_or_default(), IsMultiGPUActive(),
                     MultiGPU::ShouldBypassSecondaryXeFGPacing(IsMultiGPUActive(), Config::Instance()->FGXeFGAsyncPresent.value_or_default()));'''

replacement = anchor + '''
            {
                const auto latencyPolicy = MultiGPU::SelectXeFGLatencyPolicy(
                    IsMultiGPUActive(), Config::Instance()->FGXeFGAsyncPresent.value_or_default());
                LOG_INFO("MultiGPU v17 latency: armed={}, externalSleepBypass={}",
                         latencyPolicy.armLatencyReduction, latencyPolicy.bypassExternalSleep);
            }'''

s = rep(s, anchor, replacement)
(root / p).write_text(s, encoding='utf-8')
(root / 'framegen/XeFGLatencyPolicy.h').write_text((kit / 'v17' / 'XeFGLatencyPolicy.h').read_text(encoding='utf-8'), encoding='utf-8')
print('v17 applied: XeLL latency reduction stays armed while secondary async external sleep pacing remains bypassed')
