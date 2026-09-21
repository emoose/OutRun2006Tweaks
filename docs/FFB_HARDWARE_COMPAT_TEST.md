# FFB hardware compatibility retest

This checklist is for the `wheel-ffb-v0.3-compat` branch. The steering feel is the established Modern / Physics SAT model from `wheel-ffb-v0.2-dev`; X-Force is not part of this compatibility branch.

## What changed

- FFB profile persistence now keeps the staged-write/backup path, but falls back from `rename` to `copy_file` on Windows when the final rename is rejected by a filesystem/filter/driver edge case.
- First-time profile creation treats a missing destination `.ini` and missing `.bak` as the normal new-profile state instead of a filesystem failure. This specifically fixes the logged `Could not inspect the existing profile: The system cannot find the file specified` failure.
- The final profile file is verified after installation.
- Profile save success and failure messages include the real runtime destination path. Failures are also written to `OutRun2006Tweaks.log`.
- The Ready to Drive panel distinguishes a real failure `[!]` from a pending state `[..]`.
- Outside gameplay, `FFB device (starts in gameplay)` is pending rather than failed because the runtime intentionally acquires the saved DirectInput FFB GUID when driving starts.
- `Direction test (not run)` is a setup confirmation state, not a hardware failure.

## Retest sequence

1. Start the game, open **Force Feedback** before entering a race, and select the intended FFB output interface.
2. Confirm the menu-time status shows `[..] FFB device (starts in gameplay)` instead of treating `waiting / released / inactive` as a hardware error.
3. Enter gameplay. Confirm FFB initializes and works normally.
4. Run **Test Left** and **Test Right** once. Confirm the direction-test status changes from pending to OK when the directions are correct.
5. Make sure `OutRun2006Tweaks.profiles\FFB\ffb.ini` does **not** already exist, enter `ffb` as the FFB profile name, and select **Save as profile**. This explicitly tests the first-save path that previously failed.
6. Confirm the UI reports the full saved path and that `<game folder>\OutRun2006Tweaks.profiles\FFB\ffb.ini` exists.
7. Change one FFB value and overwrite the same `ffb` profile once. Confirm overwrite also succeeds.
8. If saving fails, attach `OutRun2006Tweaks.log`; the error should now include the destination folder and the Windows filesystem error.
9. During gameplay, disconnect and reconnect the wheelbase once. Confirm the device is reacquired and FFB resumes without restarting the game.
10. Repeat one short left and right corner and confirm SAT direction/feel is unchanged from the previous Modern / Physics SAT build.

## Requested hardware coverage

- Fanatec wheelbase / DirectInput FFB interface
- Simucube wheelbase / SC-Link or the actual force-feedback interface exposed by its driver
- MOZA R3 regression check

Do not change force-feel tuning for this test. The goal is device/runtime/profile compatibility, not a new FFB character.
