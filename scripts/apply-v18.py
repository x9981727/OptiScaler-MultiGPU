"""v18: restore XeFG auto frame-time source to Input cadence for secondary async MultiGPU.

v15 intentionally used the SDK 'unavailable' / zero sentinel for auto on the
secondary async path. Real-game telemetry from RX 9070 XT + RX 6600 XT shows
that this creates a pacing feedback loop: XeFG Present blocks ~32 ms and the
render thread is consequently pulled down to ~31 FPS. Upstream OptiScaler's
default is Input, so v18 restores that default while preserving explicit
FTInput=1 (Present) and FTInput=2 (Zero) overrides.
"""
from pathlib import Path

kit = Path(__file__).resolve().parents[1]
root = kit / 'upstream' / 'OptiScaler'


def read(path):
    return (root / path).read_text(encoding='utf-8-sig')


def rep(text, old, new, count=1):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'v18 anchor count {actual} != {count}: {old[:120]}')
    return text.replace(old, new)

# Restore upstream-style auto semantics: Input cadence is the default even on
# secondary async XeFG. Explicit FTInput values remain untouched.
p = 'framegen/XeFGFrameTime.h'
s = read(p)
old = '''    // The optional input-duration hint has not been validated against the
    // cadence of this two-device asynchronous pipeline. Auto omits that hint
    // using the SDK-documented unavailable sentinel for this path only.
    out.source = configured.value_or(secondaryAsync ? 2 : 0);'''
new = '''    // v18: Input cadence is the stable default used by upstream OptiScaler.
    // Keeping the real source-frame interval avoids a zero-frame-time pacing
    // feedback loop on the secondary XeFG swapchain. Explicit 1/2 overrides
    // continue to select Present/Zero respectively.
    (void) secondaryAsync;
    out.source = configured.value_or(0);'''
s = rep(s, old, new)
(root / p).write_text(s, encoding='utf-8')

# Add an unmistakable runtime marker so logs prove that the v18 binary is in use.
p = 'framegen/xefg/XeFG_Dx12.cpp'
s = read(p)
anchor = '                LOG_INFO("MultiGPU v17: XeLL latency reduction armed; external sleep pacing bypassed");'
replacement = anchor + '\n                LOG_INFO("MultiGPU v18: XeFG auto frame time restored to Input cadence");'
s = rep(s, anchor, replacement)
(root / p).write_text(s, encoding='utf-8')

print('v18 applied: secondary async XeFG auto frame-time source restored to Input cadence; explicit overrides preserved')
