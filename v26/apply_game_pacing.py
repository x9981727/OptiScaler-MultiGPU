from pathlib import Path
import hashlib,json,shutil,sys
kit=Path(__file__).resolve().parents[1]
root=kit/'upstream/OptiScaler'
header=(kit/'v26/GameOutputPacing.h').read_text(encoding='utf-8')
def once(text,old,new):
    if text.count(old)!=1:raise RuntimeError('v26 anchor mismatch: '+old[:120])
    return text.replace(old,new)
header=once(header,'UINT cursor=0;UINT64 serial=0;','UINT cursor=0;std::atomic<UINT64> serial{0};')
start=header.index('inline void Finish(void* owner)')
end=header.index('class FactoryScope {',start)
header=header[:start]+'''inline void Finish(void* owner){
    auto s=session.load();if(!s||s->owner!=owner)return;
    HRESULT hr;
    {
        std::lock_guard<std::recursive_mutex> lock(s->submitMutex);
        hr=s->Stop();
        *reinterpret_cast<void***>(s->chain.Get())=s->oldTable;
        session=nullptr;
    }
    if(SUCCEEDED(hr))delete s;
    // Device-loss failure retains allocations until process exit rather than
    // freeing resources that a failed queue may still reference.
}
'''+header[end:]
(root/'framegen/xefg/GameOutputPacing.h').write_text(header,encoding='utf-8')
p=root/'framegen/xefg/XeFG_Dx12.cpp';s=p.read_text(encoding='utf-8-sig')
s=once(s,'#include "XeFG_Dx12.h"','#include "XeFG_Dx12.h"\n#include "GameOutputPacing.h"')
anchor='        ScopedMultiGpuXeFGInit isolateXeFGInit {};'
if s.count(anchor)!=2:raise RuntimeError('Expected two protected XeFG creation sites')
s=s.replace(anchor,anchor+'''\n        XeFGGamePacing::FactoryScope gameOutputPacing(_swapChainContext, factory12, xefgQueue, hwnd);
        LOG_INFO("MultiGPU v26 RC1: optional native output pacing, configuration XeFGPacing.ini; game-test candidate");''')
s=once(s,'        auto result = XeFGProxy::Destroy()(context);','''        XeFGGamePacing::Pause(context);
        auto result = XeFGProxy::Destroy()(context);
        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            XeFGGamePacing::Finish(context);''')
s=once(s,'void XeFG_Dx12::Deactivate()\n{','''void XeFG_Dx12::Deactivate()
{
    XeFGGamePacing::Source(_swapChainContext, 0.0, false);''')
s=once(s,'bool XeFG_Dx12::Present()\n{','''bool XeFG_Dx12::Present()
{
    // Output cadence uses the actual source-present interval, independently of
    // the retained SDK frame-time hint. No game-render-thread Sleep is added.
    if (IsMultiGPUActive())
        XeFGGamePacing::Source(_swapChainContext, State::Instance().lastFGFrameTime, IsActive());''')
p.write_text(s,encoding='utf-8')
# Header-only inclusion intentionally requires no unreviewed project file edits.
manifest={'kind':'OptiScaler-v26-RC1-game-candidate','baseline_recipe_commit':'11839e091686bd6a6118330096bd6f91493c1742',
          'native_output':'bounded complete-image FIFO, independent DIRECT display queue, phase adjustment',
          'automatic_display_feedback':False,'default_phase_ms':-2.2,'quality_settings_changed':False,
          'game_FPS_or_image_quality_verified_by_build':False,'new_configuration_file':'XeFGPacing.ini',
          'resize_implemented':True,'HDR_metadata_forwarding_implemented':True,
          'native_DoNotWait_supported':False,
          'source_changes':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest(),
                            'framegen/xefg/GameOutputPacing.h':hashlib.sha256(header.encode()).hexdigest()}}
(kit/'v26/INTEGRATION-MANIFEST.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
(kit/'v26/GameOutputPacing-compiled.h').write_text(header,encoding='utf-8')
print(json.dumps(manifest),flush=True)
