OptiScaler MultiGPU v13 - dual-GPU XeFG timing diagnostics

This is a diagnostic build. It retains v12 behavior and does not claim to fix
the remaining dual-GPU XeFG performance regression.

The user reports working single-GPU XeFG and working dual-GPU FSRFG. The v12
log shows around 69 application FPS without FG, 35-36 application FPS with
XeFG, and around 70-73 SDK-queued output FPS. In steady state, CPU input batch
recording is about 0.043 ms/call and input slot reuse about 0.001 ms/frame.
These timings do not measure GPU execution, the backbuffer bridge or XeLL sleep.

v13 measures only the dual-GPU XeFG path:
- GPU queue spans around main-GPU backbuffer export, secondary wait/import,
  and the SDK proxy Present's interpolation submissions.
- CPU time in backbuffer-slot reuse, backbuffer transfer, overlay, swapchain
  locks, proxy Present and the intercepted Reflex/fakenvapi Sleep call.
- The current XeLL minimum frame interval and low-latency state via its getter.
- Render, FG and containing-output adapter LUIDs (when DXGI reports an output).

Interpretation:
- Queue timestamps include queue idle gaps and waits between the markers.
  sdkQueueSpanGPU is NOT pure XeFG shader execution time.
- fgWaitImportGPU includes waiting for the main GPU's export fence and copying
  the backbuffer into the secondary proxy. renderExportGPU brackets the primary
  export only. CPU slot waits are separately reported.
- CPU phases overlap: bridgeCPU contains the two slot timings. Do not sum them.
- reflexSleepCPU is per intercepted Sleep call; its call count is logged so
  missing interception or multiple calls per application frame remain visible.
- No samples means -1 (unknown), not zero. A failed XeLL query also means its
  returned/default fields must be ignored.
- GPU measurement uses four independent slots per queue span. Pending slots
  are skipped; telemetry never waits for GPU completion in the frame loop.
  Only teardown drains queues. Probe failures disable that probe, not FG.
- These are diagnostic samples, not monitor scanout measurements.

Test the same scene with FG off for 10 seconds, then XeFG on for 15 seconds,
then off for 10 seconds. Keep the same GPU selection, resolution, cap and VSync
settings during this test. Return OptiScaler.log and OptiScaler.ini. There is
no need to repeat single-GPU validation for this capture.

Scope: no change to algorithms, motion-vector resolution, game frame caps,
VSync, XeLL sleep settings, adapter selection, or existing synchronization.
The FSRFG path uses no-op telemetry methods and retains its previous behavior.
No user-uploaded logs, INI or screenshots are added to the public repository.

Build gates: the retained six Windows v9-v12 tests, a new WARP test for
asynchronous query readback/reuse and CPU counters, full Release x64 compilation,
and 31 binary markers. Hardware performance remains to be measured by the user.

Source: https://github.com/x9981727/OptiScaler-MultiGPU
Upstream: OptiScaler v0.9.4, commit 7534ad00bf9e590eedb99e8dd9fd8c89dae3654f.
SDK timing semantics:
https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md
https://github.com/intel/xess/blob/main/doc/xell_developer_guide_english.md
