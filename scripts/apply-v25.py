"""Apply reviewed v25 delta plus explicit consumer-lifetime reservation.
The readable follow-up protects tagged data until the matching SDK Present
returns, in addition to GPU-copy completion. No source shedding or quality edits.
"""
from pathlib import Path
import base64
import gzip
import hashlib
import subprocess
import sys

kit = Path(__file__).resolve().parents[1]
encoded = ''.join((kit / 'v25/consumer-pacing.patch.gz.b64').read_text().split())
# Deterministic transport correction, followed by strict whole-diff verification.
encoded = encoded.replace('/DS1Vyby', '/DS1FVyby').replace('WS1a3fBj', 'WS1a3dBj')
payload = gzip.decompress(base64.b64decode(encoded, validate=True))
expected = 'f628acd2a4951ab859ea58b84a5243e6f077847d07070f66cf9815a14ebc2986'
actual = hashlib.sha256(payload).hexdigest()
if actual != expected:
    raise RuntimeError(f'v25 source checksum mismatch: {actual}')
patch = kit / 'v25/consumer-pacing.patch'
patch.write_bytes(payload)
print(f'v25 source SHA256 verified: {actual}; {len(payload)} bytes', flush=True)
if '--check-payload' in sys.argv:
    sys.exit(0)
root = kit / 'upstream'
command = ['git', '-C', str(root), 'apply', '--ignore-space-change']
subprocess.run(command + ['--check', str(patch)], check=True)
subprocess.run(command + [str(patch)], check=True)

def replace(s, a, b, count=1):
    if s.count(a) != count:
        raise RuntimeError('v25 reservation anchor mismatch: ' + a[:100])
    return s.replace(a, b)

p = root / 'OptiScaler/framegen/XeFGTransferPool.h'
s = p.read_text(encoding='utf-8-sig')
s = replace(s, '    std::array<Slot, SlotCount> _slots;', '    std::array<Slot, SlotCount> _slots;\n    std::array<bool, SlotCount> _consumerOwned {};')
s = replace(s, '[&] { return _recording != static_cast<int>(slot); }', '[&] { return _recording != static_cast<int>(slot) && !_consumerOwned[slot]; }')
s = replace(s, '        const UINT index = frameSlot % SlotCount;\n        HRESULT hr = WaitUnlocked(index, 5000);', '        const UINT index = frameSlot % SlotCount;\n        if (_consumerOwned[index]) return DXGI_ERROR_INVALID_CALL;\n        HRESULT hr = WaitUnlocked(index, 5000);')
methods = '''    // UNTIL_NEXT_PRESENT resources remain owned beyond GPU-copy submission.
    HRESULT HoldForConsumer(UINT frameSlot)
    {
        std::lock_guard lock(_mutex);
        const UINT index = frameSlot % SlotCount;
        if (_recording != static_cast<int>(index) || _consumerOwned[index])
            return DXGI_ERROR_INVALID_CALL;
        _consumerOwned[index] = true;
        return S_OK;
    }
    void ReleaseConsumer(UINT frameSlot)
    {
        std::lock_guard lock(_mutex);
        _consumerOwned[frameSlot % SlotCount] = false;
        _submitted.notify_all();
    }

'''
s = replace(s, '    HRESULT Submit(UINT frameSlot)\n', methods + '    HRESULT Submit(UINT frameSlot)\n')
p.write_text(s, encoding='utf-8')
p = root / 'OptiScaler/framegen/xefg/XeFG_Dx12.cpp'
s = p.read_text(encoding='utf-8-sig')
s = replace(s, '    const auto result = SubmitConsumerInputs(frame, false);', '    const auto result = SubmitConsumerInputs(frame, false);\n    _inputTransfers.ReleaseConsumer(frame.slot);')
s = replace(s, '    frame.commands = commands;\n    _pendingConsumerFrame', '''    const auto held = _inputTransfers.HoldForConsumer(frame.slot);
    if (FAILED(held)) { _inputTransfers.Abort(frame.slot); return false; }
    frame.commands = commands;
    _pendingConsumerFrame''')
s = replace(s, '        SubmitConsumerInputs(frame, false);\n        return []() -> HRESULT', '        SubmitConsumerInputs(frame, false);\n        _inputTransfers.ReleaseConsumer(frame.slot);\n        return []() -> HRESULT')
s = replace(s, '''        const auto id = frame.token.sdkId;
        ++_consumerFrames;''', '''        struct ReleaseSlot
        {
            MultiGPU::XeFGTransferPool& pool; unsigned slot;
            ~ReleaseSlot() { pool.ReleaseConsumer(slot); }
        } releaseSlot {_inputTransfers, frame.slot};
        const auto id = frame.token.sdkId;
        ++_consumerFrames;''')
p.write_text(s, encoding='utf-8')
# Exercise both boundaries: submission and matching Present retirement.
p = root / 'v25/deferred-input-warp-test.cpp'
s = p.read_text(encoding='utf-8-sig')
s = replace(s, '    cmd->CopyBufferRegion(readback.Get(),0,upload.Get(),0,256);', '    Hr(pool.HoldForConsumer(0),"reserve exact source bundle");\n    cmd->CopyBufferRegion(readback.Get(),0,upload.Get(),0,256);')
s = replace(s, '    Hr(gate->Signal(1),"release GPU gate");', '''    Hr(gate->Signal(1),"release GPU gate");
    Hr(MultiGPU::WaitForQueueIdle(consumer.Get()),"confirm all GPU copies complete");
    Require(waiter.wait_for(30ms)==std::future_status::timeout,"completed copies still owned until SDK Present returns");
    pool.ReleaseConsumer(0);''')
s = replace(s, 'bit-exact copies, abort wakeup, bounded ring reuse, independent producer progress;', 'bit-exact copies, SDK Present reservation, abort wakeup, bounded ring reuse, independent producer progress;')
p.write_text(s, encoding='utf-8')
print('v25 applied: consumer XeLL, immutable packets, SDK Present reservation, MV flags and sampled diagnostics')
