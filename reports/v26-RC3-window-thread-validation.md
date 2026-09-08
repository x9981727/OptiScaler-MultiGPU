# v26 RC3 window-thread hotfix: completed checks

Build commit: 2533a62f4aec7364f0eb65cc2300c974ab0bb1e5
Workflow run: 34206534588 (success)
Artifact: 10048175313
Artifact SHA256: 5326520fed5b2911d115a9eff60c75432cc19bd97096bd845a443b9fc740df10
Windows x64 game dxgi.dll SHA256: e032f7992049184edb820a727a3d5d99a03bfd48ab514a30fb43294cf23a383d

## Negative control
The unchanged RC2 header was compiled separately. A native Present callback sent a synchronous message to the HWND owner with a bounded 650 ms timeout. Calling Drain on the owner blocked that message; the old test reproduced the failure. This is a controlled reproduction of a code defect, not a trace proving the sole cause of the user's latest game symptom.

## Patched window regression
The compiled RC3 header passed: HWND-owner asynchronous drain, original-thread synchronous Present and Present1, resize and ResizeTarget with pending output, contended submission mutex, actual test-window maximization and client-size buffer reconstruction, restore/minimize, ResizeBuffers1, and normal teardown. Ten sent-message probes completed with zero probe timeouts. A separately posted application command remained queued; the production wait does not remove it.

## Retained regression
PASS: 185000 phase-only policies (30-1000 source FPS, irregular cadence, expired deadlines, disabled state, pressure bypass, no cumulative rate clock). Mathematical policy test, not game FPS validation.
PASS: 42 actual native Presents, sync/async Present1, TEST/DONOTWAIT handling, 3->4->2 buffers, SDR/10bit format resize, ResizeBuffers1 queue mapping, normal teardown. WARP lifecycle only; no XeFG algorithm, real HDR display or FPS acceptance.

## Scope
RC3 changes window-owner waits, preserves the original calling thread for synchronous presentation, and serializes ResizeTarget. It does not change the RC2 PhaseOnlyDeadline.h policy: the archived file is byte-identical. No fixed frame-rate target or recurring deadline clock is introduced. SourcePeriodMs remains ignored. The sidecar still uses Enabled=1, PhaseOffsetMs=-2.2, PhaseParity=0, Trace=1; it is read at swapchain creation, not hot-reloaded.

## Deliverables checked locally
Update ZIP bytes: 12573471
Update ZIP SHA256: 5557121525b9b699d31365b2fbcadc8104fa7e0e57592199403b17743a748217
Full ZIP bytes: 136838836
Full ZIP SHA256: 4b11eff13dd77f50db474cf7cf07e5615cd73bc4f48edf3ac0a011fc28fd4efd
Both ZIPs passed archive integrity and exact content comparison. Full runtime dependencies are unchanged from the RC2 package. Upgrade replaces CopyToGame/dxgi.dll and CopyToGame/XeFGPacing.ini only; preserve the existing OptiScaler.ini and other dependencies. Rollback-v25 contains the earlier saved DLL. Enabled=0 followed by a full game restart disables the new presentation layer.

## Not validated
No RC3 DLL was installed on the user's machine. Remote device was offline. There is no new Enabled=1 failure-session pacing log in this turn. Real 9070 XT + 6600 XT gameplay, exclusive fullscreen, HDR appearance, frame-pacing quality and FPS cost remain unverified. This is a window-regression candidate, not a perfect-pacing or zero-cost release.
