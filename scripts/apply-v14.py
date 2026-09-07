"""Apply the bounded XeFG presentation handoff to exact reconstructed v13 sources."""
from pathlib import Path
kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream/OptiScaler'
changes = {}

def read(path):
    return (root / path).read_text(encoding='utf-8-sig')

def rep(s, old, new, count=1):
    if s.count(old) != count:
        raise RuntimeError(f'v14 anchor mismatch: expected {count}, found {s.count(old)}: {old[:100]}')
    return s.replace(old, new)

p = 'framegen/IFGFeature_Dx12.h'
s = read(p)
s = rep(s, '#include "XeFGTimingCounters.h"', '#include "XeFGTimingCounters.h"\n#include <functional>')
s = rep(s, '    virtual void CaptureSDKPresentStatus() {}', '''    virtual void CaptureSDKPresentStatus() {}
    virtual HRESULT FinishXeFGPresent(bool consumeStatus = false) { return S_OK; }
    virtual bool QueueXeFGPresent(std::function<HRESULT()> call) { return false; }''')
changes[p] = s

p = 'framegen/XeFGTimingCounters.h'
s = read(p)
s = rep(s, 'Overlay, Locks, Count', 'Overlay, Locks, PresentWait, Count')
changes[p] = s

p = 'Config.h'
s = read(p)
s = rep(s, '    CustomOptional<bool> FGXeFGForceBorderless { false };', '''    CustomOptional<bool> FGXeFGForceBorderless { false };
    CustomOptional<bool> FGXeFGAsyncPresent { true }; // dual-GPU virtual swapchain only''')
changes[p] = s
p = 'Config.cpp'
s = read(p)
s = rep(s, '            FGXeFGForceBorderless.set_from_config(readBool("XeFG", "ForceBorderless"));', '''            FGXeFGForceBorderless.set_from_config(readBool("XeFG", "ForceBorderless"));
            FGXeFGAsyncPresent.set_from_config(readBool("XeFG", "AsyncPresent"));''')
s = rep(s, '        ini.SetValue("XeFG", "DebugView", GetBoolValue(Instance()->FGXeFGDebugView.value_for_config()).c_str());', '''        ini.SetValue("XeFG", "DebugView", GetBoolValue(Instance()->FGXeFGDebugView.value_for_config()).c_str());
        ini.SetValue("XeFG", "AsyncPresent", GetBoolValue(Instance()->FGXeFGAsyncPresent.value_for_config()).c_str());''')
changes[p] = s

p = 'framegen/xefg/XeFG_Dx12.h'
s = read(p)
s = rep(s, '#include <framegen/QueueSpanProbe_Dx12.h>', '#include <framegen/QueueSpanProbe_Dx12.h>\n#include <framegen/DeferredPresent.h>\n#include <atomic>')
s = rep(s, '    double _inputRecordCpuMs = 0;', '''    std::atomic<double> _inputRecordCpuMs {0};
    std::recursive_mutex _deferredControlMutex;
    std::unique_ptr<MultiGPU::DeferredPresent> _deferredPresent;
    HRESULT _deferredResult = S_OK;
    UINT64 _deferredQueued = 0, _deferredCompleted = 0;''')
s = rep(s, '    void CaptureSDKPresentStatus() override final;', '''    void CaptureSDKPresentStatus() override final;
    HRESULT FinishXeFGPresent(bool consumeStatus = false) override final;
    bool QueueXeFGPresent(std::function<HRESULT()> call) override final;''')
changes[p] = s

p = 'framegen/xefg/XeFG_Dx12.cpp'
s = read(p)
s = rep(s, '        _adapterBinding.Bind(RequestedXeFGAdapter());', '''        _deferredResult = S_OK;
        _adapterBinding.Bind(RequestedXeFGAdapter());''')
s = rep(s, 'bool XeFG_Dx12::DestroySwapchainContext()\n{', '''bool XeFG_Dx12::DestroySwapchainContext()
{
    std::lock_guard control(_deferredControlMutex);
    FinishXeFGPresent();
    _deferredPresent.reset();''')
s = rep(s, 'void XeFG_Dx12::Deactivate()\n{', '''void XeFG_Dx12::Deactivate()
{
    std::lock_guard control(_deferredControlMutex);
    FinishXeFGPresent();''')
s = rep(s, 'void XeFG_Dx12::ReleaseObjects()\n{', '''void XeFG_Dx12::ReleaseObjects()
{
    std::lock_guard control(_deferredControlMutex);
    FinishXeFGPresent();''')
s = rep(s, 'bool XeFG_Dx12::Shutdown()\n{', '''bool XeFG_Dx12::Shutdown()
{
    std::lock_guard control(_deferredControlMutex);
    FinishXeFGPresent();
    _deferredPresent.reset();''')
