OptiScaler 0.9.4 MultiGPU FSRFG + XeFG v9 — experimental, unsigned

This build targets the XeFG initialization failure seen with RX 9070 XT
rendering and RX 6600 XT selected for frame generation. Hardware runtime
success has NOT been verified by the build machine.

Changes from v8:
- Preserve the game's D3D12 device while creating the selected FG device.
- Keep XeFG internal DXGI factory creation and adapter enumeration native.
- Keep driver D3D12 helper/probe adapter arguments unchanged. The selected FG
  device and queue are still passed explicitly to XeFG.
- Return false when XeFG context setup returns an SDK error.
- Record hardware exceptions escaping initialization, including module paths,
  fault address, module offsets, and the exception filter's call stack.

Installation for the ASI setup shown in your log:
1. Exit the game and back up the current OptiScaler.asi and OptiScaler.ini.
2. Extract this ZIP to a separate folder.
3. Copy this package's OptiScaler.dll into the game folder as OptiScaler.asi,
   replacing the old OptiScaler.asi. Retain your existing OptiScaler.ini and
   ASI loader. Do not add a second OptiScaler DLL loader name.
4. Run the same scenario. OptiScaler.log should contain "MultiGPU v9".
5. If initialization still fails, provide OptiScaler.log and, if created,
   OptiScaler-MultiGPU-exception.txt from beside RDR2.exe. The exception file
   is appended locally; it is never automatically uploaded. A hang or a fault
   on a different thread may not produce this file.

The exception recorder does not catch-and-continue a corrupt driver state.
It records supported exceptions and preserves normal Windows exception search.

Build checks: Windows MSVC Release x64 build; exact marker verification;
native test of successful return propagation, exception propagation to an
outer handler, and exception-file content. These do not replace testing on
the actual two-GPU system.

D3D12 adapter argument contract:
https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice
