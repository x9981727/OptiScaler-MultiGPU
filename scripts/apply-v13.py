"""Add v13 timing diagnostics to the exact reconstructed v12 sources."""
from pathlib import Path
kit=Path(__file__).resolve().parents[1]
root=kit/'upstream/OptiScaler'
changes={}
def replace(s,old,new,count=1):
    if s.count(old)!=count:
        raise RuntimeError(f'v13 anchor count {s.count(old)} != {count}: {old[:90]}')
    return s.replace(old,new)

p='framegen/IFGFeature_Dx12.h'
s=(root/p).read_text(encoding='utf-8-sig')
s=replace(s,'#include "MultiGPU_Dx12.h"','#include "MultiGPU_Dx12.h"\n#include "XeFGTimingCounters.h"')
s=replace(s,'    virtual void CaptureSDKPresentStatus() {}','''    virtual void CaptureSDKPresentStatus() {}
    virtual void RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase phase, double ms) {}
    virtual bool BeginXeFGGpuTiming() { return false; }
    virtual void EndXeFGGpuTiming() {}
    virtual bool BeginXeFGTransferTiming(bool renderSide, ID3D12CommandQueue* queue) { return false; }
    virtual void EndXeFGTransferTiming(bool renderSide) {}''')
changes[p]=s

p='framegen/xefg/XeFG_Dx12.h'
s=(root/p).read_text(encoding='utf-8-sig')
s=replace(s,'#include <framegen/XeFGPresentationStats.h>','#include <framegen/XeFGPresentationStats.h>\n#include <framegen/QueueSpanProbe_Dx12.h>')
s=replace(s,'    double _inputRecordCpuMs = 0;','''    double _inputRecordCpuMs = 0;
    MultiGPU::XeFGTimingCounters _cpuTimings;
    MultiGPU::QueueSpanProbe _sdkGpuSpan;
    MultiGPU::QueueSpanProbe _renderExportSpan;
    MultiGPU::QueueSpanProbe _fgWaitImportSpan;''')
s=replace(s,'    void CaptureSDKPresentStatus() override final;','''    void CaptureSDKPresentStatus() override final;
    void RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase phase, double ms) override final
    { if (IsMultiGPUActive()) _cpuTimings.Add(phase, ms); }
    bool BeginXeFGGpuTiming() override final;
    void EndXeFGGpuTiming() override final;
    bool BeginXeFGTransferTiming(bool renderSide, ID3D12CommandQueue* queue) override final
    { return IsMultiGPUActive() && (renderSide ? _renderExportSpan : _fgWaitImportSpan).Begin(queue); }
    void EndXeFGTransferTiming(bool renderSide) override final
    { if (IsMultiGPUActive()) (renderSide ? _renderExportSpan : _fgWaitImportSpan).End(); }''')
changes[p]=s

p='framegen/xefg/XeFG_Dx12.cpp'
s=(root/p).read_text(encoding='utf-8-sig')
s=replace(s,'void XeFG_Dx12::CaptureSDKPresentStatus()','''bool XeFG_Dx12::BeginXeFGGpuTiming()
{
    return IsMultiGPUActive() && _sdkGpuSpan.Begin(_multiGpuRuntime->FGQueue());
}

void XeFG_Dx12::EndXeFGGpuTiming()
{
    if (IsMultiGPUActive()) _sdkGpuSpan.End();
}

void XeFG_Dx12::CaptureSDKPresentStatus()''')
s=replace(s,'            _inputRecordCpuMs = 0;','''            _inputRecordCpuMs = 0;
            const auto cpu = _cpuTimings.Take();
            const auto gpu = _sdkGpuSpan.Take();
            const auto renderCopy = _renderExportSpan.Take();
            const auto fgCopy = _fgWaitImportSpan.Take();
            const auto mean = [&](MultiGPU::XeFGCpuPhase phase) { return cpu[static_cast<size_t>(phase)].Mean(); };
            xell_sleep_params_t sleep {};
            const auto getSleep = XeLLProxy::GetSleepMode();
            const int sleepResult = getSleep != nullptr && XeLLProxy::Context() != nullptr
                ? static_cast<int>(getSleep(XeLLProxy::Context(), &sleep)) : -999;
            LOG_INFO("MultiGPU v13 timing: active={}, sdkQueueSpanGPU={:.3f} ms, gpuSamples={}, gpuSkipped={}, gpuStatus={:X}, renderExportGPU={:.3f} ms, exportSamples={}, exportStatus={:X}, fgWaitImportGPU={:.3f} ms, importSamples={}, importStatus={:X}, proxyPresentCPU={:.3f} ms, bridgeCPU={:.3f} ms, bridgeFGSlotCPU={:.3f} ms, bridgeRenderSlotCPU={:.3f} ms, overlayCPU={:.3f} ms, locksCPU={:.3f} ms, reflexSleepCPU={:.3f} ms, sleepCalls={}, presentCalls={}, xellQuery={}, xellMinIntervalUs={}, xellLowLatency={}, optiFpsLimit={}",
                     status.isFrameGenEnabled, gpu.meanMs, gpu.samples, gpu.skipped, static_cast<UINT>(gpu.error),
                     renderCopy.meanMs, renderCopy.samples, static_cast<UINT>(renderCopy.error),
                     fgCopy.meanMs, fgCopy.samples, static_cast<UINT>(fgCopy.error),
                     mean(MultiGPU::XeFGCpuPhase::ProxyPresent), mean(MultiGPU::XeFGCpuPhase::Bridge),
                     mean(MultiGPU::XeFGCpuPhase::FGSlot), mean(MultiGPU::XeFGCpuPhase::RenderSlot),
                     mean(MultiGPU::XeFGCpuPhase::Overlay), mean(MultiGPU::XeFGCpuPhase::Locks),
                     mean(MultiGPU::XeFGCpuPhase::Sleep), cpu[static_cast<size_t>(MultiGPU::XeFGCpuPhase::Sleep)].calls,
                     cpu[static_cast<size_t>(MultiGPU::XeFGCpuPhase::ProxyPresent)].calls, sleepResult,
                     sleep.minimumIntervalUs, static_cast<UINT>(sleep.bLowLatencyMode),
                     Config::Instance()->FramerateLimit.value_or_default());''')
