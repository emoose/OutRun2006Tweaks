# Emulator rendering review for OutRun VR

This note records the September 2026 comparison of PPSSPP, Dolphin, PCSX2 and RPCS3 rendering patterns against the `vr-openxr` renderer. It is a design/behavior review, not a source-copy ledger. GPL implementation code from those projects is not imported into this fork.

## Goal

Use mature emulator renderer lessons only where they reduce ambiguity in OutRun's D3D9 true-stereo path without changing game simulation, input, wheel/FFB behavior, or the proven R9/R13 transport model.

## PPSSPP

Observed pattern:

- VR distinguishes flat/screen-space geometry from 3D geometry before applying headset transforms.
- Projection structure participates in the decision; orthographic/identity-like objects remain flat.
- Render-target/framebuffer identity is explicit and persists independently from individual draw calls.
- Compatibility work is fail-safe: unusual sky/effect passes can stay outside the normal stereo path rather than being blindly replayed.

OutRun decision:

- **Adopt:** projection-semantic classification before c64 HMD injection.
- **Already equivalent:** real backbuffer identity versus auxiliary targets; unsafe passes already fail closed.
- **Reject for now:** PPSSPP-style frame-wide `3D geometry count` threshold. OutRun has a verified c64 `WorldView*Projection` boundary, so delaying stereo until an arbitrary draw count would add latency and hide classification bugs.

## Dolphin

Observed pattern:

- Perspective and orthographic projection are explicit renderer states.
- Camera/free-look transforms are applied to perspective projection paths, not indiscriminately to orthographic UI.
- Projection/viewport/depth changes are tracked as state transitions instead of inferred from a single draw.

OutRun decision:

- **Adopt:** structural perspective-versus-orthographic classification using the stable D3D projection terms (`m34`, `m44`).
- **Adopt:** unknown projection signatures fail closed to the stock game WVP.
- **Already equivalent:** projection is read from OutRun's verified renderer globals and the c64 upload must match `Transpose(WorldView * Projection)` before a draw can seed stereo replay.

## PCSX2

Observed pattern:

- Special rendering behavior is selected only after multiple state signals agree: render target, source texture, depth target, dimensions/formats/masks and draw geometry are commonly combined.
- Render-target identity is not by itself proof of semantic meaning.

OutRun decision:

- **Adopt:** world classification now requires multiple independent signals to agree:
  1. game D3D9 device;
  2. real main backbuffer with no auxiliary MRT;
  3. perspective projection signature;
  4. authoritative c64 upload matching `Transpose(WorldView * Projection)`;
  5. matching shader identity/serial when stereo replay consumes the verified WVP.
- **Reject for now:** game-specific draw-size/texture heuristics. They would be brittle until runtime evidence identifies a concrete OutRun effect that needs them.

## RPCS3

Observed pattern:

- Frame contexts have explicit ownership and cleanup boundaries.
- Presentation success/failure, queued frame advancement and resource retirement are kept coherent rather than allowing an ambiguous half-owned frame to leak into the next one.

OutRun decision:

- **No duplicate implementation needed:** the current renderer already latches one immutable pose for a presented game frame, reuses it across additional `BeginScene` calls, publishes frame/pose association at Present, and releases the pose lock only in `NotifyGamePresent()` or reset.
- Keep this invariant and continue to reject pose-sequence mismatches rather than attempting late repair.

## Applied R13 semantic policy

The central `src/vr/d3d9/vr_pass_policy.hpp` now classifies two independent dimensions:

1. **Target policy** — internal stereo, main backbuffer, or auxiliary/stock.
2. **Projection class** — perspective 3D, orthographic 2D, or unknown.

Those dimensions produce a render semantic:

- `World3D`: main backbuffer + perspective projection. This is the only semantic allowed to proceed to the existing authoritative c64 verification and HMD injection.
- `ScreenSpace2D`: main backbuffer + orthographic projection. It stays stock/zero-disparity.
- `Auxiliary`: reflection/shadow/offscreen/MRT-related target. It stays stock.
- `Unknown`: unrecognized main-backbuffer projection. It stays stock fail-closed until runtime evidence proves it safe.
- `InternalStereo`: the mod's own replay/composition work; never recursively reclassified as game world geometry.

For the stock OutRun renderer, `D3DXMatrixPerspectiveFovRH` produces the stable perspective structural signature `|m34| ~= 1, m44 ~= 0`; orthographic D3D projection produces `m34 ~= 0, |m44| ~= 1`. FOV, aspect, near/far and eye asymmetry do not change those two structural terms, so the classifier does not depend on resolution or user FOV.

## Why this is low-risk

The new policy does not create a new stereo renderer. It only narrows eligibility before the already-proven R13/R9 path:

- perspective world draw -> existing c64 verification -> existing per-eye replay;
- orthographic/UI draw -> original game matrix;
- auxiliary/offscreen draw -> original game matrix;
- unknown -> original game matrix;
- MRT/query hazard -> existing single-execution/mono-shadow fallback.

Therefore a classification miss degrades to stock mono/zero-disparity rendering for that draw instead of corrupting the world transform or duplicating side effects.

## Runtime validation targets

On Quest 3 + VirtualDesktopXR/VDXR, a healthy run should show:

- `VR R13 emulator policy: main-backbuffer perspective class confirmed...`
- optionally `orthographic/screen-space c64 WVP kept stock...` when HUD/UI uses c64;
- no repeating `unknown main-backbuffer projection` warning during normal driving;
- true stereo still reaches `presentation=1`, `stereoState=2`, `stereoFailure=0`;
- `composeFail=0` and pose mismatch failures remain zero;
- compare `depthState`, `poseLag`, and `hostMissing` against the previous runtime baseline.

If normal world geometry logs as unknown, do not loosen the classifier blindly. Capture `_34/_44`, target/depth state and c64 verification together and add only an evidence-based signature.
