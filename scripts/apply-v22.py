"""v22: remove unsafe outer-Present shedding while retaining v19/v20 pacing fixes.

Real-game telemetry from v21 shows that returning S_OK for a dropped outer
Present decouples the render-side virtual cursor from the Depth/Velocity/frame-ID
lifecycle. The result is repeated missing input resources, frame-count jumps,
false 200-300 FPS accounting while XeFG is inactive, and a much lower real
render rate once XeFG is active.

v22 keeps the useful v21 nonblocking completion probe for diagnostics, but
restores exact previous-Present completion before the next outer Present touches
virtual backbuffers or FG input ownership. This deliberately prioritizes
correctness and the known-good v19/v20 behavior while a proper multi-slot FG
mailbox is developed at the staging layer rather than by faking game Present.
"""
from pathlib import Path
import subprocess
import sys

kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream' / 'OptiScaler'


def read(path):
    return (root / path).read_text(encoding='utf-8-sig')


def rep(text, old, new, count=1):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'v22 anchor count {actual} != {count}: {old[:180]}')
    return text.replace(old, new)

p = 'wrapped/wrapped_swapchain.cpp'
s = read(p)
start_token = '    const bool v21AsyncEligible = AllowMultiGPUQueuedPresent(SyncInterval, Flags, '
end_token = '    HRESULT result;'

# Present and Present1 each contain the v21 shedding block. Replace the complete
# block with the exact-completion handoff used by the known-good v20 path.
for _ in range(2):
    start = s.find(start_token)
    if start < 0:
        raise RuntimeError('v22 could not locate v21 backpressure block')
    end = s.find(end_token, start)
    if end < 0:
        raise RuntimeError('v22 could not locate end of v21 backpressure block')
    end += len(end_token)
    replacement = '''    const HRESULT previousPresent = FinishMultiGPUQueuedPresent(true);\n    if (previousPresent != S_OK) return previousPresent;\n    HRESULT result;'''
    s = s[:start] + replacement + s[end:]

(root / p).write_text(s, encoding='utf-8')

# Add an unmistakable runtime marker.  Keep the v20 baseline freeze unchanged.
p = 'framegen/xefg/XeFG_Dx12.cpp'
s = read(p)
anchor = '            LOG_INFO("MultiGPU v20 frameTime: pre-FG baseline frozen while active={}", IsActive());'
marker = anchor + '\n            LOG_INFO("MultiGPU v22: exact outer Present completion restored; v21 source shedding disabled");'
s = rep(s, anchor, marker)
(root / p).write_text(s, encoding='utf-8')

print('v22 applied: unsafe outer-Present source shedding disabled; exact completion restored')
subprocess.run([sys.executable, str(kit / 'scripts' / 'apply-v23.py')], check=True)
