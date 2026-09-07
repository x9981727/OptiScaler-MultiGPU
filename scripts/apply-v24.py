"""Apply v24 full-resolution COPY prefetch and its validated COMMON-layout handoff.

The compressed base diff is verified and applied to reconstructed v23. The
readable follow-up below fixes the COPY layout violation caught by our first
WARP run. COPY textures stay in COMMON using implicit promotion/decay; explicit
DIRECT resource barriers are retained. No quality, frame-time or dropping edits.
"""
from pathlib import Path
import base64
import gzip
import hashlib
import subprocess

kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream/OptiScaler'
encoded = ''.join((kit / 'v24/exact-2x.patch.gz.b64').read_text().split())
payload = gzip.decompress(base64.b64decode(encoded, validate=True))
expected = '38becc44e0545b0fa467fb9fbd04bf9316be112ac2e1c9f9d5fcd439b06b8d43'
if hashlib.sha256(payload).hexdigest() != expected:
    raise RuntimeError('v24 source diff checksum mismatch')
patch = kit / 'v24/exact-2x.patch'
patch.write_bytes(payload)
command = ['git', '-C', str(kit / 'upstream'), 'apply', '--ignore-space-change']
subprocess.run(command + ['--check', str(patch)], check=True)
subprocess.run(command + [str(patch)], check=True)

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('v24 COMMON layout anchor mismatch: ' + old[:100])
    return text.replace(old, new)

helper_path = root / 'framegen/CopyCommonTexture.h'
helper = helper_path.read_text(encoding='utf-8-sig')
anchor = 'namespace MultiGPU\n{\n'
copy_helper = '''// COPY queues use COMMON texture layout. No explicit legacy COPY_DEST/SOURCE
// transitions here: implicit promotion/decay plus the exact queue fences provide
// the handoff. The caller must own both COMMON resources until this copy retires.
inline HRESULT CopyCommonBufferToTexture(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer,
                                        ID3D12Resource* texture,
                                        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint)
{
    if (list == nullptr || buffer == nullptr || texture == nullptr) return E_POINTER;
    const auto a = buffer->GetDesc();
    const auto b = texture->GetDesc();
    if (list->GetType() != D3D12_COMMAND_LIST_TYPE_COPY ||
        a.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        b.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        b.Width != footprint.Footprint.Width || b.Height != footprint.Footprint.Height ||
        b.Format != footprint.Footprint.Format || footprint.Footprint.Depth != 1 ||
        b.MipLevels != 1 || b.DepthOrArraySize != 1 || b.SampleDesc.Count != 1)
        return E_INVALIDARG;
    D3D12_TEXTURE_COPY_LOCATION source {}, destination {};
    source.pResource = buffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    return S_OK;
}

'''
helper = replace_once(helper, anchor, anchor + copy_helper)
helper_path.write_text(helper, encoding='utf-8')

runtime_path = root / 'framegen/MultiGPU_Dx12.cpp'
runtime = runtime_path.read_text(encoding='utf-8-sig')
runtime = replace_once(runtime, '#include "MultiGPU_Dx12.h"',
                       '#include "MultiGPU_Dx12.h"\n#include "CopyCommonTexture.h"')
old = '''    Transition(fgCmdList, _fgSharedBuffer.Get(), _fgSharedState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(fgCmdList, _fgLocalTexture.Get(), _fgLocalState, D3D12_RESOURCE_STATE_COPY_DEST);'''
new = '''    if (fgCmdList->GetType() == D3D12_COMMAND_LIST_TYPE_COPY)
    {
        if (_fgSharedState != D3D12_RESOURCE_STATE_COMMON ||
            _fgLocalState != D3D12_RESOURCE_STATE_COMMON || finalState != D3D12_RESOURCE_STATE_COMMON)
            return false;
        return SUCCEEDED(CopyCommonBufferToTexture(fgCmdList, _fgSharedBuffer.Get(),
                                                   _fgLocalTexture.Get(), _footprint));
    }

''' + old
runtime = replace_once(runtime, old, new)
runtime_path.write_text(runtime, encoding='utf-8')

# Exercise the exact production helper, rather than keeping a separate test-only
# copy implementation. Do not weaken or disable D3D12 error validation.
test_path = kit / 'v24/copy-pipeline-test.cpp'
test = test_path.read_text(encoding='utf-8-sig')
start = test.index('        Barrier(list,transport[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);')
end = test.index('        Hr(imports.Submit(slot)', start)
test = test[:start] + '''        Hr(MultiGPU::CopyCommonBufferToTexture(list,transport[slot].Get(),staging[slot].Get(),footprint),
           "production COMMON-layout COPY upload");
''' + test[end:]
test = replace_once(test, '        src={};dst={};src.pResource=proxy[slot].Get();',
                    '        D3D12_TEXTURE_COPY_LOCATION src {},dst {};src.pResource=proxy[slot].Get();')
test = replace_once(test, 'd.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;',
                    'd.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;')
test = replace_once(test, 'desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;',
                    'desc.Width=width;desc.Height=height;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;')
test_path.write_text(test, encoding='utf-8')
print('v24 applied: dedicated COPY prefetch with COMMON layout + exact destination lookup; source shedding disabled')