s = rep(s, 'bool XeFG_Dx12::BeginXeFGGpuTiming()', '''bool XeFG_Dx12::QueueXeFGPresent(std::function<HRESULT()> call)
{
    std::lock_guard control(_deferredControlMutex);
    if (!IsMultiGPUActive() || !IsActive() || IsPaused() ||
        !Config::Instance()->FGXeFGAsyncPresent.value_or_default() || _deferredResult != S_OK)
        return false;
    try
    {
        if (_deferredPresent == nullptr)
            _deferredPresent = std::make_unique<MultiGPU::DeferredPresent>();
        // The worker only touches its own probe/counters and the SDK. It never
        // acquires the game/FG swapchain lock or reads next-frame input arrays.
        const bool accepted = _deferredPresent->Submit([this, call = std::move(call)]() -> std::int32_t {
            const bool began = _sdkGpuSpan.Begin(_multiGpuRuntime->FGQueue());
            const double start = Util::MillisecondsNow();
            const HRESULT result = call();
            const double elapsed = Util::MillisecondsNow() - start;
            if (began) _sdkGpuSpan.End();
            _cpuTimings.Add(MultiGPU::XeFGCpuPhase::ProxyPresent, elapsed);
            return result;
        });
        if (accepted) ++_deferredQueued;
        return accepted;
    }
    catch (...) { return false; } // Allocation/thread creation failure: use synchronous Present.
}

HRESULT XeFG_Dx12::FinishXeFGPresent(bool consumeStatus)
{
    std::lock_guard control(_deferredControlMutex);
    if (_deferredPresent != nullptr && _deferredPresent->Pending())
    {
        const double start = Util::MillisecondsNow();
        while (!_deferredPresent->ReadyWithin(std::chrono::milliseconds(5))) {}
        const auto completed = _deferredPresent->Take();
        if (completed.has_value())
        {
            _cpuTimings.Add(MultiGPU::XeFGCpuPhase::PresentWait, Util::MillisecondsNow() - start);
            ++_deferredCompleted;
            const HRESULT result = static_cast<HRESULT>(*completed);
            if (result != S_OK)
            {
                _deferredResult = result;
                LOG_WARN("MultiGPU v14: deferred SDK Present returned {:X}", static_cast<UINT>(result));
            }
            else
                CaptureSDKPresentStatus(); // Read once, before another SDK Present or TEST can overwrite it.
        }
    }
    const HRESULT result = _deferredResult;
    if (consumeStatus && SUCCEEDED(result)) _deferredResult = S_OK;
    // Device failures stay sticky until the SDK context is recreated.
    return result;
}

bool XeFG_Dx12::BeginXeFGGpuTiming()''')
s = rep(s, 'void XeFG_Dx12::CaptureSDKPresentStatus()\n{', 'void XeFG_Dx12::CaptureSDKPresentStatus()\n{\n    std::lock_guard control(_deferredControlMutex);')
s = rep(s, '_inputRecordCpuMs / divisor', '_inputRecordCpuMs.exchange(0) / divisor')
s = rep(s, '            _inputRecordCpuMs = 0;\n', '')
anchor = '                     Config::Instance()->FramerateLimit.value_or_default());'
s = rep(s, anchor, anchor + '''
            LOG_INFO("MultiGPU v14 present: asyncSetting={}, queued={}, completed={}, nextPresentWaitCPU={:.3f} ms, waitCalls={}, pendingResult={:X}",
                     Config::Instance()->FGXeFGAsyncPresent.value_or_default(), _deferredQueued, _deferredCompleted,
                     mean(MultiGPU::XeFGCpuPhase::PresentWait),
                     cpu[static_cast<size_t>(MultiGPU::XeFGCpuPhase::PresentWait)].calls,
                     static_cast<UINT>(_deferredResult));
            _deferredQueued = _deferredCompleted = 0;''')
changes[p] = s

p = 'hooks/FG_Hooks.h'
s = read(p)
s = rep(s, 'inline static bool _skipPresent = false;', 'inline static thread_local bool _skipPresent = false;')
s = rep(s, 'inline static bool _skipPresent1 = false;', 'inline static thread_local bool _skipPresent1 = false;')
changes[p] = s

