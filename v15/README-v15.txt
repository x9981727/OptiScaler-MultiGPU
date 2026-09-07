OptiScaler MultiGPU v15 - frame-time persistence and secondary pacing test

This is an experimental pacing change, not a verified cure for the remaining
render-FPS loss. It retains v14 bounded asynchronous secondary XeFG Present,
GPU fences, resource lifetime guards and the existing FSRFG path.

Confirmed configuration bug
The inherited configuration saved FrameGen/FTInput but loaded FrameGen/FTSource.
v15 loads the canonical FTInput key, with legacy FTSource used only if FTInput
does not exist. A canonical auto/empty/invalid value clears a stale override.
The same valid source ranges are retained: XeFG 0..2, other backends 0..1.
The Windows test round-trips a saved INI through the production SimpleIni library.

Experimental pacing behavior
FrameGen/FTInput meanings remain 0=input, 1=application Present interval, 2=zero.
An explicit choice always takes priority. With FTInput=auto, eligible secondary
asynchronous XeFG frames now submit frameRenderTime=0, the SDK's documented
unavailable-value sentinel. This omits an optional timing hint whose effect in
the two-device pipeline has not been verified. It does not set rendering time
to zero or remove the actual GPU interpolation work.
Auto for single-GPU or synchronous XeFG retains the input source. FSRFG does not
use the new XeFG selector. Nonfinite or negative timing hints are submitted as
unavailable (zero), and counted in diagnostics.
XeLL Sleep, markers, low-latency mode, frame limits, VSync, motion-vector settings
and all existing synchronization remain in place. No timing-wait sum is used
to decide whether a wait is redundant.

Diagnostics
MultiGPU v15 config: FTInput=-1 means auto; 0/1/2 means an explicit source.
legacyUsed=true means the old FTSource key supplied the setting.
MultiGPU v15 frameTime: requested, lastSource, frames, per-source counts,
invalidFrames, sdkFrameMs, inputFrameMs and applicationPresentMs.
Frames/counts and means cover each reporting window. Empty windows use -1 for
unknown means and source. Source 2 means an omitted hint; explicit source 0/1
with invalid timing also yields zero, counted by invalidFrames.
The v14 policy and handoff logs and v13 queue/CPU timing remain available.
SDK queued-output estimates do not measure monitor scanout. GPU spans include
waits/idle. CPU phases can overlap and must not be added together.

Testing and rollback
Keep current hardware/settings and use windowed/borderless mode. Keep the
current per-machine INI; do not copy a saved adapter LUID from an earlier boot.
Set the existing [FrameGen] FTInput to auto or 2 for the zero-hint test.
Set [FrameGen] FTInput=0 and restart for the original input-hint behavior.
Use the same scene: FG off 10 s, XeFG on 15 s, FG off 10 s. Return the new log
and current INI. No additional single-GPU baseline is required.

Validation gates
Retain all nine Windows v14 tests. Add frame-time configuration precedence and
SimpleIni save/reload, legacy compatibility, automatic/explicit source policy,
single-GPU/synchronous behavior, invalid-value handling and concurrent telemetry.
Require a full Windows MSVC Release x64 build and 35 compiled binary markers.
These tests do not measure hardware FPS or prove game/driver compatibility.

References
https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md#frame-constants
https://github.com/intel/xess/blob/main/doc/xell_developer_guide_english.md#sleep
https://github.com/x9981727/OptiScaler-MultiGPU
No user logs, INI files or screenshots are included in the public build kit.
