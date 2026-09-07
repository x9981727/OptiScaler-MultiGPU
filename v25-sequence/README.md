# v25 sequence audit — source-only corrective patch

The v25 compositor experiment failed hardware acceptance: reduced source frame rate and reported flicker. Do not treat the v25 WARP/protocol test passes as a visual-quality or performance validation. No replacement game DLL is released by this branch.

## Confirmed source defect

`RunManagedFrame` increments `_managedSerial` before checking whether an input packet is complete. An incomplete packet then takes the original-only fallback without calling XeLL Sleep or markers. Re-entering managed generation consequently skips XeLL IDs. The portable test reproduces requested ID 2437 while expecting 73 after 2364 incomplete packets.

The patch allocates an ID only for a complete protocol attempt and rejects wraparound. It does not consume an ID for an incomplete original-only packet. It does not change quality, SDK pacing hints, GPU copies, or source Present semantics. Handling an arbitrary XeLL API failure after Sleep begins still needs lifecycle-specific validation; this test does not establish the proprietary SDK's complete failure-state behavior.

## Apply and test

First reconstruct the exact v25 sources (v23 baseline -> apply-v24.py -> apply-v25.py). Then:

```sh
git -C upstream apply --check ../v25-sequence/sequence-only.patch
git -C upstream apply ../v25-sequence/sequence-only.patch
g++ -std=c++17 -Wall -Wextra -Werror -Iupstream/OptiScaler/framegen v25-sequence/sequence-regression.cpp -o sequence-regression
./sequence-regression
```

Local validation used the actual v25 production protocol header and the final v25 source-review archive from commit 11839e091686bd6a6118330096bd6f91493c1742. Patch application and the portable regression passed. No Windows DLL build, Intel DLL execution, AMD GPU test, or pixel/flicker test was performed for this sequence-only change.

## Acceptance remains open

The sequence defect explains reactivation errors, not the entire sustained ~30–35 source FPS regression. The continuous capture has long intervals with successful interpolation but low throughput. Likewise, ETW display timestamps do not contain pixel values and cannot establish the cause of persistent brightness or image flicker.

Immediate containment: exit the game and set ManagedPacing=false and AsyncRenderExport=false, retaining FTInput=0; alternatively restore the exact backed-up v24 DLL. v24 remains an earlier, faster experimental baseline with known uneven frame presentation, not a completed smooth 67->134 solution. If flicker persists, disable frame generation rather than treating this patch as a verified visual fix.
