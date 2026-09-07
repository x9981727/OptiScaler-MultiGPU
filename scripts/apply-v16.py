"""Apply v16 secondary-XeFG no-wait pacing experiment after v15."""
from pathlib import Path

kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream' / 'OptiScaler'
changes = {}

def read(path):
    return (root / path).read_text(encoding='utf-8-sig')

def rep(s, old, new, count=1):
    actual = s.count(old)
    if actual != count:
        raise RuntimeError(f'v16 anchor count {actual} != {count}: {old[:120]}')
    return s.replace(old, new)

# Scope the Reflex/XeLL sleep bypass to active secondary XeFG + async Present only.
p = 'hooks/Reflex_Hooks.cpp'
s = read(p)
s = rep(s, '#include <nvapi/fakenvapi.h>', '#include <nvapi/fakenvapi.h>\n#include <framegen/XeFGNoWaitPolicy.h>')
old = '''    auto fg = State::Instance().currentFG;
    const bool measureXeFG = fg != nullptr && fg->UsesSDKPresentRates();
    const double started = measureXeFG ? Util::MillisecondsNow() : 0;
    const NvAPI_Status result = State::Instance().activeFgOutput == FGOutput::XeFG && fakenvapi::ForNvidia_Sleep
        ? fakenvapi::ForNvidia_Sleep(pDev) : o_NvAPI_D3D_Sleep(pDev);
    if (measureXeFG)
        fg->RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase::Sleep, Util::MillisecondsNow() - started);
    return result;'''
new = '''    auto fg = State::Instance().currentFG;
    const bool measureXeFG = fg != nullptr && fg->UsesSDKPresentRates();
    const bool bypassSecondaryPacing = fg != nullptr &&
        MultiGPU::ShouldBypassSecondaryXeFGPacing(
            fg->MultiGPUActive(), Config::Instance()->FGXeFGAsyncPresent.value_or_default());
    const double started = measureXeFG ? Util::MillisecondsNow() : 0;
    NvAPI_Status result;
    if (bypassSecondaryPacing)
    {
        result = NVAPI_OK;
    }
    else
    {
        result = State::Instance().activeFgOutput == FGOutput::XeFG && fakenvapi::ForNvidia_Sleep
            ? fakenvapi::ForNvidia_Sleep(pDev) : o_NvAPI_D3D_Sleep(pDev);
    }
    if (measureXeFG)
        fg->RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase::Sleep, Util::MillisecondsNow() - started);
    return result;'''
s = rep(s, old, new)
changes[p] = s

# Keep XeLL context/markers/latency reduction intact, but do not advertise low-latency
# sleep pacing inside the secondary async context. Single-GPU and sync fallback unchanged.
p = 'framegen/xefg/XeFG_Dx12.cpp'
s = read(p)
s = rep(s, '#include <framegen/XeFGPresentPolicy.h>', '#include <framegen/XeFGPresentPolicy.h>\n#include <framegen/XeFGNoWaitPolicy.h>')
old = '''            xell_sleep_params_t sleepParams = {};
            sleepParams.bLowLatencyMode = true;
            sleepParams.bLowLatencyBoost = false;
            sleepParams.minimumIntervalUs = 0;'''
new = '''            xell_sleep_params_t sleepParams = {};
            const bool bypassSecondaryPacing = MultiGPU::ShouldBypassSecondaryXeFGPacing(
                IsMultiGPUActive(), Config::Instance()->FGXeFGAsyncPresent.value_or_default());
            sleepParams.bLowLatencyMode = !bypassSecondaryPacing;
            sleepParams.bLowLatencyBoost = false;
            sleepParams.minimumIntervalUs = 0;
            if (bypassSecondaryPacing)
                LOG_INFO("MultiGPU v16: secondary XeFG no-wait pacing active; XeLL sleep mode bypassed");'''
s = rep(s, old, new)
anchor = '            LOG_INFO("MultiGPU v14 present: asyncSetting={}, queued={}, completed={}, nextPresentWaitCPU={:.3f} ms, waitCalls={}, pendingResult={:X}",' 
s = rep(s, anchor, '            LOG_INFO("MultiGPU v16 pacing: asyncSetting={}, multiGPUActive={}, noWait={}",\n                     Config::Instance()->FGXeFGAsyncPresent.value_or_default(), IsMultiGPUActive(),\n                     MultiGPU::ShouldBypassSecondaryXeFGPacing(IsMultiGPUActive(), Config::Instance()->FGXeFGAsyncPresent.value_or_default()));\n' + anchor)
changes[p] = s

changes['framegen/XeFGNoWaitPolicy.h'] = (kit / 'v16' / 'XeFGNoWaitPolicy.h').read_text(encoding='utf-8')
for path, text in changes.items():
    (root / path).write_text(text, encoding='utf-8')
print('v16 applied: secondary async XeFG pacing bypass with scoped telemetry; sync and single-GPU paths preserved')
