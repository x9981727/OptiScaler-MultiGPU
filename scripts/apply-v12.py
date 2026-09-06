"""Apply the XeFG v12 transfer/measurement fix to the reconstructed v11 tree."""
from pathlib import Path

kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream/OptiScaler'
changes = {}

def replace(s, old, new, count=1):
    if s.count(old) != count:
        raise RuntimeError(f'v12 anchor count {s.count(old)} != {count}: {old[:100]}')
    return s.replace(old, new)

path = 'framegen/IFGFeature_Dx12.h'
s = (root / path).read_text(encoding='utf-8-sig')
s = replace(s, '    bool FlushMultiGPUResources(int index = -1);', '''    bool FlushMultiGPUResources(int index = -1);
    bool RecordMultiGPUResourceCopies(int index, ID3D12GraphicsCommandList* commandList);''')
s = replace(s, '    virtual bool MultiGPUAdapterChangePending() const { return false; }', '''    virtual bool MultiGPUAdapterChangePending() const { return false; }
    virtual void CaptureSDKPresentStatus() {}
    virtual bool UsesSDKPresentRates() const { return false; }
    virtual bool GetSDKPresentRates(float& renderFps, float& outputFps, bool& outputKnown) { return false; }''')
changes[path] = s

path = 'framegen/IFGFeature_Dx12.cpp'
s = (root / path).read_text(encoding='utf-8-sig')
start = s.index('    for (auto& [type, bridge] : bridges)', s.index('bool IFGFeature_Dx12::FlushMultiGPUResources'))
end = s.index('    if (!_multiGpuRuntime->EndFGTransfer', start)
loop = s[start:end]
record_loop = replace(loop, '            _multiGpuRuntime->AbortFGTransfer(static_cast<UINT>(index));\n', '')
s = s[:start] + '''    if (!RecordMultiGPUResourceCopies(index, fgCmdList))
    {
        _multiGpuRuntime->AbortFGTransfer(static_cast<UINT>(index));
        return false;
    }

''' + s[end:]
method = '''bool IFGFeature_Dx12::RecordMultiGPUResourceCopies(int index, ID3D12GraphicsCommandList* fgCmdList)
{
    if (!IsMultiGPUActive() || fgCmdList == nullptr)
        return false;
    if (index < 0)
        index = GetIndex();
    index %= BUFFER_COUNT;
    auto& bridges = _multiGpuBridge[index];
''' + record_loop + '''    return true;
}

'''
s = replace(s, 'ID3D12Resource* IFGFeature_Dx12::GetMultiGPUResource(', method + 'ID3D12Resource* IFGFeature_Dx12::GetMultiGPUResource(')
changes[path] = s

path = 'framegen/xefg/XeFG_Dx12.h'
s = (root / path).read_text(encoding='utf-8-sig')
s = replace(s, '#include "AdapterSelectionBinding.h"', '''#include "AdapterSelectionBinding.h"
#include <framegen/XeFGTransferPool.h>
#include <framegen/XeFGPresentationStats.h>''')
s = replace(s, '    MultiGPU::AdapterSelectionBinding _adapterBinding;', '''    MultiGPU::AdapterSelectionBinding _adapterBinding;
    MultiGPU::XeFGTransferPool _inputTransfers;
    MultiGPU::XeFGPresentationStats _presentationStats;
    double _inputRecordCpuMs = 0;
    int _lastPresentQueryResult = 0;
    int _lastInterpolationResult = 0;''')
s = replace(s, '    bool MultiGPUAdapterChangePending() const override final;', '''    bool MultiGPUAdapterChangePending() const override final;
    void CaptureSDKPresentStatus() override final;
    bool UsesSDKPresentRates() const override final { return IsMultiGPUActive(); }
    bool GetSDKPresentRates(float& renderFps, float& outputFps, bool& outputKnown) override final;''')
changes[path] = s

