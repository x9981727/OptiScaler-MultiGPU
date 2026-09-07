# XeFG virtual regression laboratory

Acceptance target: preserve the same-quality 9070 XT source cadence near the measured no-FG baseline (~67 FPS), with one genuinely evenly presented generated frame per source (~134 displayed FPS), without flicker, dropped originals or unbounded latency. A software simulation is not a benchmark of a Radeon GPU.

## What actually runs

The `XeFG virtual regression laboratory` workflow provisions a fresh Windows VM. It reconstructs the exact v25 source chain, applies the confirmed source-only sequence fix, then runs:

1. The original adapter/lifetime, queue, WARP and protocol regressions.
2. A production-header protocol stress test: 32 deterministic seeds, 1,048,576 events, missing-data bursts and reactivation, injected errors/exceptions, all 16 backend scopes, and ID wraparound. API callbacks are mocks; this is not execution of Intel's scheduler.
3. Real D3D12 WARP copies through production COPY helpers/pools. Cases include 3840x2160 R10G10B10A2, 3840x2160 R16G16B16A16, 2953x1661 depth/MV formats and odd row pitches. Every pixel word is checked after transfer. Three slots and four queues, delayed consumers, premature reuse, and drained recreation are exercised. This is one software adapter, not a PCIe simulation of two AMD cards, and it does not run the XeFG interpolation algorithm.
4. A capability probe that enumerates real VM adapters and actually attempts XeFG/XeLL DLL initialization on WARP. Unsupported, missing, aborted and timed-out states are recorded, never treated as a hardware pass.
5. PresentMon timing-parser controls: ideal balanced 67->134, burst timing, balanced-but-halved throughput, missing generated frames, drops, malformed/short input. Every timing-only result explicitly excludes pixel/latency/hardware certification.
6. Windows Release compilation of the sequence-patched candidate. The candidate game DLL is NOT uploaded or labelled Final; only tests and reports are published.

The confirmed fix changes when the compositor ID is allocated. It does not claim to fix sustained v25 throughput or flicker. A green virtual-lab workflow means the lab's checks ran, NOT that game acceptance passed. `lab-results/release-state.json` records remaining blockers.

## Reproduce locally

On Windows with the reconstructed source chain and MSVC/CMake:

```
python lab/prepare_lab.py
cmake -S lab -B lab-build -A x64 -T v143
cmake --build lab-build --config Release
ctest --test-dir lab-build -C Release --output-on-failure --no-tests=error
python -m unittest discover -s lab -p test_trace_gate.py -v
python lab/run_probe.py
```

On Linux, set PRODUCTION_ROOT to the reconstructed `OptiScaler` directory. CMake enables ASan and UBSan; Windows-only tests are not claimed as run there.

Analyze a physical capture without uploading it to this public repository:

```
python lab/evaluate_trace.py capture.csv --target-source-fps 67 --output report.json --strict
```

All complete 5-second windows after the declared warm-up are retained, including original-only/fallback intervals. The tool never chooses the fastest window or equates SDK-queued counts to measured display changes. Its engineering thresholds are visible in the source. A reported target is not a substitute for a matched OFF capture. Tearing may produce partial scanout.

## Final release blockers

A final game binary still requires matching binary hashes, real 9070 XT and 6600 XT adapter selection, paired same-scene/same-quality OFF/ON captures, per-frame displayed cadence, pixel/ghosting/flicker inspection, and end-to-end latency/queue-depth evidence. None can be certified by changing GPU names in a VM or by replaying CSV timestamps. No self-hosted runner is silently registered and no paid cloud GPU is provisioned by this workflow.

This workflow runs on pushes changing the laboratory or code-patch inputs, and by explicit workflow dispatch. It does not claim an unattended AI continues changing code after a chat ends.

## Primary references

- Microsoft WARP: https://learn.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp
- Intel XeSS-FG / XeLL developer guides in pinned submodule `207b703ad215da5b86dde04819a16277a96980aa`.
- PresentMon console metrics: https://github.com/GameTechDev/PresentMon/blob/v2.5.1/README-ConsoleApplication.md

Use only permitted offline/modding environments. Do not disable or circumvent anti-cheat. User raw logs/CSV and personal filesystem paths are not committed to the public repository.
