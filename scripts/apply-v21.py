"""v21: preserve render-GPU cadence by shedding source presents under XeFG backpressure.

v19/v20 remove frame-time feedback, but RX 9070 XT + RX 6600 XT telemetry still
shows the next outer Present waiting ~13-15 ms while the secondary SDK Present
worker is busy. The 6600 XT XeFG GPU span is only ~10-11 ms, so the remaining
loss is presentation backpressure rather than pure XeFG compute saturation.

For immediate, ordinary async presents only, v21 polls the previous worker
without blocking. If it is still busy, the current virtual source frame is
intentionally dropped and the render-side cursor advances. This is mailbox-like
backpressure: preserve native render cadence and skip a small number of FG source
frames instead of stalling the 9070 XT. VSync, TEST, DO_NOT_SEQUENCE, partial,
fullscreen and synchronous fallback paths retain the old blocking semantics.
"""
from pathlib import Path

kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream' / 'OptiScaler'


def read(path):
    return (root / path).read_text(encoding='utf-8-sig')


def rep(text, old, new, count=1):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'v21 anchor count {actual} != {count}: {old[:160]}')
    return text.replace(old, new)

# Nonblocking completion probe on the FG abstraction.
p = 'framegen/IFGFeature_Dx12.h'
s = read(p)
s = rep(s,
'''    virtual HRESULT FinishXeFGPresent(bool consumeStatus = false) { return S_OK; }
    virtual bool QueueXeFGPresent(std::function<HRESULT()> call) { return false; }''',
'''    virtual HRESULT FinishXeFGPresent(bool consumeStatus = false) { return S_OK; }
    virtual HRESULT TryFinishXeFGPresent(bool consumeStatus = false, bool* pending = nullptr)
    {
        if (pending != nullptr) *pending = false;
        return S_OK;
    }
    virtual bool QueueXeFGPresent(std::function<HRESULT()> call) { return false; }''')
(root / p).write_text(s, encoding='utf-8')

p = 'framegen/xefg/XeFG_Dx12.h'
s = read(p)
s = rep(s,
'''    HRESULT FinishXeFGPresent(bool consumeStatus = false) override final;
    bool QueueXeFGPresent(std::function<HRESULT()> call) override final;''',
'''    HRESULT FinishXeFGPresent(bool consumeStatus = false) override final;
    HRESULT TryFinishXeFGPresent(bool consumeStatus = false, bool* pending = nullptr) override final;
    bool QueueXeFGPresent(std::function<HRESULT()> call) override final;''')
(root / p).write_text(s, encoding='utf-8')

p = 'framegen/xefg/XeFG_Dx12.cpp'
s = read(p)
finish_anchor = 'HRESULT XeFG_Dx12::FinishXeFGPresent(bool consumeStatus)\n{'
try_method = '''HRESULT XeFG_Dx12::TryFinishXeFGPresent(bool consumeStatus, bool* pending)
{
    std::lock_guard control(_deferredControlMutex);
    if (pending != nullptr) *pending = false;

    if (_deferredPresent != nullptr && _deferredPresent->Pending())
    {
        if (!_deferredPresent->ReadyWithin(std::chrono::milliseconds(0)))
        {
            if (pending != nullptr) *pending = true;
            return _deferredResult;
        }

        const auto completed = _deferredPresent->Take();
        if (completed.has_value())
        {
            ++_deferredCompleted;
            const HRESULT presentResult = static_cast<HRESULT>(*completed);
            if (presentResult != S_OK)
            {
                _deferredResult = presentResult;
                LOG_WARN("MultiGPU v21: deferred SDK Present returned {:X}", static_cast<UINT>(presentResult));
            }
            else
            {
                CaptureSDKPresentStatus();
            }
        }
        else if (_deferredPresent->Pending())
        {
            if (pending != nullptr) *pending = true;
        }
    }

    const HRESULT result = _deferredResult;
    if (consumeStatus && SUCCEEDED(result)) _deferredResult = S_OK;
    return result;
}

'''
s = rep(s, finish_anchor, try_method + finish_anchor)
(root / p).write_text(s, encoding='utf-8')

