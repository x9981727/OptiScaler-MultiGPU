"""Apply v11 to v10. Validate every edit before writing the reconstructed source."""
from pathlib import Path
kit=Path(__file__).resolve().parents[1]
root=kit/'upstream/OptiScaler'
changes={}

def replace(s,old,new,count=1):
    if s.count(old)!=count:
        raise RuntimeError(f'v11 anchor count {s.count(old)} != {count}: {old[:120]}')
    return s.replace(old,new)

path='wrapped/wrapped_swapchain.cpp'
s=(root/path).read_text(encoding='utf-8-sig')
s=replace(s,'#include "wrapped_swapchain.h"','#include "wrapped_swapchain.h"\n#include <framegen/ScopedSwapchainLock.h>\n#include <framegen/QueueIdle_Dx12.h>')
start=s.index('static ID3D12Fence* resizeFence = nullptr;')
end=s.index('#ifdef DXGI_DEBUG_ENABLED\nvoid ReportDXGILiveObjects()',start)
s=s[:start]+'''static HRESULT WaitForGPUIdle(IUnknown* object)
{
    if (object == nullptr)
        return E_POINTER;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    const HRESULT result = object->QueryInterface(IID_PPV_ARGS(&queue));
    // A D3D11 swapchain does not have a D3D12 queue.
    if (result == E_NOINTERFACE)
        return S_OK;
    if (FAILED(result))
        return result;
    return MultiGPU::WaitForQueueIdle(queue.Get());
}

'''+s[end:]

# Keep primary device/queue references across resize and partial rebuild failure.
# They are released by the wrapper's member destructors, not by clearing textures.
s=replace(s,'''    _multiGpuRuntime = nullptr;
    _multiGpuRenderDevice.Reset();
    _multiGpuRenderQueue.Reset();
    _multiGpuVirtualBackbufferReady = false;''','''    _multiGpuVirtualBackbufferReady = false;''')

anchor='bool WrappedIDXGISwapChain4::TransferMultiGPUVirtualBackbuffer()'
s=replace(s,anchor,'''HRESULT WrappedIDXGISwapChain4::DrainMultiGPUForResize()
{
    if (_multiGpuRenderQueue == nullptr || _multiGpuRuntime == nullptr || !_multiGpuRuntime->IsActive())
        return DXGI_ERROR_INVALID_CALL;

    LOG_INFO("MultiGPU v11: draining render and FG queues before swapchain resize");
    auto result = MultiGPU::WaitForQueueIdle(_multiGpuRenderQueue.Get());
    if (FAILED(result))
    {
        LOG_ERROR("MultiGPU v11: render queue drain failed: {:X}; retaining virtual backbuffers", (UINT) result);
        return result;
    }
    result = MultiGPU::WaitForQueueIdle(_multiGpuRuntime->FGQueue());
    if (FAILED(result))
    {
        LOG_ERROR("MultiGPU v11: FG queue drain failed: {:X}; retaining virtual backbuffers", (UINT) result);
        return result;
    }
    LOG_INFO("MultiGPU v11: both queues idle; entering SDK resize with virtual backbuffers retained");
    return S_OK;
}

'''+anchor)

def rewrite_resize(s,signature,next_signature,owner):
    start=s.index(signature);end=s.index(next_signature,start)
    p=s[start:end]
    old=f'''        OwnedLockGuard lock(_localMutex, {owner});'''
    # Keep the existing Nukem callback exception, but make the guard live to return.
    old_block=f'''    if (!(_localMutex.getOwner() == 4 && Config::Instance()->FGInput.value_or_default() == FGInput::Nukems))
        OwnedLockGuard lock(_localMutex, {owner});'''
    p=replace(p,old_block,f'''    MultiGPU::ScopedSwapchainLock<OwnedMutex> localResizeLock(
        (_localMutex.getOwner() == 4 && Config::Instance()->FGInput.value_or_default() == FGInput::Nukems)
            ? nullptr : &_localMutex, {owner});''')
    old_fg='''    if (State::Instance().currentFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default() &&
        State::Instance().currentFG->Mutex.getOwner() != 6677 && State::Instance().currentFG->Mutex.getOwner() != 6678)
    {
        LOG_TRACE("Waiting ffxMutex 3, current: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.lock(3);
        LOG_TRACE("Accuired ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
    }'''
    p=replace(p,old_fg,'''    auto resizeFG = State::Instance().currentFG;
    MultiGPU::ScopedSwapchainLock<OwnedMutex> fgResizeLock(
        resizeFG != nullptr && (_multiGpuVirtualBackbufferRequested ||
                                Config::Instance()->FGUseMutexForSwapchain.value_or_default())
            ? &resizeFG->Mutex : nullptr, 3);''')
    # Overlay command allocators and heaps may still be referenced by GPU work.
    # Keep them alive until both queues have drained, including on timeout/error.
    p=replace(p,'    MenuOverlayDx::CleanupRenderTarget(true, _handle);\n\n','')
    p=replace(p,'''    WaitForGPUIdle(_device);

    const bool recreateMultiGpuVirtualBackbuffers = _multiGpuVirtualBackbufferRequested;
    if (recreateMultiGpuVirtualBackbuffers)
        ReleaseMultiGPUVirtualBackbuffers();''','''    const bool recreateMultiGpuVirtualBackbuffers = _multiGpuVirtualBackbufferRequested;
    const HRESULT idleResult = recreateMultiGpuVirtualBackbuffers ? DrainMultiGPUForResize() : WaitForGPUIdle(_device);
    if (FAILED(idleResult))
    {
        LOG_ERROR("MultiGPU v11: resize aborted before releasing resources, HRESULT={:X}", (UINT) idleResult);
        return idleResult;
    }
    MenuOverlayDx::CleanupRenderTarget(true, _handle);

    // The SDK owns secondary backbuffers; it does not own our primary textures.
    // Retain those textures until SDK resize succeeds, so failure preserves them.''')
    msg='ResizeBuffers' if owner==1 else 'ResizeBuffers1'
    p=replace(p,f'''        if (!_multiGpuVirtualBackbufferReady)
            LOG_ERROR("MultiGPU v6: failed recreating virtual backbuffers after {msg}");''',f'''        if (!_multiGpuVirtualBackbufferReady)
        {{
            LOG_ERROR("MultiGPU v11: virtual backbuffer rebuild failed after {msg}; blocking secondary resource exposure");
            result = DXGI_ERROR_DEVICE_REMOVED;
        }}
        else
            LOG_INFO("MultiGPU v11: {msg} completed and virtual backbuffers rebuilt");''')
    tail_condition=('State::Instance().currentFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default()'
                    if owner==1 else 'State::Instance().activeFgOutput == FGOutput::FSRFG &&\n        Config::Instance()->FGUseMutexForSwapchain.value_or_default()')
    p=replace(p,'''    if ('''+tail_condition+''')
    {
        LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.unlockThis(3);
    }

''','')
    # Track the primary textures exposed to the game, not SDK-owned secondary ones.
    p=replace(p,'if (_real->GetBuffer(i, IID_PPV_ARGS(&buffer)) == S_OK)',
              'if (GetBuffer(i, IID_PPV_ARGS(&buffer)) == S_OK)')
    return s[:start]+p+s[end:]

