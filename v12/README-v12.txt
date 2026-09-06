OptiScaler MultiGPU v12 - XeFG transfer batching and measured presentation rates

Scope: XeFG on a secondary GPU. Retains v11 resize synchronization and the
existing FSRFG backend. Tested build gates are listed in the associated CI run;
they do not replace RX 9070 XT + RX 6600 XT hardware validation.

Evidence from the supplied v11 run:
- Secondary XeFG initialization and both resolution changes completed.
- An FG-off five-second window averaged 14.50 ms / 69.0 application FPS.
- An early FG-on window averaged 30.38 ms / 32.9 application FPS.
- Across 1,177 active frames, Dispatch-to-first-SDK-tag had a 26.23 ms median.
  That CPU interval contains resource import and allocator waits; it is not a
  measurement of XeFG shader execution time or PCIe bandwidth alone.
- The overlay divided its application-frame rate by the configured multiplier,
  displaying a spurious half-rate as the second number in virtual XeFG mode.
- LogLevel=auto resolved to Trace, producing 414,824 log lines in this run.

Changes:
1. Record secondary input imports and SDK resource tags into one command list.
   Submit once; no CPU wait for the batch that was just submitted.
2. Give XeFG inputs a dedicated four-slot command allocator pool, independent
   of the virtual-backbuffer transfer pool.
3. Before reusing a shared input slot, wait for its prior consumer. Backbuffer
   slot retirement similarly occurs before the next producer overwrite.
   Keep cross-adapter GPU fence dependencies and v11's resize queue drains.
4. Query xefgSwapChainGetLastPresentStatus after each successful non-test proxy
   Present. Render FPS counts application presents; estimated output FPS counts
   frames actually reported queued by the SDK. This is not a monitor scanout
   measurement. Missing status is shown as unknown, not an assumed multiplier.
5. Once per measurement window, report application/SDK rates, CPU import/tag
   recording time, input-slot reuse waits, and interpolation status at Info.

FSRFG continues to use its existing transfer submission path. Only the shared
copy-recording loop is factored out for reuse; its resource/state handling and
caller-side error handling are unchanged. Motion-vector resolution conventions
are preserved; this patch does not force HighResMV=false or resample resources.

Validation:
- Native D3D12 WARP test blocks the GPU, records an import and a dependent copy
  in one batch, checks nonblocking submission and independent pools, exercises
  producer/allocator reuse and abort paths, and validates GPU readback bytes.
- Portable/native presentation-statistics tests cover real SDK frame counts,
  skipped interpolation, unavailable status, transitions and stale samples.
- Existing v9, v10 and v11 Windows tests, complete Release x64 build and markers.

Upstream XeFG requirements and statistics semantics:
https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md#resource-tagging
https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md#execution
https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md#present-status

Source: https://github.com/x9981727/OptiScaler-MultiGPU
Base: OptiScaler v0.9.4, commit 7534ad00bf9e590eedb99e8dd9fd8c89dae3654f,
with cumulative MultiGPU fixes through v12.
