# OutRun VR Autonomous Development Protocol

Purpose: durable coordination contract for the four Work-scheduled development jobs. Scheduling is external to this repository. This file defines what each worker may read/write and how work survives between runs.

## Authority and safety rules

- Integration/source branch: `vr-unified-backends`.
- Stable `master` and legacy stable branches are never modified by autonomous workers.
- Only the **FIX worker** may modify production source on `vr-unified-backends`.
- REVIEW and VALIDATION are source-read-only. They may update only their dedicated handoff/report files.
- INTEGRATION/PLANNER owns `docs/VR_WORK_QUEUE.json` and the coordination section of `docs/VR_AUTODEV_STATE.json`.
- Do not claim runtime/HMD correctness from CI. Quest 3 + VDXR observations are authoritative for stereo, depth, HUD placement, recenter, sky/effects and pacing.
- A finding that has failed two materially different fixes becomes `BLOCKED`; do not loop on it.
- If the relevant source/input hash has not changed, do not repeat an already completed review/build unless a new review lens or new runtime evidence exists.

## Checkpoint contract

Every source-changing unit follows:

C0 RECOVER -> C1 REVIEW -> C2 IMPLEMENT -> C3 VALIDATE -> C4 COMMIT -> C5 PACKAGE (only when useful) -> C6 STATE

A worker resumes from the first incomplete checkpoint. Completed checkpoints with unchanged inputs are reused.

## Worker roles

### REVIEW
Reads latest integration source, `VR_AUTODEV_STATE.json`, queue, findings and runtime feedback. Performs deep review using a rotating lens not already completed for the same input hash. It writes only:
- `docs/VR_REVIEW_FINDINGS.md` when there is material durable evidence
- `docs/autodev/REVIEW_HANDOFF.json`

A review result must include evidence paths/functions, severity, confidence, automatic-testability, HMD requirement and a bounded fix hypothesis. No source modification.

### FIX
Reads the queue and latest REVIEW handoff. It chooses the highest-priority `READY` item that does not require HMD evidence. It is the only autonomous worker allowed to change production source on `vr-unified-backends`.

Rules:
- one coherent hypothesis per commit;
- prefer DX9Ex/D3D9-safe correctness first; DXVK/DX12 receive bounded ports after the common behavior is stable;
- fail closed for render classifiers;
- do not broaden WVP/WorldBillboard ownership without exact evidence;
- write `docs/autodev/FIX_HANDOFF.json` with source commit, changed paths, hypothesis, expected effect and validation request;
- if two materially different attempts fail, mark the handoff as BLOCKED and move on.

### VALIDATION
Source-read-only. Reads FIX handoff and the exact source commit. It checks compile/static/regression/config/package/CI evidence and records:
- `docs/autodev/VALIDATION_HANDOFF.json`
- build/workflow IDs and artifact hashes when available.

It must distinguish:
- `PASS_STATIC`
- `PASS_BUILD_RUNTIME_PENDING`
- `FAIL_BUILD`
- `FAIL_REGRESSION`
- `NEED_HMD_TEST`

It never upgrades runtime-pending work to fixed.

### INTEGRATION / PLANNER
Consumes all handoffs and runtime feedback, deduplicates findings, updates the central queue/state, and decides which candidates are worth packaging. It does not rewrite renderer/game/host source.

It may:
- close DONE items only with required evidence;
- move hardware-dependent items to `NEED_HMD_TEST`;
- generate/refresh no more than six meaningful runtime candidates;
- freeze a tester matrix when it improves diagnostic value;
- prepare bisect ranges when a last-good and first-bad boundary exists;
- append durable run history.

## Central queue state machine

`NEW -> READY -> IMPLEMENTING -> IMPLEMENTED -> VALIDATING -> NEED_HMD_TEST -> DONE`

Alternative terminal/holding states:
- `BLOCKED`: two independent fixes failed or required evidence is unavailable.
- `REJECTED`: hypothesis disproved.
- `SUPERSEDED`: replaced by a newer, narrower finding.

Queue ownership is logical, not a long-lived lock. A worker must verify that the item source/evidence hash still matches before acting.

## Evidence priority

1. Runtime Quest 3/VDXR logs + user observation tied to BuildMatrixId/VariantId/SessionId
2. Reproducible local/CI build or regression result
3. Static source proof
4. Hypothesis only

Higher evidence may invalidate lower-level conclusions.

## Candidate policy

Keep candidate names diagnostic rather than cosmetic:
- A_CONTROL: current stable D3D9/SAFE control
- B_CORRECTNESS: isolated correctness hypothesis
- C_PERFORMANCE: isolated performance hypothesis after correctness gate
- D_DIRECTGPU_OPENXR: isolated transport/timing hypothesis
- E_DXVK_SAFE / E_DXVK_MULTIVIEW
- F_DX12_STRICT

Never create nominally different candidates from identical binaries.

## Runtime feedback ingestion

`docs/VR_RUNTIME_FEEDBACK.json` is the normalized durable input. Each test result should identify:
- BuildMatrixId
- VariantId
- SessionId
- source commit
- VDXR refresh/render scale when known
- startup/stereo/HUD/effects/recenter/pacing observations
- uploaded log bundle identity

The planner converts new runtime evidence into queue transitions on the next pass.

## Idle-time rule

A worker does not stop merely because its primary item is blocked. It moves to the next eligible queue item or a new review lens. It must not re-run unchanged work only to consume time.
