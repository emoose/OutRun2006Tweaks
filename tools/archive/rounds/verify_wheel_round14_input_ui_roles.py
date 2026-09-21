from pathlib import Path


def read(path: str) -> str:
    return Path(path).read_text(encoding='utf-8')


def require(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle not in data:
        raise SystemExit(f'ROUND14 VERIFY FAILED [{label}]: {needle!r} missing from {path}')
    print(f'ROUND14 VERIFY OK [{label}]')


def forbid(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle in data:
        raise SystemExit(f'ROUND14 VERIFY FAILED [{label}]: stale {needle!r} remains in {path}')
    print(f'ROUND14 VERIFY OK [{label}]')


require(
    'src/overlay/input_bindings_ui.cpp',
    'With UseNewInput enabled, steering, pedals, buttons, menu controls and calibration are saved and applied only from Input Bindings.',
    'Input Bindings declares sole SDL input ownership')
require(
    'src/overlay/input_bindings_ui.cpp',
    'Force feedback is configured separately in the Force Feedback tab.',
    'Input Bindings points to FFB page')
forbid(
    'src/overlay/input_bindings_ui.cpp',
    'ImGui::BeginTabItem("Force Feedback")',
    'no duplicate FFB tab inside Input Bindings')
require(
    'src/overlay/wheel_setup_ui.cpp',
    'This page does not create input bindings; it only selects the DirectInput FFB wheel and tunes its forces.',
    'main page is FFB-only')
require(
    'src/overlay/wheel_setup_ui.cpp',
    'Input setup: Input Bindings only. FFB setup: this Force Feedback page only.',
    'navigation uses exact Input Bindings name')
require(
    'src/overlay/wheel_setup_ui.cpp',
    'return Settings::UseNewInput ? "Force Feedback" : "Legacy Wheel Setup";',
    'legacy mapper remains compatibility-only')
require(
    'src/overlay/wheel_setup_ui.cpp',
    'return Settings::WheelInputCompatibility && !Settings::UseNewInput &&',
    'legacy input hooks cannot own default SDL input')
forbid(
    'src/overlay/wheel_setup_ui.cpp',
    'game Controls / Controller Setup',
    'stale ambiguous navigation removed')

for path in ['src/overlay/input_bindings_ui.cpp', 'src/overlay/wheel_setup_ui.cpp']:
    data = read(path)
    if data.count('{') != data.count('}'):
        raise SystemExit(f'ROUND14 VERIFY FAILED [brace balance]: {path}')
    print(f'ROUND14 VERIFY OK [brace balance {path}]')

print('Round-14 single-owner input/FFB UI verification passed')
