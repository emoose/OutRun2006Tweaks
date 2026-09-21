# VR Run State

Updated: 2026-09-21 02:55 KST

## Current checkpoint
- C0 RECOVER: complete — 2D and D3D9 SAFE startup regression cleared on the previous recovery build
- C1 REVIEW: complete — runtime logs isolated the flat half-screen symptom to transport/fallback policy rather than common game startup
- C2 IMPLEMENT: complete — SAFE transport policy, host fallback crop guard, and DX12 D3D9On12 device-creation semantics updated
- C3 VALIDATE: complete — all unified backend jobs and package validation succeeded
- C4 COMMIT: complete
- C5 PACKAGE: complete — frozen tester ZIP produced
- C6 STATE: complete

## Frozen recovery candidate
- Integration commit: `803144005c0e45352602917a2f51804ec6b18ae6`
- Matrix: `VRM-20260920-803144005c0e`
- Unified workflow: `35514339728` — success
- Build workflow: `35514339665` — success
- Unified artifact: `10606816032`
- Artifact ZIP SHA256: `1517890fd2efe90f9c5fe8f345d7fd695b534b33de5ad1937441836f386e6cca`
- Tester inner ZIP SHA256: `e48b1648ce80f7027e3cdea3bbb13192cc56c5bd99628ee886759e6d8916e065`

## Backend source identity
- D3D9 SAFE: `803144005c0e45352602917a2f51804ec6b18ae6`
- DXVK game + host: `5e58ee35e14636dbfad3ad3744d350aa09859992`
- DXVK provider: `5bb301ab22f7b41d68a879bf84c1e3d10ae0c473`
- Multiview patcher: `0ef3253da1738b07e67358a027311c7bcedaa001`
- DX12 strict: `ab8e00ab4a21e4ebb23fbc4046abda51e885bcb6`

## Runtime evidence that led to this candidate
The previous recovery package reached normal startup in 2D and D3D9 SAFE. D3D9 SAFE and DXVK SAFE then showed the same enlarged/cropped left-half flat image in gameplay, while DX12 STRICT failed device creation.

The logs and source review showed:
1. SAFE configuration had `DirectGpuOnly=false`, but the game R9 transport gate read stale CRT environment values and stayed in direct-only mode. That suppressed SBS/Desktop Duplication fallback even though no shared-eye transport was ready.
2. The host recovery theater always cropped the left half, even when the desktop source was a normal full-width mono frame. That exactly produced the observed half-screen zoom.
3. DX12 reached `Direct3DCreate9On12Ex`, then failed device creation with `D3DERR_INVALIDCALL`. The strict branch now uses legacy D3D9On12 `CreateDevice` semantics and a bounded compatibility retry without permitting native D3D9 fallback.

## Fixes frozen into this matrix
- D3D9 SAFE: `R9DirectOnlyTransport()` follows parsed `Settings::VRDirectGpuOnly` directly.
- DXVK SAFE/MULTIVIEW: same game-side transport fix synchronized to the DXVK branch.
- D3D11 OpenXR hosts: left-half crop is used only for a frame explicitly marked completed SBS; otherwise recovery shows full-width mono.
- DX12 STRICT: legacy `CreateDevice` contract first, then one bounded presentation normalization retry while staying on D3D9On12.
- `SkyGlowFactor = 1` remains packaged.

## Non-blocking branch CI notes
Two generic branch workflows still have stale checks unrelated to the unified tester result: the DXVK generic R34/OFF build exercises an older incompatible renderer combination, and its host verification script uses PowerShell's reserved `$Host` variable; the DX12 generic verifier searches for the old strict-pass marker string. The unified workflow builds and stages the intended SAFE/DX12 payloads successfully and is the acceptance path for this tester.

## Next runtime test
Use only the frozen `VRM-20260920-803144005c0e` tester. Test D3D9 SAFE first and look for actual geometry stereo rather than a flat recovery theater. If D3D9 is good, test DXVK SAFE. Then test DX12 STRICT only far enough to see whether device creation/startup now passes; deeper DX12 rendering work comes after that boundary is cleared.


## Manual hourly-equivalent run — 2026-09-21 02:55 KST