path = 'framegen/xefg/XeFG_Dx12.cpp'
s = (root / path).read_text(encoding='utf-8-sig')
start = s.index('    if (IsMultiGPUActive())', s.index('bool XeFG_Dx12::Dispatch()'))
end = s.index('    if (XeFGProxy::SetUiCompositionState()', start)
s = s[:start] + '''    if (IsMultiGPUActive())
    {
        const double recordStarted = Util::MillisecondsNow();
        // Keep the cross-adapter GPU dependency. Copies and SDK tags are now
        // recorded together, rather than waiting on our just-submitted copy.
        if (!_multiGpuRuntime->SignalRenderAndWaitOnFG(_gameCommandQueue))
            return false;
        ID3D12GraphicsCommandList* tagCmdList = nullptr;
        auto transferResult = _inputTransfers.Begin(_multiGpuRuntime->FGQueue(), static_cast<UINT>(fIndex), &tagCmdList);
        if (FAILED(transferResult))
        {
            LOG_ERROR("MultiGPU v12: XeFG input batch begin failed: {:X}", (UINT) transferResult);
            return false;
        }
        // Even if a later tag fails, submit already-recorded copies/barriers so
        // bridge state tracking cannot describe transitions that never execute.
        const auto submit = [&]() {
            const auto hr = _inputTransfers.Submit(static_cast<UINT>(fIndex));
            if (FAILED(hr))
                LOG_ERROR("MultiGPU v12: XeFG input batch submit failed: {:X}", (UINT) hr);
            return SUCCEEDED(hr);
        };
        if (!RecordMultiGPUResourceCopies(fIndex, tagCmdList))
        {
            submit();
            return false;
        }
        const auto frameId = static_cast<uint32_t>(willDispatchFrame);
        for (auto type : { FG_ResourceType::Depth, FG_ResourceType::Velocity })
        {
            auto resourceParam = GetResourceData(type, fIndex);
            if (resourceParam.pResource == nullptr)
            {
                submit();
                return false;
            }
            const auto tagResult = XeFGProxy::D3D12TagFrameResource()(_swapChainContext, tagCmdList, frameId, &resourceParam);
            LOG_DEBUG("MultiGPU v12: XeFG batched resource tag frameId: {}, type: {}, result: {} ({})", frameId,
                      magic_enum::enum_name(type), magic_enum::enum_name(tagResult), (int32_t) tagResult);
            if (tagResult < XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                submit();
                return false;
            }
        }
        if (!submit())
            return false;
        _inputRecordCpuMs += Util::MillisecondsNow() - recordStarted;
    }

''' + s[end:]
old = '''                if (!StageMultiGPUResource(fResource, fIndex))'''
s = replace(s, old, '''                // Protect the producer's shared source slot before recording a
                // new write, not only the consumer allocator before its reset.
                const auto reuseResult = _inputTransfers.WaitForSlot(static_cast<UINT>(fIndex));
                if (FAILED(reuseResult))
                {
                    LOG_ERROR("MultiGPU v12: XeFG input slot reuse failed: {:X}", (UINT) reuseResult);
                    return false;
                }
                if (!StageMultiGPUResource(fResource, fIndex))''')
s = replace(s, 'void XeFG_Dx12::ReleaseObjects()\n{', '''void XeFG_Dx12::ReleaseObjects()
{
    const auto drainResult = _inputTransfers.Drain();
    if (FAILED(drainResult))
        LOG_ERROR("MultiGPU v12: XeFG input drain during resource release failed: {:X}", (UINT) drainResult);''')
status = '''void XeFG_Dx12::CaptureSDKPresentStatus()
{
    if (!IsMultiGPUActive() || _swapChainContext == nullptr)
        return;
    xefg_swapchain_present_status_t status {};
    const auto query = XeFGProxy::GetLastPresentStatus();
    const int queryResult = query != nullptr ? static_cast<int>(query(_swapChainContext, &status)) : -999;
    const bool known = queryResult >= 0;
    const int interpolationResult = known ? static_cast<int>(status.frameGenResult) : queryResult;
    if (queryResult != _lastPresentQueryResult || interpolationResult != _lastInterpolationResult)
    {
        LOG_INFO("MultiGPU v12: XeFG present status query={}, interpolation={}, enabled={}, queuedFrames={}",
                 queryResult, interpolationResult, status.isFrameGenEnabled, status.framesPresented);
        _lastPresentQueryResult = queryResult;
        _lastInterpolationResult = interpolationResult;
    }
    const auto now = Util::MillisecondsNow();
    if (_presentationStats.Record(now, status.framesPresented, known,
                                  known ? status.isFrameGenEnabled != 0 : IsActive()))
    {
        MultiGPU::XeFGPresentationStats::Snapshot sample;
        if (_presentationStats.Read(now, sample))
        {
            const double divisor = sample.renderFrames > 0 ? static_cast<double>(sample.renderFrames) : 1.0;
            LOG_INFO("MultiGPU v12 XeFG perf: renderFPS={:.1f}, SDKqueuedFPS={:.1f}, outputKnown={}, inputBatchCPU={:.3f} ms/frame, reuseWait={:.3f} ms/frame, queuedFrames={}, renderFrames={}, interpolation={}",
                     sample.renderFps, sample.queuedFps, sample.outputKnown, _inputRecordCpuMs / divisor,
                     _inputTransfers.TakeReuseWaitMs() / divisor, sample.queuedFrames, sample.renderFrames,
                     interpolationResult);
            _inputRecordCpuMs = 0;
        }
    }
}

bool XeFG_Dx12::GetSDKPresentRates(float& renderFps, float& outputFps, bool& outputKnown)
{
    MultiGPU::XeFGPresentationStats::Snapshot sample;
    if (!_presentationStats.Read(Util::MillisecondsNow(), sample))
        return false;
    renderFps = static_cast<float>(sample.renderFps);
    outputFps = static_cast<float>(sample.queuedFps);
    outputKnown = sample.outputKnown;
    return true;
}

'''
s = replace(s, 'bool XeFG_Dx12::SetResource(Dx12Resource* inputResource)', status + 'bool XeFG_Dx12::SetResource(Dx12Resource* inputResource)')
changes[path] = s

