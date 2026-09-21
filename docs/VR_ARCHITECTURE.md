# OpenXR VR architecture

This document is the source-of-truth architecture for the `vr-openxr` branch.

## Decision

The VR software skeleton is being **reconstructed**, not merely cleaned up. The current prototype proved important game-specific facts, but its large mixed-ownership translation units and compatibility-oriented IPC are not the architecture we want to keep.

Two classes of work are treated differently:

**Retain verified evidence:**

- OutRun's verified View / Projection / WorldView globals;
- VS c64..c67 as the authoritative `Transpose(WorldView * Projection)` upload boundary;
- one immutable render pose for one presented game frame;
- simulation, input, timers and native FFB execute once;
- only GPU draw submission is duplicated for the second eye;
- unsafe/unclassified rendering fails closed;
- the submitted OpenXR pose/FOV must be the pose/FOV that rendered the accepted image;
- reset/session generations invalidate stale resources and frame history.

**Replace prototype structure:**

- broad shared state with mixed host/client writers;
- semantic use of `reserved[]` fields;
- transport details embedded in the game renderer contract;
- runtime/session, D3D11, frame-source and compositor ownership concentrated in `vrhost/src/main.cpp`;
- D3D9 draw classification, stereo projection, transport and diagnostics concentrated in one backend TU;
- one-shot mutation workflows and obsolete host generations.

A zero-knowledge rewrite would throw away the expensive reverse engineering above. Keeping the old skeleton would preserve accidental coupling. The chosen approach is therefore a **clean skeleton reconstruction around the verified OutRun adapter**.

## Target architecture

```text
                         OpenXR runtime
                              |
                              v
                    +-------------------+
                    |   IVrRuntime      |
                    | frame timing      |
                    | views / lifecycle |
                    +---------+---------+
                              |
                    HostState | render pose
                              v
+------------------ Cross-bitness IPC v3 ------------------+
| HostState  : host -> game                                |
| ClientState: game -> host                                |
| FrameRing  : game producer -> host consumer              |
| AckState   : host consumer -> game producer              |
+-------------------------+---------------------------------+
                          |
             +------------+-------------+
             |                          |
             v                          v
 +---------------------+      +-------------------------+
 | OutRun IGameAdapter |      | Host IFrameSource       |
 | c64 WVP boundary    |      | D3D9Ex shared backend  |
 | frame pose latch    |      | Desktop Dup fallback   |
 | recenter/culling    |      | optional DXVK backend  |
 +----------+----------+      +------------+------------+
            |                              |
            v                              v
 +---------------------+          +---------------------+
 | IStereoBackend      |          | compositor/layers   |
 | classify draws      |          | projection/theater  |
 | per-eye projection  |          | HDR/UI/depth later  |
 | duplicate GPU draw  |          +----------+----------+
 +----------+----------+                     |
            |                                v
            +---------------------------> Quest / VDXR
```

The important property is ownership. OpenXR does not know OutRun internals; the game adapter does not know how images reach the host; transport backends do not define camera mathematics; the compositor does not choose game draw classification.

## New core boundaries

The reconstructed skeleton starts with these interfaces/types:

```text
src/vr/core/frame_types.hpp
    renderer-independent pose, eye, frame and failure domain types

src/vr/core/transport.hpp
    AdapterId, TransportKind, FrameSlot
    IFrameProducer / IFrameConsumer

src/vr/game/game_adapter.hpp
    IGameAdapter
    the only architectural layer allowed to encode OutRun camera/WVP facts

src/vr/d3d9/stereo_backend.hpp
    IStereoBackend
    draw classification and stereo GPU submission boundary

src/vr/ipc/protocol_v3.hpp
    HostState / ClientState / FrameRing / AckState
    fixed-width cross-bitness wire fields, no semantic reserved slots

vrhost/src/runtime/vr_runtime.hpp
    IVrRuntime
    OpenXR timing/view/session ownership boundary

vrhost/src/frame_source.hpp
    IFrameSource
    host-side image acquisition boundary
```

Protocol v3 is compiled on both Win32 and x64 immediately, even while protocol v2 remains the active compatibility runtime during migration. This catches ABI mistakes before the runtime is switched.

## Protocol v3 ownership

Protocol v2 proved the transport but has acknowledged design debt: the host and game write different regions of broad shared structures and some meanings are encoded through compatibility/reserved storage. Protocol v3 removes that ambiguity.

```text
HostState
  only host writes
  game reads
  pose ID, predicted display time, head/eye poses, eye FOV,
  recommended resolution, runtime adapter and generation

ClientState
  only game writes
  host reads
  game state, presentation state, adapter/probe state,
  backbuffer information and last rendered/presented IDs

FrameRing
  only game publishes slots
  host consumes
  fixed-width frame ID, render-pose ID, presentation QPC,
  rendered eye poses/FOV and explicit transport descriptor

AckState
  only host writes
  game reads
  accepted probe generation/token and consumed frame/slot
```

Native handles use a fixed 64-bit `WireHandle`; pointer-sized fields are forbidden in the v3 wire ABI. This is required because the producer is x86 and the consumer is x64.