s=replace(s,'    const auto drainResult = _inputTransfers.Drain();','''    _renderExportSpan.Drain();
    _fgWaitImportSpan.Drain();
    const auto probeDrain = _sdkGpuSpan.Drain();
    if (FAILED(probeDrain))
        LOG_WARN("MultiGPU v13: GPU timing drain failed: {:X}", static_cast<UINT>(probeDrain));
    const auto drainResult = _inputTransfers.Drain();''')
changes[p]=s

p='hooks/FG_Hooks.cpp'
s=(root/p).read_text(encoding='utf-8-sig')
start=s.index('HRESULT FGHooks::FGPresent(')
end=s.index('ULONG FGHooks::hkFGRelease(',start)
part=s[start:end]
part=replace(part,'''    HRESULT result;
    if (pPresentParameters == nullptr)''','''    const bool measureXeFG = willPresent && fg != nullptr && fg->UsesSDKPresentRates();
    const bool gpuTimingStarted = measureXeFG && fg->BeginXeFGGpuTiming();
    const double proxyStarted = measureXeFG ? Util::MillisecondsNow() : 0;
    HRESULT result;
    if (pPresentParameters == nullptr)''')
part=replace(part,'''    // Query the SDK only after the application proxy Present completes; test''','''    if (measureXeFG)
    {
        const double proxyMs = Util::MillisecondsNow() - proxyStarted;
        if (gpuTimingStarted) fg->EndXeFGGpuTiming();
        fg->RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase::ProxyPresent, proxyMs);
    }

    // Query the SDK only after the application proxy Present completes; test''')
s=s[:start]+part+s[end:]
changes[p]=s

p='hooks/Reflex_Hooks.cpp'
s=(root/p).read_text(encoding='utf-8-sig')
old='''    if (State::Instance().activeFgOutput == FGOutput::XeFG && fakenvapi::ForNvidia_Sleep)
        return fakenvapi::ForNvidia_Sleep(pDev);
    else
        return o_NvAPI_D3D_Sleep(pDev);'''
new='''    auto fg = State::Instance().currentFG;
    const bool measureXeFG = fg != nullptr && fg->UsesSDKPresentRates();
    const double started = measureXeFG ? Util::MillisecondsNow() : 0;
    const NvAPI_Status result = State::Instance().activeFgOutput == FGOutput::XeFG && fakenvapi::ForNvidia_Sleep
        ? fakenvapi::ForNvidia_Sleep(pDev) : o_NvAPI_D3D_Sleep(pDev);
    if (measureXeFG)
        fg->RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase::Sleep, Util::MillisecondsNow() - started);
    return result;'''
s=replace(s,old,new)
changes[p]=s

p='wrapped/wrapped_swapchain.cpp'
s=(root/p).read_text(encoding='utf-8-sig')
s=replace(s,'static HRESULT LocalPresent(','''namespace
{
class XeFGTransferTimingScope
{
    IFGFeature_Dx12* _fg;
    bool _renderSide;
    bool _began = false;
  public:
    XeFGTransferTimingScope(IFGFeature_Dx12* fg, bool renderSide, ID3D12CommandQueue* queue)
        : _fg(fg), _renderSide(renderSide)
    { _began = _fg != nullptr && _fg->BeginXeFGTransferTiming(_renderSide, queue); }
    ~XeFGTransferTimingScope()
    { if (_began) _fg->EndXeFGTransferTiming(_renderSide); }
};
}

static HRESULT LocalPresent(''')
for begin,end in [('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present(',
                  'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer('),
                 ('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(',
                  'BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain4::IsTemporaryMonoSupported(')]:
    start=s.index(begin);stop=s.index(end,start);part=s[start:stop]
    part=replace(part,'#ifdef USE_LOCAL_MUTEX','''    const double lockStarted = _multiGpuVirtualBackbufferRequested ? Util::MillisecondsNow() : 0;
#ifdef USE_LOCAL_MUTEX''')
    part=replace(part,'    HRESULT result;','''    if (presentFG != nullptr && (Flags & DXGI_PRESENT_TEST) == 0)
        presentFG->RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase::Locks, Util::MillisecondsNow() - lockStarted);
    HRESULT result;''')
    for params in ['nullptr','pPresentParameters']:
        overlay=f'            MenuOverlayDx::Present(this, SyncInterval, Flags, {params}, _device, _handle, _uwp);'
        if overlay in part:
            part=replace(part,overlay,'''            const double overlayStarted = Util::MillisecondsNow();
'''+overlay+'''
            if (presentFG != nullptr)
                presentFG->RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase::Overlay, Util::MillisecondsNow() - overlayStarted);
            const double bridgeStarted = Util::MillisecondsNow();''')
    part=replace(part,'                return DXGI_ERROR_DEVICE_REMOVED;\n            result = LocalPresent(','''                return DXGI_ERROR_DEVICE_REMOVED;
            if (presentFG != nullptr)
                presentFG->RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase::Bridge, Util::MillisecondsNow() - bridgeStarted);
            result = LocalPresent(''')
    s=s[:start]+part+s[stop:]
