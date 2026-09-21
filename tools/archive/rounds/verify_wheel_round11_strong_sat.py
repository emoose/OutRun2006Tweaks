from pathlib import Path
import math


def read(path: str) -> str:
    return Path(path).read_text(encoding='utf-8')


def require(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle not in data:
        raise SystemExit(f'ROUND11 VERIFY FAILED [{label}]: {needle!r} missing from {path}')
    print(f'ROUND11 VERIFY OK [{label}]')


def forbid(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle in data:
        raise SystemExit(f'ROUND11 VERIFY FAILED [{label}]: stale {needle!r} remains in {path}')
    print(f'ROUND11 VERIFY OK [{label}]')

ffb = read('src/hooks_wheel_ffb.cpp')
setup_ui = read('src/overlay/wheel_setup_ui.cpp')
round16 = 'Load MOZA R3 Natural SAT' in setup_ui
round15 = 'Load MOZA R3 Strong SAT' in setup_ui

if round16:
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SteeringWeight", 1.45f,', 'Round16 retains declared SAT baseline')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SpringStrength", 0.65f,', 'Round16 retains low-speed spring baseline')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "DamperStrength", 0.30f,', 'Round16 damping stabilizer')
    require('src/hooks_wheel_ffb.cpp', 'const float satAngleInput =', 'Round16 natural SAT successor')
elif round15:
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SteeringWeight", 1.45f,', 'Round15 SAT supersedes Round11')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SpringStrength", 0.65f,', 'Round15 spring supersedes Round11')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "DamperStrength", 0.30f,', 'Round15 damping supersedes Round11')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SpringLoadBoost", 0.30f,', 'Round15 spring load boost')
else:
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SteeringWeight", 1.10f,', 'strong SAT default')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SpringStrength", 0.32f,', 'lighter generic spring')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "DamperStrength", 0.34f,', 'damper is stabilizer')
    require('src/hooks_wheel_ffb.cpp', '"WheelFFB", "SpringLoadBoost", 0.18f,', 'spring load boost reduced')

require('src/hooks_wheel_ffb.cpp', 'const float selfAligningTorque =', 'explicit SAT torque')
require('src/hooks_wheel_ffb.cpp', '(steer >= 0.0f ? -1.0f : 1.0f)', 'logical SAT returns toward centre')
require('src/hooks_wheel_ffb.cpp', 'structural = (softwareSpring + selfAligningTorque) * loadMod + damper;', 'SAT is primary structural torque')
forbid('src/hooks_wheel_ffb.cpp', 'latNorm * speedNorm *\n                static_cast<float>(Settings::WheelFFBSteeringWeight)', 'old signed lateral torque removed')
require('src/overlay/input_bindings_ui.cpp', 'Self-aligning torque (SAT)', 'bindings SAT source retained')
require('src/overlay/wheel_setup_ui.cpp', 'Self-aligning Torque (SAT)', 'setup SAT slider')

# Round-11 reference model remains as a historical regression check unless a
# later hardware-tested natural model has replaced its exact curve. Round-16's
# own verifier checks the final response shape numerically.
if not round16:
    def sat(steer: float, speed: float, corner: float, drift: float) -> float:
        steer_abs = min(abs(steer), 1.0)
        steer_for_sat = ((steer_abs - 0.012) / 0.988) ** 0.78 if steer_abs > 0.012 else 0.0
        sat_speed = speed ** 0.62
        sat_load = 1.0 + 0.80 * corner
        sat_slip = min(max((drift - 0.35) / 0.65, 0.0), 1.0)
        sat_grip = 1.0 - 0.65 * (sat_slip ** 1.35)
        return steer_for_sat * sat_speed * sat_load * sat_grip * 1.10

    low_speed = sat(0.50, 0.25, 0.50, 0.10)
    mid_speed = sat(0.50, 0.50, 0.50, 0.10)
    high_speed = sat(0.50, 0.75, 0.50, 0.10)
    small_angle = sat(0.20, 0.75, 0.50, 0.10)
    large_angle = sat(0.60, 0.75, 0.50, 0.10)
    deep_drift = sat(0.50, 0.75, 0.50, 1.00)
    if not (0.0 < low_speed < mid_speed < high_speed):
        raise SystemExit('ROUND11 VERIFY FAILED [SAT must grow with speed]')
    if not (0.0 < small_angle < large_angle):
        raise SystemExit('ROUND11 VERIFY FAILED [SAT must grow with steering angle]')
    if not (deep_drift < high_speed * 0.50):
        raise SystemExit('ROUND11 VERIFY FAILED [deep drift must unload SAT strongly]')
    if not (high_speed > 0.70):
        raise SystemExit('ROUND11 VERIFY FAILED [normal high-speed SAT is too weak]')
    print(f'ROUND11 reference SAT: low={low_speed:.4f} mid={mid_speed:.4f} high={high_speed:.4f} deep-drift={deep_drift:.4f}')
else:
    print('ROUND11 VERIFY OK [exact curve superseded by Round16 natural SAT verifier]')

for path in ['src/hooks_wheel_ffb.cpp', 'src/overlay/input_bindings_ui.cpp', 'src/overlay/wheel_setup_ui.cpp']:
    data = read(path)
    if data.count('{') != data.count('}'):
        raise SystemExit(f'ROUND11 VERIFY FAILED [brace balance]: {path}')
    print(f'ROUND11 VERIFY OK [brace balance {path}]')

print('Round-11 pseudo-SAT architecture verification passed')