### Checkpoint result
- C0 RECOVER: complete — recovered `vr-unified-backends` HEAD `fc71a50a10ddcccb6e38e6a41fba8e3f7fc3d74f`, T0-debug build SHA `365b294b6a96c98ada434b439a982725e1ba542c`, and T0-debug branch HEAD `7298d1f2426da9134aca17b83f23076a495fd788`.
- C1 REVIEW: complete — RB01 bounded 5-lens review covered architecture/build boundaries, resource/state lifetime, stereo/HUD/effects correctness, performance, and adversarial regression risk.
- C2 IMPLEMENT: complete — ported only the T0 loading/bootstrap and gameplay stall projection guards into the current unified Host. HUD/effect/WVP classification was intentionally unchanged.
- C3 VALIDATE: complete — Build run `35527017219` succeeded. Unified run `35527017201` succeeded for D3D9 SAFE, DXVK game+host, DXVK provider, multiview patcher, DX12 strict, and final package.
- C4 COMMIT: complete — source commit `ad14ed6c6757634233ec79b71f42be53c563edb7`.
- C5 PACKAGE: complete — unified artifact `10609683851`; outer SHA256 `a5e21693ae74d51ea0ba7b2483ef133fbfde9517068993a5e8796f6b5bc37243`; tester ZIP SHA256 `12088212f8ec4af2f92cdc6280e785f51112b9e50945c10b8fe9fb4f1dd08780`; ZIP integrity passed.
- C6 STATE: complete — durable state updated with the T0-derived working base and next action.

### Adopted DX9Ex working reference input
- Branch: `vr-d3d9ex-t0-debug`
- Build-validated source commit: `365b294b6a96c98ada434b439a982725e1ba542c`
- Branch HEAD: `7298d1f2426da9134aca17b83f23076a495fd788` (later CI/generic-build policy only; runtime payload source remains `365b294...`)
- T0-debug CI: `35526213136` — host/game/package success
- Artifact: `10609747550`
- Artifact SHA256: `7cf2cc7a8ed762a07bcc3bb474e9d37bb0980ac0320b944a1999d61f23612a4f`
- Inner tester ZIP SHA256: `b034553ac0b919a9d7944ee11168a6e5dd3f868221a047585b3a8da7cfe8ed0d`

### Behavior contract carried forward
- Preserve T0-style stable world geometry and normal sky classification.
- Preserve normal stage-loading transition into gameplay.
- Preserve last-good stereo projection across gameplay frame stalls so HMD output does not become a solid/empty color.
- T0's duplicated lens flare is not accepted as final. The S6/S7 narrow alpha + flat-WVP observation remains a candidate because it collapsed the flare to one in runtime tests without the broad world corruption seen in ownership experiments.
- Position HUD `6th/6`, white rank/score, and exit `YES/NO` remain unresolved.
- T5/R29 LEFT+RIGHT-only behavior is performance evidence only: frame rate improved somewhat, but sky/world graphics regressed.

### RB01 review result
The T0-debug branch and unified branch are heavily diverged, so wholesale merge/file replacement is rejected. The current unified Host already contains newer R42/R23 transport and projection-cache work. Only validated T0 behavior should be ported in bounded changes. The first bounded port in this run was the loading/bootstrap and persistent projection-hold behavior.

### Next action
Continue from unified source commit `ad14ed6c6757634233ec79b71f42be53c563edb7`. Use T0-debug `365b294...` as the DX9Ex reference input. Next hourly run should port bounded fingerprint telemetry and the narrow S6/S7 lens-flare candidate into the D3D9Ex path only, with fail-closed matching and without broad WVP/WorldBillboard promotion. Prepare no more than six runtime candidates. Do not mark Position HUD, white overlays, or the fast path fixed until runtime evidence confirms them.


## Four-worker autonomous development environment — 2026-09-21 03:36 KST

The repository-side environment is now separated from external Work scheduling.

- Durable protocol: `docs/VR_AUTODEV_PROTOCOL.md`
- Central work queue: `docs/VR_WORK_QUEUE.json`
- Normalized hardware/runtime inbox: `docs/VR_RUNTIME_FEEDBACK.json`
- REVIEW handoff: `docs/autodev/REVIEW_HANDOFF.json`
- FIX handoff: `docs/autodev/FIX_HANDOFF.json`
- VALIDATION handoff: `docs/autodev/VALIDATION_HANDOFF.json`
- Coordination state is linked from `docs/VR_AUTODEV_STATE.json`.

Concurrency rule: FIX is the sole autonomous production-source writer. REVIEW and VALIDATION are source-read-only; INTEGRATION/PLANNER owns the central queue/state. This avoids four scheduled workers editing the same source/state concurrently while still allowing staggered hourly execution.

The queue is pre-seeded from the current durable findings. Highest-value automatic work remains bounded DX9Ex fingerprint telemetry, followed by the isolated S6/S7 lens-flare candidate. Position HUD, white overlays and DX12 startup remain runtime-evidence gated. T5/R29 performance work stays blocked behind correctness.
