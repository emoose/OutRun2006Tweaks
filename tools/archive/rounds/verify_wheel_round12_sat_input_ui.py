from pathlib import Path


def read(path: str) -> str:
    return Path(path).read_text(encoding='utf-8')


def require(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle not in data:
        raise SystemExit(f'ROUND12 VERIFY FAILED [{label}]: {needle!r} missing from {path}')
    print(f'ROUND12 VERIFY OK [{label}]')


def forbid(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle in data:
        raise SystemExit(f'ROUND12 VERIFY FAILED [{label}]: stale {needle!r} remains in {path}')
    print(f'ROUND12 VERIFY OK [{label}]')


# SAT must consume the SDL multi-device steering state directly when UseNewInput
# is active, while retaining the legacy game-function route as fallback.
require('src/hooks_wheel_ffb.cpp', 'extern float InputManager_SteeringValue();', 'direct SDL steering export declared')
require('src/hooks_wheel_ffb.cpp', 'if (Settings::UseNewInput)\n            {\n                const float steering = InputManager_SteeringValue();', 'new input SAT uses direct steering')
require('src/hooks_wheel_ffb.cpp', 'auto getVolume = Module::fn_ptr<GetVolumeFn>(0x53720);', 'legacy steering fallback retained')
require('src/hooks_wheel_ffb.cpp', 'sat={:.3f}', 'diagnostic logs SAT')
require('src/hooks_wheel_ffb.cpp', 'steerSrc={}', 'diagnostic logs steering source')
require('src/hooks_wheel_ffb.cpp', 'Settings::UseNewInput ? "SDL" : "legacy"', 'diagnostic source selector')

forbid('src/overlay/input_bindings_ui.cpp', 'ImGui::BeginTabItem("Force Feedback")', 'duplicate Controls FFB tab removed')
require('src/overlay/wheel_setup_ui.cpp', 'return Settings::UseNewInput ? "Force Feedback" : "Legacy Wheel Setup";', 'main tab has clear role')
require('src/overlay/wheel_setup_ui.cpp', '"Force feedback only. With UseNewInput enabled, steering, pedals, buttons, menu controls and calibration come only from Input Bindings.', 'SDL navigation guidance')
require('src/overlay/wheel_setup_ui.cpp', 'if (!Settings::UseNewInput)\n            {\n            const int regularSlots', 'legacy mapper gated')
require('src/overlay/wheel_setup_ui.cpp', 'if (!Settings::UseNewInput)\n            {\n            ImGui::SeparatorText("Wheel options")', 'legacy wheel options gated')
require('src/overlay/wheel_setup_ui.cpp', 'Settings::write(Module::UserIniPath);', 'FFB device/preset selection persists')
require('src/overlay/wheel_setup_ui.cpp', 'Settings::WheelFFBEnable = true;', 'SAT preset recovers disabled FFB')
require('src/overlay/wheel_setup_ui.cpp', '"Input setup: Input Bindings only. FFB setup: this Force Feedback page only.', 'single navigation summary')

require('src/hooks_wheel_ffb.cpp', 'const float selfAligningTorque =', 'SAT model retained')
require('src/hooks_wheel_ffb.cpp', 'structural = (softwareSpring + selfAligningTorque) * loadMod + damper;', 'SAT output retained')
setup = read('src/overlay/wheel_setup_ui.cpp')
if 'Load MOZA R3 Natural SAT' in setup:
    require('src/overlay/wheel_setup_ui.cpp', 'Settings::WheelFFBSteeringWeight = 1.75f;', 'Round16 natural SAT preset retained')
elif 'Load MOZA R3 Strong SAT' in setup:
    require('src/overlay/wheel_setup_ui.cpp', 'Settings::WheelFFBSteeringWeight = 1.45f;', 'Round15 strong SAT setting retained')
else:
    require('src/overlay/wheel_setup_ui.cpp', 'Load MOZA R3 SAT test', 'SAT test preset retained')
    require('src/overlay/wheel_setup_ui.cpp', 'Settings::WheelFFBSteeringWeight = 1.10f;', 'Round11 SAT setting retained')

for path in ['src/hooks_wheel_ffb.cpp', 'src/overlay/input_bindings_ui.cpp', 'src/overlay/wheel_setup_ui.cpp']:
    data = read(path)
    if data.count('{') != data.count('}'):
        raise SystemExit(f'ROUND12 VERIFY FAILED [brace balance]: {path}')
    print(f'ROUND12 VERIFY OK [brace balance {path}]')

print('Round-12 SAT steering-source and UI consolidation verification passed')
