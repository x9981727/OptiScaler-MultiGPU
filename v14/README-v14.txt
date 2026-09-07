OptiScaler MultiGPU v14 - bounded secondary XeFG Present overlap

This is an experimental performance fix, requiring hardware validation. It does
not claim a measured FPS gain on RX 9070 XT + RX 6600 XT.

Why
v13 showed approximately 66 application FPS without FG, 35 application FPS with
XeFG, and about 25 ms inside the SDK proxy Present. Main-GPU backbuffer export
was around 1.2 ms. The SDK queue span included waits/idle and was not a pure
shader-duration measurement. This identifies a blocking region, not the full
internal SDK cause.

Change
- Submit only the final secondary XeFG SDK Present to a dedicated worker.
- Keep exactly one uncollected Present. At the next outer application Present,
  collect its result before importing the next backbuffer, tagging SDK inputs,
  changing the present ID or issuing a TEST Present.
- Allow the game to prepare/render its next frame during the pending Present.
- Maintain the main-GPU virtual backbuffer index separately from the SDK's
  current destination index. Existing producer/consumer GPU fences are retained.
- Capture SDK status once after completion. Deferred errors are delivered on
  the next outer Present; device failures remain sticky until context recreation.
- Retain the SDK swapchain during the call; release that reference before
  advertising completion. Drain queued work before lifecycle changes.
- Make recursion flags thread-local and establish both flags on the worker.
- Keep FSRFG on its existing path. Single-GPU XeFG does not use this worker.

Eligibility and rollback
[XeFG] AsyncPresent = auto (or true) enables the new policy for eligible
dual-GPU XeFG frames. The frame must use a windowed swapchain with at least two
buffers, and the present caller must differ from the window's message thread.
Use windowed/borderless mode when testing. Exclusive fullscreen, partial Present1
updates, TEST, DO_NOT_WAIT and other special flags use synchronous presentation.
Present1 with empty parameters is supported without retaining caller-owned data.
Native frame-latency waitable objects and VSync/frame-cap settings are preserved;
an engine's own waitable-object pacing can limit the overlap benefit.
Set AsyncPresent = false and restart to compare synchronous vs deferred behavior
in the same v14 build. The packet contains no forced adapter or resolution change.
One pending operation bounds extra queued work but can affect input latency.

Logs
MultiGPU v14 policy: eligible=... identifies whether the outer swapchain permits
the handoff, including window/present thread IDs, flags and buffer count.
MultiGPU v14 present: asyncSetting=..., queued=..., completed=...,
nextPresentWaitCPU=..., waitCalls=..., pendingResult=... reports actual handoffs.
Counts are per reporting window; a boundary can put a submission and completion
in adjacent windows. nextPresentWaitCPU is the CPU wait at the next Present.
v13 proxyPresentCPU now measures SDK work on the worker for deferred calls.
v12 render/queued-output statistics and the other v13 diagnostic fields remain.
GPU spans include waits/idle; CPU phases may overlap and must not be summed.

Validation gates
Retain the seven previous Windows tests and add two tests:
1. Deferred Present ownership/overlap, FIFO result delivery, HRESULT and C++
   exception propagation, shutdown drain, virtual-buffer rotation and TLS policy.
2. WARP render/FG queues: render the next buffer while the FG queue is blocked,
   verify the pending frame's data is unchanged, then drain before teardown.
Also require full MSVC Release x64 compilation and 33 binary markers.
WARP exercises synchronization; it does not execute the XeFG hardware algorithm
or prove compatibility with a game's DXGI/window callbacks.

Test
Use the same scene and settings: FG off 10 s -> XeFG on 15 s -> FG off 10 s.
Return OptiScaler.log and OptiScaler.ini. No additional single-GPU run is needed.

Source and design references:
https://github.com/x9981727/OptiScaler-MultiGPU
https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md
https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi#multithread-considerations
No user logs, INI files or screenshots are included in the public build kit.
