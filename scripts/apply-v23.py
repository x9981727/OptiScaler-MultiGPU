"""v23: overlap the primary-GPU virtual-backbuffer export with the previous XeFG Present.

v22 restores correctness by completing the previous secondary SDK Present at the
very start of every eligible outer Present. Real-game telemetry on RX 9070 XT +
RX 6600 XT is now stable at ~60/120 FPS, but the render thread still spends
roughly 8-13 ms waiting for that prior Present while the primary-side export
itself costs ~1.2 ms GPU.

For the ordinary async secondary-XeFG path only, v23 moves exact completion from
outer-Present entry to the latest safe point inside the virtual-backbuffer bridge:
after the current primary virtual backbuffer has already been exported and its
render->FG queue dependency has been queued, but before the secondary swapchain
backbuffer is queried/touched and before the current SDK Present/tag lifecycle.
This preserves exact frame ownership and never drops/advances a source frame.
TEST, partial/VSync/synchronous/ineligible paths keep the v22 entry wait.
"""
from pathlib import Path

kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream' / 'OptiScaler'


def read(path):
    return (root / path).read_text(encoding='utf-8-sig')


def rep(text, old, new, count=1):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'v23 anchor count {actual} != {count}: {old[:180]}')
    return text.replace(old, new)

p = 'wrapped/wrapped_swapchain.cpp'
s = read(p)

# v22 waits unconditionally at outer-Present entry. Keep that exact wait for
# paths that cannot safely overlap, but let the ordinary async path export its
# primary virtual backbuffer first.
old = '''    const HRESULT previousPresent = FinishMultiGPUQueuedPresent(true);
    if (previousPresent != S_OK) return previousPresent;
    HRESULT result;'''

# Present uses no DXGI_PRESENT_PARAMETERS.
new_present = '''    const bool v23OverlapEligible = AllowMultiGPUQueuedPresent(SyncInterval, Flags, nullptr);
    if (!v23OverlapEligible)
    {
        const HRESULT previousPresent = FinishMultiGPUQueuedPresent(true);
        if (previousPresent != S_OK) return previousPresent;
    }
    HRESULT result;'''

# Present1 must preserve exact partial-update semantics.
new_present1 = '''    const bool v23OverlapEligible = AllowMultiGPUQueuedPresent(SyncInterval, Flags, pPresentParameters);
    if (!v23OverlapEligible)
    {
        const HRESULT previousPresent = FinishMultiGPUQueuedPresent(true);
        if (previousPresent != S_OK) return previousPresent;
    }
    HRESULT result;'''

# Replace in method order rather than globally so Present1 gets its parameter.
present_start = s.index('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present(')
present_end = s.index('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer(', present_start)
part = s[present_start:present_end]
part = rep(part, old, new_present)
s = s[:present_start] + part + s[present_end:]

present1_start = s.index('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(')
present1_end = s.index('BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain4::IsTemporaryMonoSupported(', present1_start)
part = s[present1_start:present1_end]
part = rep(part, old, new_present1)
s = s[:present1_start] + part + s[present1_end:]

# The render-side copy and SignalRenderAndWaitOnFG do not touch the SDK-owned
# destination backbuffer. Completing the previous SDK Present immediately before
# GetBuffer lets those operations overlap the pending secondary Present while
# retaining exact destination/backbuffer/frame-ID ownership.
transfer_start = s.index('bool WrappedIDXGISwapChain4::TransferMultiGPUVirtualBackbuffer()')
transfer_end = s.index('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::QueryInterface', transfer_start)
part = s[transfer_start:transfer_end]
needle = '    Microsoft::WRL::ComPtr<ID3D12Resource> fgBackbuffer;'
insert = '''    const HRESULT v23PreviousPresent = FinishMultiGPUQueuedPresent(true);
    if (v23PreviousPresent != S_OK)
    {
        _multiGpuRuntime->AbortFGTransfer(index);
        LOG_WARN("MultiGPU v23: prior XeFG Present failed after overlapped render export: {:X}",
                 static_cast<UINT>(v23PreviousPresent));
        return false;
    }

''' + needle
part = rep(part, needle, insert)
s = s[:transfer_start] + part + s[transfer_end:]
(root / p).write_text(s, encoding='utf-8')

# Runtime marker alongside the existing per-second timing telemetry.
p = 'framegen/xefg/XeFG_Dx12.cpp'
s = read(p)
anchor = '            LOG_INFO("MultiGPU v22: exact outer Present completion restored; v21 source shedding disabled");'
marker = anchor + '\n            LOG_INFO("MultiGPU v23: primary render export overlaps prior XeFG Present before exact destination access");'
s = rep(s, anchor, marker)
(root / p).write_text(s, encoding='utf-8')

print('v23 applied: primary render export overlaps previous secondary XeFG Present; exact destination ownership retained')
