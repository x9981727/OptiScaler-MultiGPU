OptiScaler MultiGPU v11 - resize synchronization fix (experimental)

The reported v10 cold start successfully created the XeFG SDK context and
virtual backbuffers on the selected secondary GPU. Several frames presented
successfully with FG inactive. The log stopped during the first ResizeBuffers
transition from 1334x750 to 3840x2160. It does not contain a crash stack.

Code inspection found a deterministic nested-lock defect: the virtual wrapper
holds FG mutex owner 3, then the SDK resize hook reacquires the same nonrecursive
mutex as owner 6677/6678. The wrapper's GPU-idle helper also never creates its
first fence because its creation branch requires that fence to exist already.
It only considers the render queue, yet the wrapper can release both adapters'
bridge resources before their queued work completes. ResizeBuffers1 additionally
failed to release its outer FG lock for XeFG.

v11 shares scoped locks across wrapper and SDK-hook entry on the same thread,
retains cross-thread exclusion, and releases owned locks on every return.
The entire virtual Present/transfer operation is serialized against resize,
including while FG is inactive. Managed presentation uses a separate owner ID
so legacy EvaluateState owner-2 recovery cannot unlock it asynchronously.

Before SDK resize, the wrapper waits for both render and secondary FG queues.
Each fence is created from the corresponding queue's device, with checked
HRESULTs, device-removal handling and a finite timeout. The rare resize wait
polls at 1 ms to avoid an outstanding event registration after timeout.
Overlay allocators/heaps are also released only after both queues drain.
Timeout/error retains overlay and virtual resources and returns failure.
Existing virtual backbuffers remain alive until SDK resize succeeds; a failed
SDK resize preserves them. Primary device/queue references survive rebuilding.
Failed rebuilding cannot expose native secondary resources to the renderer.

Validation workflow:
- Native nested-lock, cross-thread and unwind regression test.
- Native D3D12 WARP independent-queue, first-fence and timeout test.
- Existing v9 exception-propagation and v10 adapter-binding tests.
- MSVC Release x64 build and embedded marker checks.
Consult the associated run for actual results. WARP tests do not prove
hardware/game compatibility or that XeFG activation succeeds on RX 6600 XT.

Test the existing saved XeFG/6600 XT settings with Active off first. Confirm
the game reaches the main menu and gameplay at the requested resolution, then
activate FG. Preserve the log from the first failure if one remains.

Sources and reproducible build:
https://github.com/x9981727/OptiScaler-MultiGPU
Upstream v0.9.4, commit 7534ad00bf9e590eedb99e8dd9fd8c89dae3654f.
Includes previous FSRFG and XeFG fixes; GPU selection still requires restart.

D3D12 synchronization references:
https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization
https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management