## Migration rule

The reconstruction is deliberately incremental at the **behavior boundary**, not incremental at the old file layout.

1. Build the new domain types, ownership-separated v3 wire contract and interfaces on both architectures.
2. Extract host pose publication and game pose consumption behind v3 HostState.
3. Extract OutRun stereo projection math from the D3D9 hook into the game adapter.
4. Extract draw classifier/state tracker behind IStereoBackend.
5. Move D3D9Ex sharing into a transport backend implementing the core producer contract.
6. Move Desktop Duplication into a host frame-source backend.
7. Split OpenXR runtime/session ownership from D3D11 resources and composition.
8. Switch the live runtime from v2 to v3 once both sides can run in parallel and produce equivalent frame IDs/poses.
9. Delete v2 compatibility protocol and the old mixed-ownership paths.
10. Only then add new features such as depth submission, independent HUD layers or more aggressive culling fixes.

At each extraction the old behavior is the comparison oracle. Once equivalent behavior is demonstrated, the old implementation is removed instead of kept as a permanent compatibility branch.

## OutRun-specific invariants

These are evidence, not legacy compatibility requirements, and they remain until runtime evidence disproves them:

- the authoritative world transform boundary is the verified VS c64..c67 upload;
- additional `BeginScene` calls before the same Present reuse the same latched render pose;
- simulation/input/timers/native FFB are never replayed for the second eye;
- second-eye work happens while the original D3D9 draw state is live;
- screen-space/non-world draws are zero-disparity unless deliberately promoted to an OpenXR UI layer;
- unknown MRT, depth/stencil, offscreen-world or query states fail closed rather than guessing;
- the frame descriptor carries the effective rendered eye poses, not a newer tracking sample;
- stale resources never survive reset, adapter, transport-generation or OpenXR session-generation changes.

## Transport backends

Transport is not part of the game adapter.

### D3D9Ex shared backend

Direct sharing is eligible only after all of the following succeed:

1. OpenXR selects/requires the D3D11 adapter;
2. the game proves a compatible D3D9Ex path;
3. adapter LUIDs match;
4. a small shared verification texture is opened by D3D11 and its content is verified;
5. the host ACKs the exact probe generation/token;
6. a multi-slot queue is created;
7. producer completion and consumer ACK prevent slot overwrite.

D3D9Ex failure is a backend-selection result, not a camera/stereo failure.

### Desktop Duplication backend

Desktop Duplication is retained as a compatibility and diagnostic frame source. Metadata must be read before capture and revalidated after capture so the host never associates an image with the wrong frame/pose. It must not shape the stereo renderer architecture.

### DXVK backend

DXVK remains isolated. It is useful as a measured alternative for old D3D9 titles, but adopting it as the primary path would make a graphics translation layer part of this project's maintenance surface. Promotion requires Quest/VDXR measurements for latency, pacing, image quality and compatibility.

## Reference projects and what is actually reused

The design uses observable architecture lessons, not blind source copying.

- Microsoft/Khronos documentation defines interop, adapter, timing and OpenXR lifecycle contracts.
- REFramework/UEVR demonstrate the value of separating runtime ownership, graphics backends, engine/game adapters and overlay/composition concerns.
- UEVR in particular contains separate OpenXR runtime, D3D11/D3D12 components and overlay/render-target components; the lesson is the ownership boundary, not its Unreal-specific implementation.
- `elliotttate/vrframework` reinforces universal-core / engine-adapter / game-data separation and explicit frame timing.
- openRBRVR and DXVK-based work demonstrate that a D3D9 translation route can be viable, while also showing that the translation layer becomes part of the product.
- historical iZ3D/stereo wrappers are used only as hints for draw duplication, projection and UI classification and must be revalidated against OutRun.

License-incompatible or unclear-license implementations are not copied. UEVR is currently treated as architecture/reference material only.

## Current transitional state

The live runtime still uses the proven v2 paths inside:

```text
src/vr/game/outrun_renderer.cpp
src/vr/d3d9/stereo_renderer.cpp
src/vr/ipc/protocol.hpp
vrhost/src/main.cpp
```

They are now migration sources, not architecture authorities. New code must target the reconstructed interfaces/v3 contract. No new feature should deepen dependencies on those monoliths.

`src/vr_shared.hpp` is compatibility-only and should disappear with v2.

## CI contract

CI must prove throughout reconstruction that:

- Win32 game DLL builds;
- x64 host builds;
- protocol v3 compiles on both architectures and uses fixed-width wire handles;
- exact compositor HLSL compiles;
- the verified OutRun c64/BeginScene/Present evidence remains present until moved into the game adapter;
- VR code never calls game simulation or native FFB ticks for stereo rendering;
- v3 contains explicit HostState/ClientState/FrameRing/AckState ownership and no semantic `reserved[]` transport extension;
- transport choices remain behind explicit boundaries;
- obsolete patch workflows and host generations do not return.

CI cannot prove optics, eye order, reprojection quality, stage-specific culling completeness, real D3D9Ex support or end-to-end latency. Those remain Quest 3 / VDXR acceptance tests.
