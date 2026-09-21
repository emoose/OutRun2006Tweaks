# OutRun 2006 OpenXR VR

`vr-openxr` is the experimental OpenXR branch for OutRun 2006: Coast 2 Coast. It is layered on the wheel/multi-device/native-FFB fork and must not replay or alter those game-tick paths.

For design decisions and source ownership, read `docs/VR_ARCHITECTURE.md`. For external reference analysis, read `docs/VR_REFERENCE_HARVEST.md`.

## Status

The branch contains a true-geometry stereo prototype with:

- renderer-side head tracking at the verified OutRun VS c64..c67 WorldViewProjection upload;
- one pose latched for a complete presented game frame;
- per-eye asymmetric OpenXR FOV and eye transforms;
- duplicated D3D9 draw submission without replaying simulation/input/timers/FFB;
- zero-disparity duplication for non-world/UI draws;
- a conditional D3D9Ex -> D3D11 shared-eye transport with adapter/probe validation and a four-slot producer queue;
- an SBS/Desktop Duplication compatibility fallback;
- an x64 D3D11 OpenXR host using `XR_KHR_D3D11_enable`;
- gameplay projection layers and a LOCAL-space theater quad for non-gameplay screens;
- render-frame/pose matching and fail-closed stereo validation.

Windows CI validates compilation and structural invariants. A Quest 3 / VDXR run is still required to validate optics, eye order, culling, frame pacing and actual transport selection.

## Source layout

```text
src/vr/settings.cpp
src/vr/game/outrun_renderer.cpp
src/vr/d3d9/stereo_renderer.cpp
src/vr/ipc/protocol.hpp
src/vr_shared.hpp                 compatibility include only

vrhost/src/main.cpp
vrhost/src/stereo_shader.hpp
vrhost/tests/stereo_shader_smoke.cpp
```

The current D3D9 backend and x64 host are still large translation units. They were first moved intact so the architecture could be cleaned without changing behavior. They will be decomposed behind the boundaries in `docs/VR_ARCHITECTURE.md`.

## Renderer invariants

The game-specific renderer adapter verifies the live OutRun transform before injecting VR:

```text
0x0095DB20  WorldView = World * View
0x0095D860  View
0x0095D8A0  Projection
VS c64..c67 = Transpose(WorldView * Projection)
```

The VR path does not call game simulation, event processing, input processing or native FFB for the second eye. The invariant is:

```text
simulation / input / timers / FFB = once
left/right GPU draw submission     = twice where classified safe
```

Unsafe or unclassified stereo work fails closed rather than replaying the whole renderer.

## Transport selection

The renderer is intentionally independent of transport choice.

### Direct shared transport

Direct transport is eligible only when all of the following pass at runtime:

1. OpenXR reports the D3D11 adapter LUID required for the session.
2. The game device proves it is compatible with D3D9Ex interop.
3. D3D9Ex and OpenXR/D3D11 adapter LUIDs match.
4. A shared verification texture is opened and read correctly by D3D11.
5. The host acknowledges that exact probe generation.
6. The four-slot eye-resource queue is created successfully.

If the stock OutRun device is classic D3D9 rather than D3D9Ex, this path is expected to reject itself. D3D9-to-9Ex conversion is treated as a separate compatibility experiment, not as an assumption inside the renderer.

### Desktop Duplication fallback

The fallback composes the two already-rendered eyes into the game Present and captures them on the x64 host. Frame metadata is checked before and after capture to avoid pairing a new pose with an older desktop image.

### DXVK experiment

`tools/vr_dxvk_poc.ps1` remains isolated. DXVK is not promoted to the primary renderer unless Quest/VDXR measurements show a clear advantage and compatibility is acceptable.

## Build

### Win32 game DLL

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

### x64 OpenXR host

```powershell
cmake -S vrhost -B build-vrhost -G "Visual Studio 17 2022" -A x64
cmake --build build-vrhost --config Release
```

The host currently pins Khronos OpenXR-SDK `release-1.1.63`. CI also builds and runs the exact compositor HLSL smoke test.

## First Quest 3 / VDXR acceptance run

Start with positional tracking disabled and union-FOV culling disabled. The first run should answer transport and correctness questions before comfort tuning.

1. Confirm VDXR is the active OpenXR runtime and the x64 host creates a session.
2. Enter gameplay and confirm yaw/pitch/roll move the world in the correct direction.
3. Press F10 while facing forward and confirm yaw recenter works.
4. Verify true depth: near traffic/cockpit elements must have more disparity than the horizon.
5. Verify left/right are not swapped.
6. Verify HUD/text is stable and zero-disparity.
7. Pause/menu and confirm theater presentation rather than invalid gameplay stereo.
8. Turn the head roughly +/-90 degrees and note missing buildings, traffic, roadside objects, billboards or flares.
9. Verify wheel, pedals, multi-device mappings and native FFB are unchanged.
10. Stop/resume the OpenXR session and verify no stale stereo frame flashes.
11. Record whether direct transport was accepted or the Desktop Duplication fallback was selected.
12. Record host timing p95/p99 values and subjective latency.

## Useful log milestones

Game side:

```text
VR: renderer-boundary head tracking + true stereo configured
VR renderer inject: verified OutRun c64 = Transpose(WorldView*Proj)
VR renderer inject: HEAD TRACKING ACTIVE
VR stereo: TRUE GEOMETRY STEREO active
```

Direct transport, when available, should additionally report successful interop verification and direct ring use. If D3D9Ex/LUID/probe validation fails, fallback selection is expected and should be logged as such.

Host side:

```text
OpenXR runtime: ...
VR presentation: true stereo projection.
VR host timing ms p95/p99: ...
```

## What CI does not prove

CI does not prove:

- the stock game device is D3D9Ex-capable;
- Quest eye order or optical comfort;
- pre-BeginScene culling correctness;
- stage-specific offscreen world passes;
- Desktop Duplication latency;
- VDXR reprojection behavior;
- subjective FFB feel.

Those require the real game, Quest 3 and VDXR.
