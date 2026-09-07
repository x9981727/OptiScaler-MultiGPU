"""Reconstruct v25 reviewed source/test bundle, then apply to reconstructed v24.
The readable finalization below keeps cross-engine fence lifetimes conservative.
No quality reduction, fake Present success, or source shedding is introduced.
"""
from pathlib import Path
import base64
import gzip
import hashlib
import json
import subprocess

kit = Path(__file__).resolve().parents[1]
parts = [kit / ('v25/payload.part%02d' % i) for i in range(3)]
encoded = ''.join(p.read_text(encoding='ascii').strip() for p in parts)
raw = gzip.decompress(base64.b64decode(encoded, validate=True))
expected = '318587f8d67e0e2f544ffa1a2556bd6aa42f6a25b3011423dadd5a9d5cec64d5'
if hashlib.sha256(raw).hexdigest() != expected:
    raise RuntimeError('v25 payload checksum mismatch; refusing to patch')
payload = json.loads(raw)
allowed = {'managed-pacing.patch', 'protocol-test.cpp', 'protocol-test.vcxproj',
           'copy-pipeline-test.cpp', 'copy-pipeline-test.vcxproj', 'README-v25-zhTW.txt'}
if set(payload) != allowed or any(not isinstance(v, str) for v in payload.values()):
    raise RuntimeError('Unexpected v25 bundle contents')
for name, text in payload.items():
    (kit / 'v25' / name).write_text(text, encoding='utf-8')
patch = kit / 'v25/managed-pacing.patch'
cmd = ['git', '-C', str(kit / 'upstream'), 'apply', '--ignore-space-change']
subprocess.run(cmd + ['--check', str(patch)], check=True)
subprocess.run(cmd + [str(patch)], check=True)
root = kit / 'upstream/OptiScaler'

def once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('v25 finalization anchor mismatch: ' + old[:100])
    return text.replace(old, new)

wrapper_path = root / 'wrapped/wrapped_swapchain.cpp'
wrapper = wrapper_path.read_text(encoding='utf-8-sig')
release = '''    _primaryExports.reset(); _primaryExportQueue.Reset(); _primaryReadyFence.Reset();
    _primaryExportFence.Reset(); _fgExportFence.Reset(); _primaryExportSlots.clear(); _primaryExportSerial = 0;
'''
wrapper = once(wrapper, release, '')
anchor = '    _multiGpuColorCopies.reset();\n'
guard = '''    // Queue Wait commands also reference these fences. Retire every consumer
    // before releasing either the source fence or its secondary-device handle.
    if (_primaryExportFence != nullptr)
    {
        if (_multiGpuRuntime != nullptr && _multiGpuRuntime->FGQueue() != nullptr)
        {
            const auto hr = MultiGPU::WaitForQueueIdle(_multiGpuRuntime->FGQueue());
            if (FAILED(hr)) LOG_WARN("MultiGPU v25: export consumer drain returned {:X}", (UINT)hr);
        }
        if (_multiGpuRenderQueue != nullptr)
        {
            const auto hr = MultiGPU::WaitForQueueIdle(_multiGpuRenderQueue.Get());
            if (FAILED(hr)) LOG_WARN("MultiGPU v25: source reuse drain returned {:X}", (UINT)hr);
        }
    }
''' + release + anchor
wrapper = once(wrapper, anchor, guard)
wrapper_path.write_text(wrapper, encoding='utf-8')

xefg_path = root / 'framegen/xefg/XeFG_Dx12.cpp'
xefg = xefg_path.read_text(encoding='utf-8-sig')
xefg = once(xefg, '                _deferredResult = presentResult;',
             '                _deferredResult = presentResult;\n                _managedReset = true;')
xefg = once(xefg, '                _deferredResult = result;',
             '                _deferredResult = result;\n                _managedReset = true;')
xefg_path.write_text(xefg, encoding='utf-8')

hooks = (root / 'hooks/Reflex_Hooks.cpp').read_text(encoding='utf-8-sig')
if hooks.count('MultiGPU::OwnsSecondaryXeLL(') != 4:
    raise RuntimeError('Missing scoped controller guards')
if 'const bool bypassSecondaryPacing = State::Instance().activeFgOutput == FGOutput::XeFG' not in hooks:
    raise RuntimeError('FSRFG sleep bypass scope regression')
for marker in ('RunManagedXeFGProtocol', '_managedRetained[slot] = packet->retained',
               'if (!managed && secondaryAsyncFrame', 'frameTime.sdkMs = 0.0f',
               '_managedLastSourceId', 'XEFG_SWAPCHAIN_UI_MODE_NONE'):
    if marker not in xefg:
        raise RuntimeError('Missing v25 production anchor: ' + marker)
print('v25 applied: compositor-owned XeLL packet protocol, independent primary COPY export, strict XeFG controller scope, fence lifetime drain')
print('Payload SHA256:', expected)
