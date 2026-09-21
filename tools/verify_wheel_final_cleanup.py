from pathlib import Path
import runpy


def read(path: str) -> str:
    return Path(path).read_text(encoding='utf-8')


def require(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle not in data:
        raise SystemExit(f'FINAL CLEANUP VERIFY FAILED [{label}]: {needle!r} missing from {path}')
    print(f'FINAL CLEANUP VERIFY OK [{label}]')


def forbid(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle in data:
        raise SystemExit(f'FINAL CLEANUP VERIFY FAILED [{label}]: stale {needle!r} remains in {path}')
    print(f'FINAL CLEANUP VERIFY OK [{label}]')


# Reset-to-default must not restore the original game's 20% gamepad deadzone.
require(
    'src/overlay/input_bindings_ui.cpp',
    'Settings::SteeringDeadZone = 0.0f;',
    'multi-device reset preserves zero steering deadzone')
forbid(
    'src/overlay/input_bindings_ui.cpp',
    'Settings::SteeringDeadZone = 0.2f;',
    'no stale 20-percent reset path')

# Shipped comments must agree with the shipped multi-device default.
require(
    'OutRun2006Tweaks.ini',
    '# This is the wheel-build default; force feedback remains on the custom',
    'UseNewInput documentation matches hybrid architecture')
require(
    'OutRun2006Tweaks.ini',
    'UseNewInput = true',
    'multi-device input remains the shipped default')
forbid(
    'OutRun2006Tweaks.ini',
    '# Leave false for steering wheels.',
    'remove obsolete wheel input guidance')

# Safe 20% force tests must never survive a menu/F11 transition into gameplay.
require(
    'src/hooks_wheel_ffb.cpp',
    'WheelFFB: ignored direction test outside gameplay; no torque was queued',
    'non-gameplay direction tests are rejected')
require(
    'src/hooks_wheel_ffb.cpp',
    'manualTestFrames_ = 0;\n            manualTestDirection_ = 1;\n\n            // Menu/race transitions',
    'pending direction test is cleared on signal reset')
require(
    'src/hooks_wheel_ffb.cpp',
    'Game::current_mode && (*Game::current_mode == STATE_GAME);',
    'direction test explicitly checks gameplay state')

print('Final wheel cleanup verification passed')

# Verify the additional source-level hardening passes on the final effective
# source produced by the build pipeline.
runpy.run_path('tools/verify_wheel_round4_fivepass.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round5_fivepass.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round6_fivepass.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round7_final_safety.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round8_input_binding.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round9_release_v01.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round10_snow_texture.py', run_name='__main__')