s=rewrite_resize(s,'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers(',
                 'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeTarget(',1)
s=rewrite_resize(s,'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers1(',
                 'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetHDRMetaData(',2)

# Serialize the whole transfer/Present operation against resize even while FG is off.
for signature,end_signature,owner in [
    ('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present(', 'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer(',4),
    ('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(', 'BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain4::IsTemporaryMonoSupported(',5),
]:
    start=s.index(signature);end=s.index(end_signature,start);p=s[start:end]
    p=replace(p,f'    OwnedLockGuard lock(_localMutex, {owner});',
              f'    MultiGPU::ScopedSwapchainLock<OwnedMutex> localPresentLock(&_localMutex, {owner});')
    p=replace(p,'    HRESULT result;', '''    auto presentFG = _multiGpuVirtualBackbufferRequested ? State::Instance().currentFG : nullptr;
    // Legacy EvaluateState releases owner 2 without acquiring it. Use a distinct
    // owner for RAII-managed presentation, so only this scope releases its lock.
    MultiGPU::ScopedSwapchainLock<OwnedMutex> fgPresentLock(presentFG ? &presentFG->Mutex : nullptr, 11002);
    if (_multiGpuVirtualBackbufferRequested && !_multiGpuVirtualBackbufferReady)
        return DXGI_ERROR_DEVICE_REMOVED;

    HRESULT result;''')
    s=s[:start]+p+s[end:]

# A failed virtual rebuild must never expose a secondary resource/device to the game.
for signature,param in [('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer(', 'ppSurface'),
                        ('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDevice(', 'ppDevice')]:
    start=s.index('{',s.index(signature))+1
    guard=f'''
    if (_multiGpuVirtualBackbufferRequested && !_multiGpuVirtualBackbufferReady)
    {{
        if ({param} != nullptr)
            *{param} = nullptr;
        return DXGI_ERROR_INVALID_CALL;
    }}
'''
    s=s[:start]+guard+s[start:]
changes[path]=s

path='wrapped/wrapped_swapchain.h'
h=(root/path).read_text(encoding='utf-8-sig')
h=replace(h,'    bool TransferMultiGPUVirtualBackbuffer();','    bool TransferMultiGPUVirtualBackbuffer();\n    HRESULT DrainMultiGPUForResize();')
changes[path]=h

path='hooks/FG_Hooks.cpp'
h=(root/path).read_text(encoding='utf-8-sig')
h=replace(h,'#include <wrapped/wrapped_swapchain.h>','#include <wrapped/wrapped_swapchain.h>\n#include <framegen/ScopedSwapchainLock.h>')
for owner in [6677,6678]:
    h=replace(h,f'    OwnedLockGuard lg(fg->Mutex, {owner});',f'''    MultiGPU::ScopedSwapchainLock<OwnedMutex> resizeLock(fg ? &fg->Mutex : nullptr, {owner});
    LOG_INFO("MultiGPU v11: entered FG resize hook {owner} with thread-scoped lock");''')
start=h.index('    bool mutexUsed = false;',h.index('HRESULT FGHooks::FGPresent('))
end=h.index('    if (willPresent && fg != nullptr)\n',start)
h=h[:start]+'''    // Keep legacy EvaluateState owner-2 recovery away from this managed lock.
    MultiGPU::ScopedSwapchainLock<OwnedMutex> presentLock(
        willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
                Config::Instance()->FGUseMutexForSwapchain.value_or_default()
            ? &fg->Mutex : nullptr, 11002);

'''+h[end:]
h=replace(h,'''    if (mutexUsed && fg != nullptr)
    {
        LOG_TRACE("Releasing FG->Mutex: {}", fg->Mutex.getOwner());
        fg->Mutex.unlockThis(2);
    }

''','')
changes[path]=h
for header in ['ScopedSwapchainLock.h','QueueIdle_Dx12.h']:
    changes['framegen/'+header]=(kit/'v11'/header).read_text()
for path,content in changes.items():
    (root/path).write_text(content,encoding='utf-8')
print('v11 applied: shared resize lock, queue drains, retained resources and guarded virtual rebuild')
