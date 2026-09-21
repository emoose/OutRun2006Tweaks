# VR EXE / HUD Inspector

This pipeline removes most manual reverse-engineering work from HUD identification.

## What runs automatically

1. GitHub Actions downloads the same public replacement OR2006C2C.EXE used by the upstream OutRun2006Tweaks build.
2. tools/analyze_outrun_exe.py parses the PE32 image, calculates SHA-256, fingerprints known renderer/HUD entry points, and finds direct x86 CALL references into them.
3. The VR game DLL installs a passive HUD inspector when VR is enabled. It traces put_sprite_ex, sprani_play_ae_auth_alpha, and put_clip_sprite without altering their arguments or rendering.
4. The runtime trace records ASLR-safe caller RVAs, sprite IDs, coordinates, priorities, game mode/stage and known reverse-engineered regions.
5. The normal test launcher automatically enables the existing shader-bytecode fingerprint telemetry for the test process; normal non-test launches keep the HUD inspector disabled.
6. Run-OutRunVRTest.ps1 waits for the game to exit and invokes Collect-OutRunVRLogs.ps1 automatically. The collector includes the HUD CSV, a HUD summary, a shader-fingerprint summary, and the exact local OR2006C2C.EXE SHA-256/match verdict.

The normal test flow therefore remains: launch the standard test launcher, play the relevant screen/race, then exit normally. No Ghidra session, address lookup, hash command, or separate log collection is required.

## Runtime output

OutRun2006Tweaks-hudtrace.csv is deliberately rate-limited. A new fingerprint is written immediately and recurring fingerprints are written only at power-of-two count milestones. This keeps long races usable while retaining frequency evidence.

Important columns:

- return_rva: return address inside OR2006C2C.EXE.
- call_rva: return_rva minus five, matching the normal x86 CALL rel32 instruction address.
- known_area: labels existing reverse-engineered regions when known.
- arg0..arg5: API-specific sprite IDs, coordinates, flags, priority, color, or pointers.
- mode / stage: game context at the time of the draw request.

## Existing anchor

The upstream UI-scaling work already identifies sub_4BAD20 as the rival-car 1st/2nd/3rd marker renderer and records these call sites:

- sprani: 0xBB0FB, 0xBB133, 0xBB16C, 0xBB1A5
- put_clip_sprite: 0xBB21F, 0xBB241, 0xBB271, 0xBB2BC, 0xBB2D0

The runtime inspector labels these automatically. This gives a known-good end-to-end check that runtime caller RVAs and static EXE analysis agree.

## Static-analysis artifacts

The OutRun EXE HUD Inspector CI workflow uploads:

- OR2006C2C_EXE_ANALYSIS.json
- OR2006C2C_EXE_ANALYSIS.md
- a build-validated dinput8.dll containing the passive inspector

The static analyzer also verifies the nine previously reverse-engineered RankMarker call sites in the reference EXE. A mismatch fails CI rather than silently producing addresses for the wrong binary.

The JSON is machine-readable so later automation can convert confirmed runtime fingerprints into precise VR pass rules instead of broad primitive-count or render-state heuristics.
