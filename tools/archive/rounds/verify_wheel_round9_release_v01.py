from pathlib import Path


def read(path: str) -> str:
    return Path(path).read_text(encoding='utf-8')


def require(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle not in data:
        raise SystemExit(f'ROUND9 VERIFY FAILED [{label}]: {needle!r} missing from {path}')
    print(f'ROUND9 VERIFY OK [{label}]')

ffb = read('src/hooks_wheel_ffb.cpp')
input_ui = read('src/overlay/input_bindings_ui.cpp')
setup_ui = read('src/overlay/wheel_setup_ui.cpp')

# Round-9 originally installed the v0.1 R3 tune. Later experimental rounds may
# deliberately supersede the spring/damper/SAT values, so verify the retained
# release-era safety/detail invariants here and let the newest round verify its
# own final tuning exactly.
require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "GripLoss", 0.65f,', 'v0.1 grip-loss baseline retained')
require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "WallImpact", 0.38f,', 'restrained collision retained')
require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "RoadTexture", 0.30f,', 'road detail retained')
require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "TireSlip", 0.20f,', 'tire slip retained')

if 'Load MOZA R3 Natural SAT' in setup_ui:
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SpringStrength", 0.65f,', 'Round16 low-speed spring baseline')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "DamperStrength", 0.30f,', 'Round16 damping stabilizer')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SteeringWeight", 1.45f,', 'declared SAT default remains backward-compatible')
    require('src/overlay/wheel_setup_ui.cpp', 'Load MOZA R3 Natural SAT', 'Round16 natural setup preset visible')
elif 'Load MOZA R3 Strong SAT' in setup_ui:
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SpringStrength", 0.65f,', 'Round15 strong spring supersedes earlier tune')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "DamperStrength", 0.30f,', 'Round15 damping stabilizer')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SteeringWeight", 1.45f,', 'Round15 strong SAT supersedes earlier tune')
    require('src/overlay/wheel_setup_ui.cpp', 'Load MOZA R3 Strong SAT', 'Round15 setup preset visible')
elif 'MOZA R3 SAT test' in input_ui:
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SpringStrength", 0.32f,', 'Round11 supersedes v0.1 spring')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "DamperStrength", 0.34f,', 'Round11 supersedes v0.1 damping')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SteeringWeight", 1.10f,', 'Round11 supersedes cornering with SAT')
    require('src/overlay/wheel_setup_ui.cpp', 'Load MOZA R3 SAT test', 'Round11 setup preset visible')
else:
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SpringStrength", 0.60f,', 'v0.1 spring default')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "DamperStrength", 0.42f,', 'v0.1 damping default')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SteeringWeight", 0.38f,', 'v0.1 cornering default')
    require('src/overlay/input_bindings_ui.cpp', 'MOZA R3 v0.1 (default)', 'bindings panel v0.1 preset')
    require('src/overlay/input_bindings_ui.cpp', 'applyPreset(0.70f, 0.60f, 0.42f, 0.38f, 0.65f, 0.38f, 0.30f, 0.20f, 0.08f, 0.35f);', 'exact v0.1 profile values')
    require('src/overlay/wheel_setup_ui.cpp', 'Load MOZA R3 v0.1 (default)', 'F11 setup v0.1 preset')

# Existing comparison presets remain in source history even though the SDL Input
# Bindings dialog no longer exposes FFB tuning itself.
require('src/overlay/input_bindings_ui.cpp', 'Simulation Balanced v1', 'balanced comparison preset retained')
require('src/overlay/input_bindings_ui.cpp', 'Arcade Light v1', 'light comparison preset retained')
require('src/overlay/input_bindings_ui.cpp', 'Arcade Strong v1', 'strong comparison preset retained')

for path, data in [
    ('src/hooks_wheel_ffb.cpp', ffb),
    ('src/overlay/input_bindings_ui.cpp', input_ui),
    ('src/overlay/wheel_setup_ui.cpp', setup_ui),
]:
    if data.count('{') != data.count('}'):
        raise SystemExit(f'ROUND9 VERIFY FAILED [brace balance]: {path}')
    print(f'ROUND9 VERIFY OK [brace balance {path}]')

print('Round-9 release-era invariants verification passed')
