# VR Review Findings

## PKG-001 — Duplicate selector entry point
Status: FIXED

The unified test package exposed both `Select-OutRunVRBackend.cmd` and `OutRunVR-Backend-Selector.cmd`. The GUI selector is the intended tester entry point; the PowerShell backend engine remains internal. The duplicate CLI CMD wrapper is no longer copied to the tester ZIP.

## PKG-002 — Release/compliance files clutter tester ZIP
Status: FIXED IN WORKFLOW / CI VALIDATION PENDING

License/notice/corresponding-source files are no longer copied into the runtime tester ZIP. CI now creates a separate compliance ZIP so release obligations can be retained without mixing them into the user's test folder.

## PKG-003 — Lean package contract
Status: STATICALLY VALIDATED

The workflow now fails if the tester package contains the duplicate selector wrapper or license/notice/source-only files, and it verifies the required selector, launcher, collector, configuration, build identity, and test-scenario files.


## VR-TRANSPORT-001 — SAFE mode ignored DirectGpuOnly=false
Status: FIXED IN FROZEN MATRIX `VRM-20260920-803144005c0e`

The game-side R9 transport policy read `OUTRUN_VR_DIRECT_ONLY` through the CRT environment cache. The launcher/host could log `DirectGpuOnly=false` while R9 still behaved as direct-only, suppressing SBS/Desktop Duplication fallback and leaving the host without a completed stereo frame. R9 now follows the parsed setting directly. The same fix is synchronized to the DXVK game branch.

## VR-HOST-001 — Recovery theater cropped mono as SBS
Status: FIXED IN FROZEN MATRIX `VRM-20260920-803144005c0e`

The gameplay recovery path always rendered the left half of the desktop source. When stereo never became active, the source was a normal full-width mono frame, so this produced the observed enlarged left-half image. The host now crops only when shared frame state explicitly reports `StereoSbsActive` and `RenderFrameStereoComplete`; otherwise it uses full-width mono.

## VR-DX12-001 — D3D9On12 CreateDeviceEx invalid-call boundary
Status: FIXED FOR RETEST / RUNTIME VALIDATION PENDING

The failing DX12 log reached `Direct3DCreate9On12Ex` successfully, then device creation failed with `D3DERR_INVALIDCALL`. The strict branch now preserves the legacy D3D9 `CreateDevice` contract on the D3D9On12 provider and performs one bounded presentation-parameter normalization retry. Native D3D9 fallback remains disabled.

## CI-LEGACY-001 — Generic branch workflows contain stale checks
Status: NON-BLOCKING FOR UNIFIED TESTER

The DXVK generic R34/OFF configuration still compiles an older incompatible renderer combination and its host verification script assigns PowerShell's reserved `$Host` variable. The DX12 generic verifier also searches for a pre-change strict-pass marker. These failures do not affect the accepted unified workflow `35514339728`, whose intended D3D9 SAFE, DXVK SAFE host/game, DX12 strict, provider, multiview, and package jobs all passed.


## VR-BASE-002 — T0-debug is a reference input, not a merge source
Status: ACTIVE / REVIEWED IN RB01

`vr-d3d9ex-t0-debug` and `vr-unified-backends` have materially diverged source histories. Replacing unified Host/renderer files wholesale with T0-debug files would discard newer R42/R23 transport, cache, and integration work. The adopted build-validated DX9Ex reference input is T0-debug commit `365b294b6a96c98ada434b439a982725e1ba542c`; future work must port only bounded behaviors into unified and keep exact source evidence.

## VR-HOST-002 — Gameplay loading and stall projection guards
Status: BUILD/MATRIX VALIDATED AT `ad14ed6c6757634233ec79b71f42be53c563edb7`; RUNTIME RETEST PENDING

T0-family runtime tests showed two useful Host behaviors: stage loading no longer entered a black direct-only interval, and a gameplay frame stall no longer degraded into a full-screen solid/empty color. The current unified Host now defers the Gameplay projection transition until a render frame exists, keeps the last menu/loading projection available during bootstrap, and reuses the last successfully released stereo projection for the duration of a Gameplay stall. Existing presentation/session/reference-space transitions still invalidate the cache. Build run `35527017219` and unified run `35527017201` passed.

## VR-FINGERPRINT-001 — Position HUD needs exact draw fingerprinting
Status: READY FOR NEXT BOUNDED PORT

Primitive-count experiments through 1..1024 and broad WVP ownership experiments did not resize or correctly place the Position `6th/6` HUD. Broad ownership caused severe world/sky/driver corruption and draw amplification. T0-debug commit `365b294...` contains bounded shader/pixel-shader/vertex-declaration/stride/texture/render-state fingerprint telemetry and passed CI, but that telemetry has not yet been ported into unified. The next diagnostic must remain passive/rate-limited until an exact fingerprint is established.

## VR-LENS-002 — Narrow S6/S7 flare behavior is useful but not final
Status: RUNTIME OBSERVED / CORRECTNESS UNCONFIRMED

Small12 S6/S7-family tests collapsed the duplicated lens flare to one while avoiding the broad world corruption seen in ownership experiments. The useful discriminator was a small alpha candidate with a live WVP that classified as flat. The flare still appeared to react to head/camera direction, which may be physically correct for a camera-facing flare but has not been confirmed. Preserve this as an isolated fail-closed candidate; do not generalize it to sky, HUD, smoke, skid, or world geometry.

## VR-PERF-002 — T5 mono-backup removal is performance evidence only
Status: PERFORMANCE SIGNAL / CORRECTNESS BLOCKED

The T5/R29 LEFT+RIGHT-only comparison removed the large steady-state mono safety replay burden and subjectively improved frame rate, but it also made sky/world rendering incorrect. Do not adopt the T5 classifier or fast path wholesale. Reuse only the mono-removal concept behind a strict correctness gate after the stable T0/unified world classification is preserved.

## VR-HUD-SEMANTICS-001 — UIScaling-derived HUD semantic baseline
Status: BASELINE / SOURCE-INTEGRATED

The existing 4:3/16:9 UI reverse engineering is now the VR semantic
source-of-truth. Time Attack, Rank, REV/gear, Ghost/You/Diff, goal time,
Heart totals, Rival HUD, girlfriend speech, ranking emoji/text and related C2C
HUD are explicitly classified as SCREEN_HUD. Rival-car rank markers
(`sub_4BAD20`) and attached car/world hearts are explicitly protected as
WORLD_BILLBOARD. HUD trace schema v2, static EXE analysis and automatic log
coverage all use the same semantic names. See `docs/VR_HUD_SEMANTIC_BASELINE.md`.

