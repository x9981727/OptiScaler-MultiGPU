# v25: XeFG compositor pacing and primary COPY export experiment

Target: retain same-quality source rendering near the no-FG baseline and display one evenly spaced generated frame per original (67 -> approximately 134), without shedding original Presents. This is an acceptance target, not an achieved hardware result.

The source bundle is decoded and checksum-verified by scripts/apply-v25.py. It contains a readable unified source diff and tests. Decoded sources are included with CI artifacts.

## Changes
- A secondary XeFG worker owns the XeLL Sleep and six compositor-phase markers, immutable Depth/MV/constants packet, SDK tags and native Present. The phase markers describe compositor work, not measured game simulation or main-GPU execution.
- Managed mode removes v19's artificially short frozen-CPU-gap override and uses the documented unavailable frameRenderTime sentinel (0) with this new worker timeline. It does not reuse the FTInput=1 feedback measurement.
- SDK ONLY_NOW tag copies use a separate allocator pool; strong texture references survive until that pool slot's GPU fence retires. A discontinuous source ID resets interpolation history.
- The main GPU exports color through an independent COPY queue and a separate single-producer cross-adapter fence, retaining v24's secondary COPY import. A GPU dependency prevents overwriting source textures still being read by COPY.
- Fix an inherited Reflex Sleep bypass predicate that was not explicitly limited to XeFG. FSRFG is excluded; FSRFG's actual runtime performance has not been benchmarked here.
- Explicit backbuffer-only UI mode matches the existing multi-GPU input contract; this does not provide a separate HUD or promise no interpolation artifacts in UI.
- Missing input/API errors use one real original-only Present, not a fabricated generated-frame success. This fallback must be counted in testing.

## Settings (restart required)
```
[FrameGen]
FTInput=0
[XeFG]
AsyncPresent=true
AsyncColorTransfer=true
ManagedPacing=true
AsyncRenderExport=true
```
The last two settings are new in v25. FSRFG does not read them as FSR frame-generation options. Do not replace the entire INI or change quality, render size, GPU LUIDs or frame multiplier.

Rollback within v25: ManagedPacing=false and AsyncRenderExport=false restores old scheduling/export behavior, but retains the scope and explicit UI fixes. Restore the backed-up v24 DLL for an exact version rollback.

## Validation boundaries
Portable tests check the production protocol helper's order, IDs, return codes, error paths and backend gating. WARP tests check 67x13 R10G10B10A2 textures through four queues and three slots, including a blocked consumer. Neither is an AMD dual-GPU benchmark or a XeFG algorithm/visual-quality test.

The old renderExportGPU timer excludes new primary COPY work. SDK queue counts are not measured scanout. Validate with same-scene FG OFF/ON logs and PresentMon display timestamps, including generated-frame residence, drop/fallback counts and latency. A 14ms/0.8ms cadence is not a successful smooth 2x result.

The public XeFG/XeLL integration primarily describes single-GPU operation. This is experimental multi-GPU compositor scheduling. It may regress or fail on a particular driver, and it cannot promise zero copy/memory contention or removal of all ghosting. Test only in environments that permit offline game mods; do not use to circumvent anti-cheat.
