from pathlib import Path
import hashlib,json,shutil,sys
repo=Path(__file__).resolve().parents[1]
root=Path(sys.argv[1]).resolve()/ 'samples/basic_sample_frame_generation'
f=root/'basic_sample.cpp';s=f.read_text(encoding='utf-8-sig');before=hashlib.sha256(f.read_bytes()).hexdigest()
def once(old,new):
 global s
 if s.count(old)!=1:raise RuntimeError('Expected unique source anchor: '+old[:100])
 s=s.replace(old,new)
# Existing ring source is the common control in both modes. No shaders/resolution change.
once('void BasicSample::OnSleep()\n{','extern "C" bool FastReferenceMode();\nextern "C" void InstallNativeGate(IDXGIFactory2*,HWND,ID3D12CommandQueue*);\nextern "C" void FinishNativeGate();\n\nvoid BasicSample::OnSleep()\n{\n    if (FastReferenceMode()) return; // isolated control reproduces the current external-sleep bypass')
once('    assert(applicationSwapChain == nullptr);','    assert(applicationSwapChain == nullptr);\n    InstallNativeGate(factory.Get(), Win32Application::GetHwnd(), m_commandQueue.Get());')
once('            constData.frameRenderTime = m_lastFrameTimeMS;','            constData.frameRenderTime = FastReferenceMode() ? 3.574f : m_lastFrameTimeMS;')
once('    ThrowIfFailed(xellDestroyContext(m_xellContext), "Failed to destroy XeLL context");','    ThrowIfFailed(xellDestroyContext(m_xellContext), "Failed to destroy XeLL context");\n    FinishNativeGate();')
f.write_text(s,encoding='utf-8');shutil.copyfile(repo/'lab-native-gate/native_gate.cpp',root/'native_gate.cpp')
cm=root/'CMakeLists.txt'
with cm.open('a',encoding='utf-8') as out:
 out.write('\nset_property(TARGET basic_xess_fg_sample PROPERTY CXX_STANDARD 17)\ntarget_sources(basic_xess_fg_sample PRIVATE native_gate.cpp)\ntarget_include_directories(basic_xess_fg_sample PRIVATE "'+(repo/'detours/src').as_posix()+'")\ntarget_link_libraries(basic_xess_fg_sample PRIVATE "'+(repo/'detours/lib.X64/detours.lib').as_posix()+'")\n')
(root/'NATIVE-GATE-MANIFEST.json').write_text(json.dumps({'kind':'isolated_native_queue_fence_experiment_not_game_release','base_intel_commit':'207b703ad215da5b86dde04819a16277a96980aa','ring_source_sha256':before,'modified_sample_sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'native_gate_sha256':hashlib.sha256((root/'native_gate.cpp').read_bytes()).hexdigest(),'gate_max_pending':12,'game_modified':False,'shader_or_resolution_changed':False,'gpu_queue_must_differ_from_interpolation_queue':True,'fast_reference_is_deliberately_nonstandard_control':True,'acceptance_not_proven':True},indent=2),encoding='utf-8')
print('Isolated native gate experiment prepared; no game release.')