p = 'hooks/FG_Hooks.cpp'
s = read(p)
s = rep(s, '#include "FG_Hooks.h"', '#include "FG_Hooks.h"\n#include <framegen/XeFGPresentPolicy.h>')
start = s.index('HRESULT FGHooks::FGPresent(')
end = s.index('ULONG FGHooks::hkFGRelease(', start)
part = s[start:end]
part = rep(part, '    const bool measureXeFG = willPresent && fg != nullptr && fg->UsesSDKPresentRates();', '''    bool deferred = false;
    if (willPresent && fg != nullptr && MultiGPU::XeFGPresentScope::Allowed() &&
        SyncInterval <= 4 && (Flags & ~DXGI_PRESENT_ALLOW_TEARING) == 0)
    {
        // Only empty Present1 parameters are eligible; partial updates retain
        // synchronous DXGI semantics and never leave caller-owned pointers behind.
        Microsoft::WRL::ComPtr<IDXGISwapChain> retained = This;
        const bool usePresent1 = pPresentParameters != nullptr;
        const auto originalPresent = o_FGSCPresent;
        const auto originalPresent1 = o_FGSCPresent1;
        deferred = fg->QueueXeFGPresent([retained, SyncInterval, Flags, usePresent1,
                                        originalPresent, originalPresent1]() -> HRESULT {
            struct RestoreFlags
            {
                bool& a; bool& b; bool oldA; bool oldB;
                ~RestoreFlags() { a = oldA; b = oldB; }
            } restore {_skipPresent, _skipPresent1, _skipPresent, _skipPresent1};
            _skipPresent = _skipPresent1 = true;
            if (!usePresent1) return originalPresent(retained.Get(), SyncInterval, Flags);
            const DXGI_PRESENT_PARAMETERS empty {};
            return originalPresent1(static_cast<IDXGISwapChain1*>(retained.Get()), SyncInterval, Flags, &empty);
        });
    }
    const bool measureXeFG = !deferred && willPresent && fg != nullptr && fg->UsesSDKPresentRates();''')
part = rep(part, '''    HRESULT result;
    if (pPresentParameters == nullptr)''', '''    HRESULT result;
    if (deferred)
        result = S_OK; // Accepted; the exact SDK result is collected at the next outer Present.
    else if (pPresentParameters == nullptr)''')
part = rep(part, '    if (willPresent && result == S_OK && fg != nullptr)', '    if (!deferred && willPresent && result == S_OK && fg != nullptr)')
s = s[:start] + part + s[end:]
changes[p] = s

p = 'wrapped/wrapped_swapchain.h'
s = read(p)
s = rep(s, '#include <framegen/MultiGPU_Dx12.h>', '#include <framegen/MultiGPU_Dx12.h>\n#include <framegen/XeFGPresentPolicy.h>')
s = rep(s, '    bool _multiGpuVirtualBackbufferReady = false;', '''    bool _multiGpuVirtualBackbufferReady = false;
    MultiGPU::VirtualBackbufferCursor _multiGpuCursor;
    int _lastAsyncEligibility = -1;
    HRESULT FinishMultiGPUQueuedPresent(bool consumeStatus = false);
    bool AllowMultiGPUQueuedPresent(UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters);''')
changes[p] = s

p = 'wrapped/wrapped_swapchain.cpp'
s = read(p)
# Independent virtual indices remain valid even when a pending SDK Present has
# not yet advanced the destination swapchain. GPU slot fences remain unchanged.
s = rep(s, '    _multiGpuVirtualBackbuffers.resize(scDesc.BufferCount);', '''    _multiGpuVirtualBackbuffers.resize(scDesc.BufferCount);
    _multiGpuCursor.Reset(scDesc.BufferCount);''')
s = rep(s, '    const UINT index = _real3->GetCurrentBackBufferIndex();', '''    const UINT index = _multiGpuCursor.Current();
    const UINT destinationIndex = _real3->GetCurrentBackBufferIndex();''')
s = rep(s, '    if (FAILED(_real->GetBuffer(index, IID_PPV_ARGS(&fgBackbuffer))) || fgBackbuffer == nullptr)', '    if (FAILED(_real->GetBuffer(destinationIndex, IID_PPV_ARGS(&fgBackbuffer))) || fgBackbuffer == nullptr)')
s = rep(s, '''    auto index = _real3->GetCurrentBackBufferIndex();
    // LOG_TRACE("index: {}", index);
    return index;''', '''    if (_multiGpuVirtualBackbufferReady) return _multiGpuCursor.Current();
    return _real3->GetCurrentBackBufferIndex();''')
