"""Patch only an isolated Intel reference; never the installed OptiScaler DLL."""
from pathlib import Path
import hashlib,json,shutil,sys
root=Path(sys.argv[1])/"samples/basic_sample_frame_generation"
p=root/"basic_sample.cpp";s=p.read_text(encoding="utf-8-sig")
before=hashlib.sha256(p.read_bytes()).hexdigest()
def once(a,b):
    global s
    if s.count(a)!=1: raise RuntimeError("Source anchor not unique: "+a[:100])
    s=s.replace(a,b)
once('#include "basic_sample.h"', '#include "basic_sample.h"\n#include "NativeGate.h"')
once('    xellSleep(m_xellContext, m_frameCounter);',
     '    if (!NativeGateLab::EnvFlag("XEFG_LAB_SLEEP_BYPASS"))\n        xellSleep(m_xellContext, m_frameCounter);')
once('    // Describe and create the swap chain.',
     '    NativeGateLab::FactoryTap nativeDisplayTap(factory.Get(), m_commandQueue.Get());\n\n    // Describe and create the swap chain.')
once('void BasicSample::OnRender()\n{',
     'void BasicSample::OnRender()\n{\n    NativeGateLab::Source(m_lastFrameTimeMS, m_enableXeFG);')
once('            constData.frameRenderTime = m_lastFrameTimeMS;',
     '            constData.frameRenderTime = NativeGateLab::Hint(m_lastFrameTimeMS);')
once('void BasicSample::OnDestroy()\n{',
     'void BasicSample::OnDestroy()\n{\n    NativeGateLab::active.store(false);')
once('    ThrowIfFailed(xefgSwapChainDestroy(m_xefgSwapChain), "Failed to destroy XeSS-FG swap chain context");',
     '    ThrowIfFailed(xefgSwapChainDestroy(m_xefgSwapChain), "Failed to destroy XeSS-FG swap chain context");\n    NativeGateLab::Finish();')
p.write_text(s,encoding="utf-8")
shutil.copyfile(Path(__file__).parent/"NativeGate.h",root/"NativeGate.h")
cm=root/"CMakeLists.txt"
text=cm.read_text(encoding="utf-8-sig")
text += '\nset_property(TARGET basic_xess_fg_sample PROPERTY CXX_STANDARD 17)\n'
cm.write_text(text,encoding="utf-8")
(root/"NATIVE-GATE-MANIFEST.json").write_text(json.dumps({
 "kind":"isolated_native_queue_gate_probe_not_game_patch",
 "source_before":before,"source_after":hashlib.sha256(p.read_bytes()).hexdigest(),
 "header_sha256":hashlib.sha256((root/"NativeGate.h").read_bytes()).hexdigest(),
 "no_resolution_or_shader_changes":True,"no_original_present_drops":True,
 "gate_default":False,"sleep_bypass_default":False,"game_modified":False
},indent=2),encoding="utf-8")
print("Native display queue probe patched; output experiments require hardware validation.")