# Add the mailbox/backpressure policy and counters to the virtual swapchain.
p = 'wrapped/wrapped_swapchain.h'
s = read(p)
s = rep(s,
        '#include <framegen/XeFGPresentPolicy.h>',
        '#include <framegen/XeFGPresentPolicy.h>\n#include <framegen/XeFGBackpressurePolicy.h>')
s = rep(s,
'''    HRESULT FinishMultiGPUQueuedPresent(bool consumeStatus = false);
    bool AllowMultiGPUQueuedPresent(UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters);''',
'''    HRESULT FinishMultiGPUQueuedPresent(bool consumeStatus = false);
    HRESULT TryFinishMultiGPUQueuedPresent(bool consumeStatus, bool* pending);
    bool AllowMultiGPUQueuedPresent(UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters);
    UINT64 _multiGpuBackpressureAttempts = 0;
    UINT64 _multiGpuDroppedSourceFrames = 0;''')
(root / p).write_text(s, encoding='utf-8')

p = 'wrapped/wrapped_swapchain.cpp'
s = read(p)
finish = '''HRESULT WrappedIDXGISwapChain4::FinishMultiGPUQueuedPresent(bool consumeStatus)
{
    auto fg = _multiGpuVirtualBackbufferRequested ? State::Instance().currentFG : nullptr;
    return fg != nullptr ? fg->FinishXeFGPresent(consumeStatus) : S_OK;
}
'''
finish_plus_try = finish + '''
HRESULT WrappedIDXGISwapChain4::TryFinishMultiGPUQueuedPresent(bool consumeStatus, bool* pending)
{
    if (pending != nullptr) *pending = false;
    auto fg = _multiGpuVirtualBackbufferRequested ? State::Instance().currentFG : nullptr;
    return fg != nullptr ? fg->TryFinishXeFGPresent(consumeStatus, pending) : S_OK;
}
'''
s = rep(s, finish, finish_plus_try)

# Replace the mandatory previous-Present wait in both Present and Present1.
for begin, end in [
    ('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present(', 'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer('),
    ('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(', 'BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain4::IsTemporaryMonoSupported('),
]:
    start = s.index(begin)
    stop = s.index(end, start)
    part = s[start:stop]
    params = 'pPresentParameters' if '::Present1(' in begin else 'nullptr'
    old = '''    const HRESULT previousPresent = FinishMultiGPUQueuedPresent(true);
    if (previousPresent != S_OK) return previousPresent;
    HRESULT result;'''
    new = f'''    const bool v21AsyncEligible = AllowMultiGPUQueuedPresent(SyncInterval, Flags, {params});
    if (v21AsyncEligible) ++_multiGpuBackpressureAttempts;

    bool v21PreviousPending = false;
    HRESULT previousPresent = TryFinishMultiGPUQueuedPresent(true, &v21PreviousPending);
    if (previousPresent != S_OK) return previousPresent;

    if (MultiGPU::CanDropBusyXeFGSource(v21AsyncEligible, SyncInterval, Flags, v21PreviousPending))
    {{
        ++_multiGpuDroppedSourceFrames;
        _multiGpuCursor.Advance(true, Flags);
        if (_multiGpuDroppedSourceFrames <= 3 || (_multiGpuDroppedSourceFrames % 30) == 0)
            LOG_INFO("MultiGPU v21 backpressure: dropped busy source frame; attempts={{}}, dropped={{}}, accepted={{}}",
                     _multiGpuBackpressureAttempts, _multiGpuDroppedSourceFrames,
                     _multiGpuBackpressureAttempts - _multiGpuDroppedSourceFrames);
        return S_OK;
    }}

    if (v21PreviousPending)
    {{
        previousPresent = FinishMultiGPUQueuedPresent(true);
        if (previousPresent != S_OK) return previousPresent;
    }}
    HRESULT result;'''
    if part.count(old) != 1:
        raise RuntimeError(f'v21 Present wait anchor mismatch in {begin}: {part.count(old)}')
    part = part.replace(old, new)
    s = s[:start] + part + s[stop:]

(root / p).write_text(s, encoding='utf-8')
(root / 'framegen/XeFGBackpressurePolicy.h').write_text(
    (kit / 'v21' / 'XeFGBackpressurePolicy.h').read_text(encoding='utf-8'), encoding='utf-8')

print('v21 applied: immediate secondary XeFG uses mailbox-style busy-source shedding instead of render-thread blocking')
