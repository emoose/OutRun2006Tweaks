# VR Scheduled / Work Run History

This is the durable human-readable history for externally scheduled Work runs. Scheduling itself is intentionally not defined in the repository.

## Append policy

The INTEGRATION/PLANNER worker appends one compact entry after it consumes REVIEW/FIX/VALIDATION handoffs. Do not create an entry for a no-op run unless it changes queue state or records a meaningful blocking reason.

Each entry should contain:
- KST timestamp and worker/run identity
- source commit observed
- handoff generations consumed
- queue transitions
- source commit produced by FIX, if any
- validation/build/workflow/artifact IDs
- runtime session IDs consumed
- candidate matrix changes
- next eligible work

## Environment initialization — 2026-09-21 03:36 KST

Repository coordination was initialized around source baseline `ad14ed6c6757634233ec79b71f42be53c563edb7`.

Durable coordination files:
- `docs/VR_AUTODEV_PROTOCOL.md`
- `docs/VR_WORK_QUEUE.json`
- `docs/VR_RUNTIME_FEEDBACK.json`
- `docs/autodev/REVIEW_HANDOFF.json`
- `docs/autodev/FIX_HANDOFF.json`
- `docs/autodev/VALIDATION_HANDOFF.json`

Source-write ownership is restricted to FIX. Runtime correctness remains Quest 3/VDXR gated.


## Environment hardening — 2026-09-21 03:36 KST

Added lightweight coordination validation:
- `tools/Validate-VRAutodevState.ps1`
- `.github/workflows/vr-autodev-state.yml`

The validator checks JSON parseability, authority invariants, unique queue IDs, allowed state transitions, dependency references, two-attempt limits and handoff identities. The existing heavy unified backend workflow is path-filtered, so coordination-only document changes do not intentionally trigger the full D3D9/DXVK/DX12 package build.

Static connector-side validation of the six coordination JSON files passed with 7 queue items and zero detected invariant errors.