s = rep(s, 'bool WrappedIDXGISwapChain4::TransferMultiGPUVirtualBackbuffer()', '''HRESULT WrappedIDXGISwapChain4::FinishMultiGPUQueuedPresent(bool consumeStatus)
{
    auto fg = _multiGpuVirtualBackbufferRequested ? State::Instance().currentFG : nullptr;
    return fg != nullptr ? fg->FinishXeFGPresent(consumeStatus) : S_OK;
}

bool WrappedIDXGISwapChain4::AllowMultiGPUQueuedPresent(UINT interval, UINT flags,
                                                       const DXGI_PRESENT_PARAMETERS* parameters)
{
    auto fg = State::Instance().currentFG;
    DXGI_SWAP_CHAIN_DESC desc {};
    const bool windowed = SUCCEEDED(_real->GetDesc(&desc)) && desc.Windowed != FALSE &&
                          !State::Instance().realExclusiveFullscreen;
    const DWORD windowThread = _handle != nullptr ? GetWindowThreadProcessId(_handle, nullptr) : 0;
    const bool partial = parameters != nullptr && (parameters->DirtyRectsCount != 0 ||
                         parameters->pScrollRect != nullptr || parameters->pScrollOffset != nullptr);
    const bool allowed = MultiGPU::CanDeferXeFGPresent(
        Config::Instance()->FGXeFGAsyncPresent.value_or_default(),
        fg != nullptr && fg->UsesSDKPresentRates() && fg->IsActive() && !fg->IsPaused(),
        windowed, windowThread != 0 && windowThread != GetCurrentThreadId(),
        static_cast<UINT>(_multiGpuVirtualBackbuffers.size()), interval, flags, partial);
    if (_lastAsyncEligibility != static_cast<int>(allowed))
    {
        LOG_INFO("MultiGPU v14 policy: eligible={}, windowed={}, windowThread={}, presentThread={}, buffers={}, syncInterval={}, flags={:X}, partialUpdate={}",
                 allowed, windowed, windowThread, GetCurrentThreadId(), _multiGpuVirtualBackbuffers.size(),
                 interval, flags, partial);
        _lastAsyncEligibility = static_cast<int>(allowed);
    }
    return allowed;
}

bool WrappedIDXGISwapChain4::TransferMultiGPUVirtualBackbuffer()''')

for begin, end in [
    ('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present(', 'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer('),
    ('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(', 'BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain4::IsTemporaryMonoSupported('),
]:
    start = s.index(begin); stop = s.index(end, start); part = s[start:stop]
    part = rep(part, '    HRESULT result;', '''    // Previous SDK Present must finish before destination-buffer access, SDK
    // resource tagging, SetPresentId, or a TEST Present for the next application frame.
    const HRESULT previousPresent = FinishMultiGPUQueuedPresent(true);
    if (previousPresent != S_OK) return previousPresent;
    HRESULT result;''')
    params = 'pPresentParameters' if '::Present1(' in begin else 'nullptr'
    real = '_real1' if params != 'nullptr' else '_real'
    old = f'            result = LocalPresent({real}, SyncInterval, Flags, {params}, _device, _handle, _uwp, true);'
    part = rep(part, old, f'''            MultiGPU::XeFGPresentScope asyncScope(AllowMultiGPUQueuedPresent(SyncInterval, Flags, {params}));
{old}
            _multiGpuCursor.Advance(result == S_OK, Flags);''')
    s = s[:start] + part + s[stop:]

# Drain before any SDK swapchain mutation, not just before the GPU queue drain.
for signature in [
    'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetFullscreenState(',
    'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers(',
    'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeTarget(',
    'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers1(',
    'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMaximumFrameLatency(',
    'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetColorSpace1(',
    'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetHDRMetaData(',
]:
    start = s.index(signature); brace = s.index('{', start) + 1
    s = s[:brace] + '''
    const HRESULT queuedPresent = FinishMultiGPUQueuedPresent();
    if (FAILED(queuedPresent)) return queuedPresent;
''' + s[brace:]
s = rep(s, 'HRESULT WrappedIDXGISwapChain4::DrainMultiGPUForResize()\n{', '''HRESULT WrappedIDXGISwapChain4::DrainMultiGPUForResize()
{
    const HRESULT queuedPresent = FinishMultiGPUQueuedPresent();
    if (FAILED(queuedPresent)) return queuedPresent;''')
s = rep(s, '''    if (ret == 0)
    {
#ifdef USE_LOCAL_MUTEX''', '''    if (ret == 0)
    {
        FinishMultiGPUQueuedPresent();
#ifdef USE_LOCAL_MUTEX''')
# Preserve the native waitable-object behavior; games using it may have a lower
# overlap benefit. Log its use instead of bypassing application latency policy.
s = rep(s, '''HANDLE STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameLatencyWaitableObject(void)
{
    return _real2->GetFrameLatencyWaitableObject();''', '''HANDLE STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameLatencyWaitableObject(void)
{
    if (_multiGpuVirtualBackbufferRequested)
        LOG_INFO("MultiGPU v14: game requested native frame-latency waitable object; preserving its pacing");
    return _real2->GetFrameLatencyWaitableObject();''')
changes[p] = s

for name in ['DeferredPresent.h', 'XeFGPresentPolicy.h']:
    changes['framegen/' + name] = (kit / 'v14' / name).read_text()
for path, text in changes.items():
    (root / path).write_text(text, encoding='utf-8')
print('v14 applied: bounded secondary XeFG Present handoff, virtual indices, exact completion and lifecycle drain')
