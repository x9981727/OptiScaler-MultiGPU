OptiScaler 0.9.4 MultiGPU v10 (experimental, unsigned)

Fix: XeFG single-GPU to multi-GPU switching during a running game could
initialize a secondary runtime while retaining the original primary-GPU SDK
context and swapchain. Secondary depth/motion-vector resources were then
passed to that old context. The reported session repeatedly returned XeFG
UNKNOWN (-1000), triggered Streamline exceptions and stopped updating the image.

v10 binds the requested adapter to the lifetime of the XeFG SDK swapchain.
FG activation and frame-resource recreation no longer reselect the adapter.
The shared runtime initialization entry point also refuses to replace the
runtime while that XeFG swapchain survives. Failed SDK destruction retains
the binding; successful destruction releases it.

The menu shows the XeFG GPU actually selected for this launch, detects pending
GPU changes, and requires Save Settings followed by a complete game restart.
New activation is blocked while the adapter selection is pending. An existing
active session can still be disabled; reverting the selection clears the warning.
The backend also guards activation requested outside the menu.

Includes the existing FSRFG v5, XeFG v6 virtual-backbuffer and v9 native
initialization fixes. FSRFG adapter/context recreation is unchanged.

Validation workflow: run the portable C++ adapter-binding regression tests and
compile Windows Release x64. The v9 Windows exception propagation regression
test remains enabled. Consult the build run for results; this description does
not assert that a Windows build has completed. Compiling and passing these
tests do not prove that cold-start dual-GPU XeFG works in a game.
RX 9070 XT + RX 6600 XT hardware validation is still required.

Setup/test:
1. Close the game completely and update the existing OptiScaler loader.
2. Start the game, choose the FG input matching the game's native input and XeFG
   output. Enable separate GPU and select RX 6600 XT. Save Settings.
3. Exit completely, then launch again. Do not use an in-game reload as restart.
4. Confirm "XeFG GPU this launch: AMD Radeon RX 6600 XT" before activating FG.
5. If the image stops updating again, preserve OptiScaler.log from that launch.

Source and reproducible build kit:
https://github.com/x9981727/OptiScaler-MultiGPU
Upstream base: optiscaler/OptiScaler v0.9.4,
commit 7534ad00bf9e590eedb99e8dd9fd8c89dae3654f.
OptiScaler is GPL-3.0; runtime libraries retain their own supplied licenses.
