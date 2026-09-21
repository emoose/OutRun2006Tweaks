# VR reference harvest

This document records the external patterns used to design the `vr-openxr` branch. It is a design/reference ledger, not a source-copy ledger.

The primary architecture is documented in `docs/VR_ARCHITECTURE.md`.

## Evidence hierarchy

Reference material is used in this order:

1. Microsoft / Khronos API contracts for graphics interop and OpenXR behavior.
2. Mature open-source VR injectors for architecture and failure-mode patterns.
3. D3D9 VR projects for legacy-renderer techniques.
4. Old wrappers/injectors only as historical hints that must be re-verified.

No behavior is accepted merely because another mod used it.

## References retained

| Reference | Lesson retained | OutRun use |
| --- | --- | --- |
| Microsoft D3D9Ex / DXGI surface-sharing documentation | D3D9Ex-to-DXGI sharing is an unsynchronized producer/consumer problem; explicit synchronization and a queue of surfaces are required | LUID/probe gate + four-slot producer queue + host ACK |
| Microsoft `ID3D11Device::OpenSharedResource` documentation | D3D9/D3D11 shared textures have strict format/resource restrictions | direct transport validates format/resource creation and fails closed |
| Khronos OpenXR | runtime-selected graphics adapter, predicted display time, view pose/FOV, session/reference-space lifecycle | x64 host and render-pose/frame matching |
| REFramework | separate generic VR/runtime infrastructure from game-specific engine work | OutRun adapter separated from runtime/transport layers |
| UEVR | isolate D3D backends, runtime components and overlay/submission concerns | architecture target for further decomposition |
| `elliotttate/vrframework` | explicit universal-core / engine-adapter / per-game-data layering and frame-timing discipline | used as structural guidance, not copied runtime code |
| openRBRVR / `dxvk-openRBRVR` | a DXVK-based D3D9 VR path is viable, but it makes the graphics translation layer part of the mod | DXVK remains an isolated measured experiment |
| ReShade / D3D wrappers | robust device/reset/present lifecycle interception patterns | compared against D3D9 hook lifecycle |
| iZ3D / historical stereo wrappers | final-draw duplication, stereo projection and zero-disparity UI concepts | historical reference only; no incompatible source imported |

## Important conclusion: D3D9Ex is a backend, not an assumption

Microsoft's graphics-API surface-sharing guidance distinguishes D3D9Ex from classic D3D9c: shared-surface interoperability with DXGI-based APIs is a D3D9Ex path, while classic D3D9c/older paths require copying.

That means the OutRun renderer must not be designed around the assumption that the stock 2006 device is already `IDirect3DDevice9Ex`.

The current direct path therefore remains conditional:

```text
OpenXR-required D3D11 adapter LUID
             |
             v
compatible D3D9Ex device exists
             |
             v
D3D9Ex adapter LUID == OpenXR adapter LUID
             |
             v
shared verification texture opens and reads correctly
             |
             v
host ACKs exact probe generation
             |
             v
four-slot direct eye transport enabled
```

If any step fails, backend selection falls back instead of weakening validation.

Converting an old D3D9 game wholesale to D3D9Ex is a separate compatibility project. Existing open-source wrappers show that this can work for some titles but can fail on legacy resource formats/pool behavior. It must therefore be tested on OutRun rather than silently built into the renderer core.

## What is kept from the existing OutRun work

These are based on game-specific evidence and are more valuable than the prototype file layout:

- verified renderer globals for View / Projection / WorldView;
- VS c64..c67 as the authoritative `Transpose(WorldView * Projection)` upload boundary;
- one immutable pose per presented game frame;
- no replay of simulation, input, timers or native FFB;
- second-eye duplication at the D3D draw boundary while renderer state is live;
- fail-closed classification for unsafe MRT/depth/offscreen/query cases;
- frame ID + pose sequence + full QPC association;
- submit the pose/FOV that rendered the accepted texture;
- reset/session changes invalidate stale history/resources.

## What is discarded

The following are development history, not architecture:

- `main.cpp`, `main_compat.cpp`, `main_compat_v2.cpp`, `main_compat_v3.cpp` host generations;
- one-shot source mutation workflows;
- one-shot Python patch scripts;
- old root-level VR implementation filenames;
- the idea that Desktop Duplication should define the renderer design;
- the idea that a successful `QueryInterface(IDirect3DDevice9Ex)` can be assumed before runtime proof.

## Current source map

```text
src/vr/settings.cpp
    VR settings/bootstrap

src/vr/game/outrun_renderer.cpp
    OutRun-specific renderer facts, c64 verification, frame pose latch,
    recenter and render-only camera synchronization

src/vr/d3d9/stereo_renderer.cpp
    D3D9 state tracking, draw classification, stereo draw duplication,
    current native shared transport and SBS fallback implementation

src/vr/ipc/protocol.hpp
    current cross-bitness Pose.v2 / Frame.v2 wire contract

vrhost/src/main.cpp
    current x64 host implementation

vrhost/src/stereo_shader.hpp
    compositor shader

vrhost/tests/stereo_shader_smoke.cpp
    exact shader compile smoke test
```

The two large runtime translation units are intentionally moved before being split. Moving first keeps behavior identical and gives CI a clean checkpoint. Further decomposition should be behavior-preserving and one boundary at a time.

## Next extraction order

Game side:

1. shared-memory pose client;
2. stereo projection/math;
3. draw classifier/state tracker;
4. D3D9Ex direct transport backend;
5. SBS fallback backend;
6. diagnostics.

Host side:

1. OpenXR runtime/session owner;
2. D3D11 device/adapter owner;
3. IPC bridge;
4. direct shared-frame source;
5. Desktop Duplication frame source;
6. compositor/theater layer;
7. timing/config.

Only after this extraction and a Quest/VDXR smoke run should the wire protocol move to a cleaner ownership-separated v3.

## License boundary

The policy differs by branch:

- `vr-openxr` keeps the previous no-GPL-source-copy boundary unless separately changed.
- `vr-openxr-gpl-reuse` may import GPL/LGPL source when the imported file is clearly identified, its upstream attribution is preserved, the exact license is recorded, and the combined branch is distributed under compatible GPL terms.
- Existing MIT-covered OutRun2006Tweaks material retains its original MIT notice and permissions.

### Imported GPL source ledger

- **3Dmigoto / GPLv3:** `src/vr/d3d9/shader_fingerprint_gpl.hpp` adapts the D3D9 shader-bytecode fingerprint approach and FNV-1 64-bit buffer hash from 3Dmigoto's `DirectX9/Direct3DDevice9Functions.h` and `util.h`. It is used only for opt-in diagnostics and does not alter draw output.
- Full GPLv3 terms are stored in `COPYING.GPL3`.

Further GPL/LGPL imports must be added to this ledger before release.
