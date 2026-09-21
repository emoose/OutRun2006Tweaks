# OutRun2 VR Development Execution Contract

This file defines the default execution model for substantial work in this repository, especially the OutRun2 VR/OpenXR backends and build matrix.

## Core rule

Do not run large review/fix/build/package tasks as one unbounded session. Treat work as resumable, bounded transactions with durable checkpoints. A fresh chat, automation run, or worker must be able to continue from repository state without relying on hidden conversation context.

Before substantial work, read:

- `docs/VR_AUTODEV_STATE.json` — machine-readable source of truth.
- `docs/VR_RUN_STATE.md` — concise human handoff, when present.
- `docs/VR_REVIEW_FINDINGS.md` — cumulative deduplicated findings, when present.
- `docs/VR_HOURLY_REVIEW_LOG.txt` and `docs/VR_BUILD_MATRIX_LOG.txt` when relevant.

If the human-readable state files do not exist, create them during the next safe checkpoint.

## Mandatory checkpoint flow

Use the following sequence by default:

### C0 — RECOVER
- Fetch the current development branch HEAD and every relevant component SHA.
- Read durable state and identify the exact unfinished checkpoint and resume cursor.
- Recover relevant CI runs, artifacts, logs, input hashes, blockers, and frozen package identity.
- Do not repeat completed work when the relevant source/dependency/config hashes are unchanged.

### C1 — REVIEW
- Perform one bounded evidence-driven review batch, normally five genuinely distinct lenses/passes.
- Deduplicate findings against prior evidence.
- Separate confirmed evidence, hypotheses, contrary evidence, and hardware-only validation needs.
- Large review requests such as "review 50 times" mean ten persistent five-pass batches (RB01..RB10), not one monolithic reread and not fifty superficial repetitions.

### C2 — IMPLEMENT
- Apply one small coherent fix set or one isolated experiment at a time.
- Do not mix unrelated risky changes in the same checkpoint.
- Preserve backend isolation and branch safety.

### C3 — VALIDATE
- Run the relevant static checks, regression checks, selector/protocol tests, and changed-input builds.
- Prefer fail-before/pass-after evidence where practical.
- A successful compile alone is not runtime proof.

### C4 — COMMIT
- Commit successful coherent changes to the active development branch with a descriptive message.
- Do not leave validated substantive changes only in ephemeral local state when safe remote publication is possible.
- Never force-publish over unrelated work or a live lease.

### C5 — PACKAGE
- Package only when the current phase requires a candidate.
- Verify actual binary/ZIP contents, manifests, component SHAs, config identity, checksums, selectors, and collectors.
- Frozen evening artifacts remain immutable during user testing.

### C6 — STATE
Before ending any substantial run, update durable continuation state.

At minimum record:
- schema version / run ID
- current checkpoint and status
- resume cursor / exact next action
- branch and integration HEAD
- all relevant component SHAs
- relevant source/dependency/config hashes
- completed review batch IDs
- finding IDs and status
- changed files and commits
- build/test/CI IDs and results
- candidate/artifact hashes
- blockers
- retry count for the same unchanged failure
- timestamp

Use atomic/CAS or equivalent single-writer protection where available.

## Bounded-run rule

A run must not keep expanding simply because more useful work exists. Prefer a complete durable checkpoint over an oversized unfinished session.

If the current batch cannot safely finish in the active execution:
1. persist exact partial status and resume cursor,
2. record what was actually completed,
3. stop cleanly,
4. let the next chat/automation run resume from that point.

Never claim background continuation.

## Failure rule

After two materially distinct failed repair attempts for the same unchanged failure:
- mark the item `BLOCKED`,
- preserve evidence and exact failure signatures,
- update durable state,
- move to independent work on the next run.

New evidence may reopen the item. Do not create unbounded repair loops and do not weaken verification to get a green result.

## Review batching

Use persistent review batch IDs:
- RB01 — full relevant source/build graph
- RB02 — caller/lifetime retrace
- RB03 — regression history
- RB04 — backend isolation
- RB05 — OpenXR / DirectGPU
- RB06 — D3D9 state / WVP / Reset / StateBlock
- RB07 — hot paths / frame pacing / draw amplification
- RB08 — failure paths / cleanup / fallback
- RB09 — build / selector / logging / packaging
- RB10 — adversarial integration review of the actual frozen candidate

Each batch should use five distinct lenses:
1. architecture/integration/build/package/license/regression boundaries
2. ownership/lifetime/synchronization/resource generations
3. stereo correctness (WVP/projection/HUD/sky/effects/recenter/menu/white rank-score/fallback)
4. performance (draw/state/caching/copies/waits/telemetry/XR pacing)
5. adversarial review trying to disprove earlier findings

Do not rerun a completed batch unless a relevant input changed. Mark only affected batches stale.

## Branch and release safety

- Treat `vr-openxr` as stable unless the user explicitly requests modification/merge.
- Use `vr-unified-backends` as the primary integration/development branch unless current durable state says otherwise.
- Preserve the five-mode architecture and explicit backend isolation.
- Record every component SHA used by a package; integration HEAD alone is not package identity.
- Do not silently substitute fallback backends or fake A-F variants.

## Interactive chat default

When a user asks to review, fix, build, package, or continue this OutRun2 VR project in chat, follow this contract automatically.

For long tasks:
- resume from repository state first,
- work in C0-C6 checkpoints,
- surface completed checkpoints as soon as they exist,
- persist enough state that a later chat can continue without the previous transcript,
- do not restart completed unchanged work.

User-provided runtime logs and Quest/VDXR tests remain the authority for hardware-only behavior; offline evidence must be labeled accordingly.