start=s.index('bool WrappedIDXGISwapChain4::TransferMultiGPUVirtualBackbuffer()')
end=s.index('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::QueryInterface',start)
part=s[start:end]
part=replace(part,'    auto fgCmd = _multiGpuRuntime->BeginFGTransfer(index);','''    const double fgSlotStarted = Util::MillisecondsNow();
    auto fgCmd = _multiGpuRuntime->BeginFGTransfer(index);
    auto fg = State::Instance().currentFG;
    if (fg != nullptr)
        fg->RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase::FGSlot, Util::MillisecondsNow() - fgSlotStarted);''')
part=replace(part,'    auto renderCmd = _multiGpuRuntime->BeginRenderTransfer(index, _multiGpuRenderQueue.Get());','''    const double renderSlotStarted = Util::MillisecondsNow();
    auto renderCmd = _multiGpuRuntime->BeginRenderTransfer(index, _multiGpuRenderQueue.Get());
    if (fg != nullptr)
        fg->RecordXeFGCpuTiming(MultiGPU::XeFGCpuPhase::RenderSlot, Util::MillisecondsNow() - renderSlotStarted);''')
part=replace(part,'''    if (renderCmd == nullptr ||
        !_multiGpuBackbufferBridges[index]->RecordRenderCopy(renderCmd, _multiGpuVirtualBackbuffers[index].Get(),
                                                              D3D12_RESOURCE_STATE_PRESENT) ||
        !_multiGpuRuntime->EndRenderTransfer(index, _multiGpuRenderQueue.Get()))''','''    bool exported = false;
    {
        XeFGTransferTimingScope timing(fg, true, _multiGpuRenderQueue.Get());
        exported = renderCmd != nullptr &&
            _multiGpuBackbufferBridges[index]->RecordRenderCopy(renderCmd, _multiGpuVirtualBackbuffers[index].Get(),
                                                                D3D12_RESOURCE_STATE_PRESENT) &&
            _multiGpuRuntime->EndRenderTransfer(index, _multiGpuRenderQueue.Get());
    }
    if (!exported)''')
part=replace(part,'''    if (!_multiGpuRuntime->SignalRenderAndWaitOnFG(_multiGpuRenderQueue.Get()))''','''    XeFGTransferTimingScope importTiming(fg, false, _multiGpuRuntime->FGQueue());
    if (!_multiGpuRuntime->SignalRenderAndWaitOnFG(_multiGpuRenderQueue.Get()))''')
s=s[:start]+part+s[end:]
anchor='''    LOG_INFO("MultiGPU v6: {} main-GPU virtual backbuffers created for secondary proxy swapchain",'''
s=replace(s,anchor,'''    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    Microsoft::WRL::ComPtr<IDXGIAdapter> outputAdapter;
    DXGI_ADAPTER_DESC outputDesc {};
    const bool outputKnown = SUCCEEDED(_real->GetContainingOutput(&output)) && output != nullptr &&
        SUCCEEDED(output->GetParent(IID_PPV_ARGS(&outputAdapter))) && outputAdapter != nullptr &&
        SUCCEEDED(outputAdapter->GetDesc(&outputDesc));
    LOG_INFO("MultiGPU v13 topology: renderLuid={}, fgLuid={}, outputKnown={}, outputLuid={}",
             wstring_to_string(MultiGPU::LuidToString(_multiGpuRenderDevice->GetAdapterLuid())),
             wstring_to_string(MultiGPU::LuidToString(_multiGpuRuntime->FGDevice()->GetAdapterLuid())),
             outputKnown, wstring_to_string(MultiGPU::LuidToString(outputDesc.AdapterLuid)));
'''+anchor)
changes[p]=s

for name in ['QueueSpanProbe_Dx12.h','XeFGTimingCounters.h']:
    changes['framegen/'+name]=(kit/'v13'/name).read_text()
for path,content in changes.items():
    (root/path).write_text(content,encoding='utf-8')
print('v13 applied: nonblocking queue timestamps, CPU phase times, actual XeLL settings and output topology')
