from pathlib import Path
import runpy

ffb_path = Path('src/hooks_wheel_ffb.cpp')
ui_path = Path('src/overlay/input_bindings_ui.cpp')
ini_path = Path('OutRun2006Tweaks.ini')

ffb = ffb_path.read_text(encoding='utf-8')
ui = ui_path.read_text(encoding='utf-8')
ini = ini_path.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected exactly one match, got {count}')
    print(f'cleanup patched: {label}')
    return text.replace(old, new, 1)


# 1) The multi-device binding screen still inherited the upstream gamepad reset
# value of 20%.  On a 270-degree wheel that is roughly +/-27 degrees around
# centre, so Reset to default must preserve the wheel-branch 0% baseline.
ui = rep(
    ui,
    'Settings::SteeringDeadZone = 0.2f;',
    'Settings::SteeringDeadZone = 0.0f;',
    'Reset to default keeps zero wheel deadzone')


# 2) The shipped values had already changed to the new multi-device input path,
# but the adjacent comments still told wheel users to leave UseNewInput false.
# Keep the documentation aligned with the actual hybrid architecture: SDL owns
# multi-device input, while the custom DirectInput COM engine remains the sole
# FFB owner. Legacy DirectInput remains an explicit fallback.
old_comment = '''# Enables new SDL-based input system
# Allowing game to see full trigger range without any shared trigger axes issues
# (experimental, not every menu/gamemode has been tested with it yet)
# Leave false for steering wheels. WheelInputCompatibility also enforces this
# after user.ini and command-line overrides are read.
UseNewInput = true
'''
new_comment = '''# Enables the multi-device SDL input system with raw joystick bindings.
# This is the wheel-build default; force feedback remains on the custom
# DirectInput COM engine. With an attached FFB wheel, automatic backend
# selection prefers SDL DirectInput. Set false only when deliberately using
# WheelInputCompatibility as the legacy input fallback.
UseNewInput = true
'''
ini = rep(ini, old_comment, new_comment, 'document the multi-device wheel default')


# 3) A direction-test button can be visible while the overlay is opened from a
# non-gameplay screen. Previously the request stayed queued and could fire on
# the first race frame. Reject such requests immediately, and also clear any
# pending test whenever the engine resets its gameplay signal state.
old_request = '''        void request_direction_test(int direction)
        {
            if (direction == 0)
            {
                manualTestFrames_ = 0;
                if (initialized_ && !panicStopped_)
                    set_constant_force(0);
                return;
            }
            manualTestDirection_ = direction < 0 ? -1 : 1;
            manualTestFrames_ = 18;
            spdlog::info(
                "WheelFFB: queued safe {} direction test at fixed 20% output",
                manualTestDirection_ < 0 ? "left" : "right");
        }
'''
new_request = '''        void request_direction_test(int direction)
        {
            if (direction == 0)
            {
                manualTestFrames_ = 0;
                manualTestDirection_ = 1;
                if (initialized_ && !panicStopped_)
                    set_constant_force(0);
                return;
            }

            const bool inGameplay =
                Game::current_mode && (*Game::current_mode == STATE_GAME);
            if (!inGameplay)
            {
                manualTestFrames_ = 0;
                manualTestDirection_ = 1;
                spdlog::warn(
                    "WheelFFB: ignored direction test outside gameplay; no torque was queued");
                return;
            }

            manualTestDirection_ = direction < 0 ? -1 : 1;
            manualTestFrames_ = 18;
            spdlog::info(
                "WheelFFB: queued safe {} direction test at fixed 20% output",
                manualTestDirection_ < 0 ? "left" : "right");
        }
'''
ffb = rep(ffb, old_request, new_request, 'direction test is gameplay-only')

old_reset = '''            splashTimer_ = 0;
            splashAmp_ = 0.0f;

            // Menu/race transitions must not reuse samples from the previous
'''
new_reset = '''            splashTimer_ = 0;
            splashAmp_ = 0.0f;
            manualTestFrames_ = 0;
            manualTestDirection_ = 1;

            // Menu/race transitions must not reuse samples from the previous
'''
ffb = rep(ffb, old_reset, new_reset, 'signal reset clears pending direction test')

ffb_path.write_text(ffb, encoding='utf-8')
ui_path.write_text(ui, encoding='utf-8')
ini_path.write_text(ini, encoding='utf-8')
print('Applied final wheel cleanup: reset deadzone, input docs, direction-test safety')

# Round-4: execute five additional source-hardening passes after all prior
# transformations so the checks operate on the exact effective source.
runpy.run_path('tools/patch_wheel_round4_fivepass.py', run_name='__main__')

# Round-5: repair the round-4 compiler regression and apply another five
# lifecycle/hotplug review passes to the exact effective source.
runpy.run_path('tools/patch_wheel_round5_fivepass.py', run_name='__main__')

# Final compiler-specific guard discovered by the real Win32 MSVC build: avoid
# Windows min/max macro expansion in the multi-device hotplug path.
runpy.run_path('tools/patch_wheel_round5_compile_fix.py', run_name='__main__')

# Round-6: five-pass review of lifecycle/focus races, exact device identity,
# FFB backend parity, numeric fail-safe behavior, and F11 runtime ownership.
runpy.run_path('tools/patch_wheel_round6_fivepass.py', run_name='__main__')

# Round-7: final DD-wheel runtime safety pass. Cancel transient force-test/event
# state on focus loss, keep direction tests pure, and forbid deferred test torque.
runpy.run_path('tools/patch_wheel_round7_final_safety.py', run_name='__main__')

# Round-8: input/key-setting hardening. Protect first-frame state, digital-axis
# aggregation, binding-file recovery, raw-axis release sequencing, and legacy
# F11 menu ownership without changing the force-feedback model.
runpy.run_path('tools/patch_wheel_round8_input_binding.py', run_name='__main__')

# Round-9 / v0.1: install the hardware-tested MOZA R3 personal release tune.
# It adds steering resistance through spring/damping while deliberately keeping
# collision feedback below the old Simulation Balanced reference preset.
runpy.run_path('tools/patch_wheel_round9_release_v01.py', run_name='__main__')

# Round-10: snow/ice stages use a high steady gamepad-rumble surface value.
# Attenuate only their road-texture sine so a DD wheel does not buzz continuously.
runpy.run_path('tools/patch_wheel_round10_snow_texture.py', run_name='__main__')
