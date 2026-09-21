# VR HUD Semantic Baseline

Baseline branch: `vr-unified-backends`

This file defines the source-level HUD policy inherited from the original
`hooks_uiscaling.cpp` reverse engineering. It is intentionally the reference
for scheduled review workers: do not replace these semantic identities with
primitive-count or broad shader/WVP guesses without contrary runtime evidence.

## Screen-space HUD

These are common-centre / zero-disparity VR HUD and pass through the existing
R30 screen-space/XYZRHW HUD transform and `VR/HudScale`:

- `HUD_TIME_ATTACK` — Time Attack timer and related scroll elements.
- `HUD_RANK` — race position/rank HUD, including the DispRank family.
- `HUD_GEAR_REV` — REV/gear indicator.
- `HUD_GHOST` — Ghost / You / Diff.
- `HUD_GOAL_TIME` — Time Attack goal time.
- `HUD_HEART_TOTAL` — HUD heart totals / C2C heart counters.
- `HUD_RIVAL` — screen HUD rival indicators.
- `HUD_GF_SPEECH` — C2C girlfriend speech bubble family.
- `HUD_RANK_EMOJI` and `HUD_RANK_TEXT` — ranking emoji and rank text.
- `HUD_GF_WARNING`, `HUD_SLIPSTREAM`, `HUD_FRUIT` — other known C2C HUD.

## World-space exceptions

These must retain true stereo/world attachment and must not be flattened into
the common-centre HUD plane:

- `WORLD_RIVAL_MARKER` — `sub_4BAD20`, rival-car 1st/2nd/etc markers.
- `WORLD_HEART` — `HeartDisp_car_heart`, hearts attached to cars/world.

## Automatic verification

`OutRun2006Tweaks-hudtrace.csv` schema v2 now records
`known_area,semantic,space_policy`.

The collector automatically produces:

- `HUD_TRACE_SUMMARY.txt` — detailed observed callers and geometry.
- `HUD_SEMANTIC_COVERAGE.txt` — expected semantic families observed during
  that session. `NOT_OBSERVED_THIS_SESSION` is not a failure; the game mode
  may simply not have displayed that HUD.

`tools/analyze_outrun_exe.py` applies the same semantic ranges to the static
direct-CALL inventory, so CI artifacts and runtime logs use the same names.

## Baseline rule for future scheduled review

Preserve semantic separation first. A source change may refine an exact
call-site or split a category, but broad promotion of screen HUD to world 3D or
world billboards to zero-disparity HUD requires specific runtime evidence.