path = 'wrapped/wrapped_swapchain.cpp'
s = (root / path).read_text(encoding='utf-8-sig')
start = s.index('bool WrappedIDXGISwapChain4::TransferMultiGPUVirtualBackbuffer()')
end = s.index('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::QueryInterface', start)
p = s[start:end]
p = replace(p, '    auto renderCmd = _multiGpuRuntime->BeginRenderTransfer(index, _multiGpuRenderQueue.Get());', '''    // Retire the prior consumer before overwriting this shared backbuffer slot.
    // XeFG input/tag commands now use a different allocator pool.
    auto fgCmd = _multiGpuRuntime->BeginFGTransfer(index);
    if (fgCmd == nullptr)
        return false;
    auto renderCmd = _multiGpuRuntime->BeginRenderTransfer(index, _multiGpuRenderQueue.Get());''')
p = replace(p, '        _multiGpuRuntime->AbortRenderTransfer(index);', '''        _multiGpuRuntime->AbortRenderTransfer(index);
        _multiGpuRuntime->AbortFGTransfer(index);''')
for message in ['render->FG synchronization failed for virtual backbuffer {}', 'failed obtaining destination secondary backbuffer {}']:
    anchor = '        LOG_ERROR("MultiGPU v6: ' + message + '", index);'
    p = replace(p, anchor, '        _multiGpuRuntime->AbortFGTransfer(index);\n' + anchor)
p = replace(p, '''    auto fgCmd = _multiGpuRuntime->BeginFGTransfer(index);
    if (fgCmd == nullptr ||
        !_multiGpuBackbufferBridges[index]->RecordFGImport''', '''    if (!_multiGpuBackbufferBridges[index]->RecordFGImport''')
s = s[:start] + p + s[end:]
changes[path] = s

path = 'hooks/FG_Hooks.cpp'
s = (root / path).read_text(encoding='utf-8-sig')
start = s.index('HRESULT FGHooks::FGPresent(')
end = s.index('ULONG FGHooks::hkFGRelease(', start)
p = s[start:end]
p = replace(p, '    if (result == S_OK)\n    {', '''    // Query the SDK only after the application proxy Present completes; test
    // presents and internal native presents must not create extra samples.
    if (willPresent && result == S_OK && fg != nullptr)
        fg->CaptureSDKPresentStatus();

    if (result == S_OK)
    {''')
s = s[:start] + p + s[end:]
changes[path] = s

path = 'menu/menu_common.cpp'
s = (root / path).read_text(encoding='utf-8-sig')
anchor = '            // Prepare Line 2'
overlay = '''            // The virtual XeFG overlay runs once per rendered frame. Dividing
            // that rate by the configured FG multiplier fabricated a lower base
            // rate. Use explicit SDK queue counts, labelled as an estimate.
            if (fg != nullptr && fg->UsesSDKPresentRates() && fg->IsActive() && !fg->IsPaused())
            {
                float renderedFps = static_cast<float>(frameRate);
                float sdkFps = 0;
                bool outputKnown = false;
                const bool measured = fg->GetSDKPresentRates(renderedFps, sdkFps, outputKnown);
                if (measured && outputKnown)
                    firstLine = StrFmt("%s | Render FPS: %.1f | XeFG output est.: %.1f", api.c_str(), renderedFps, sdkFps);
                else
                    firstLine = StrFmt("%s | Render FPS: %.1f | XeFG output: --", api.c_str(), renderedFps);
                if (currentFeature != nullptr && !currentFeature->IsFrozen() &&
                    config->FpsOverlayType.value_or_default() != FpsOverlay_JustFPS)
                    firstLine += StrFmt(" | %s -> %s %u.%u.%u", state.currentInputApiName.c_str(), currentFeature->Name().c_str(),
                                        currentFeature->Version().major, currentFeature->Version().minor,
                                        currentFeature->Version().patch);
            }

'''
s = replace(s, anchor, overlay + anchor)
changes[path] = s

for header in ['XeFGTransferPool.h', 'XeFGPresentationStats.h']:
    changes['framegen/' + header] = (kit / 'v12' / header).read_text()
for path, content in changes.items():
    (root / path).write_text(content, encoding='utf-8')
print('v12 applied: one XeFG input batch, independent pool, safe slot reuse and SDK presentation statistics')
