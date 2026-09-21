from pathlib import Path
import math


def read(path: str) -> str:
    return Path(path).read_text(encoding='utf-8')


def require(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle not in data:
        raise SystemExit(f'ROUND16 VERIFY FAILED [{label}]: {needle!r} missing from {path}')
    print(f'ROUND16 VERIFY OK [{label}]')


def forbid(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle in data:
        raise SystemExit(f'ROUND16 VERIFY FAILED [{label}]: stale {needle!r} remains in {path}')
    print(f'ROUND16 VERIFY OK [{label}]')


ffb_path = 'src/hooks_wheel_ffb.cpp'
ui_path = 'src/overlay/wheel_setup_ui.cpp'
vibration_path = 'src/hooks_forcefeedback.cpp'
input_ui_path = 'src/overlay/input_bindings_ui.cpp'

# Natural SAT shape and return-rate stability.
require(ffb_path, '(steerAbs - 0.025f) / 0.975f', 'soft centre deadband')
require(ffb_path, 'std::sin(satAngleInput * HalfPi)', 'progressive sine SAT angle curve')
require(ffb_path, 'const float satMotionGate =', 'smooth rolling-speed gate')
require(ffb_path, 'const float satLoadBoost = 0.72f + 0.38f * cornerLoadSmooth;', 'gentle lateral-load scaling')
require(ffb_path, '(driftAmt - 0.60f) / 0.35f', 'late smooth grip unload')
require(ffb_path, 'const bool returningToCentre = steer * steerRate < 0.0f;', 'detect fast return toward centre')
require(ffb_path, 'const float satReturnRelief = 1.0f - 0.45f * returnRateSmooth;', 'bounded return-rate relief')
forbid(ffb_path, 'std::pow((steerAbs - 0.012f) / 0.988f, 0.58f)', 'Round15 small-angle snap curve removed')
forbid(ffb_path, 'const float satLoadBoost = 1.0f + 1.10f * cornerLoad;', 'Round15 aggressive load boost removed')

# Spring is now a low-speed helper and cannot stack a second strong high-speed
# centering law on SAT.
require(ffb_path, '(speedNorm - 0.05f) / 0.35f', 'smooth spring fade window')
require(ffb_path, 'const float springSpeed = 1.0f - 0.88f * springFade;', 'high-speed spring fade')
require(ffb_path, 'const float springGrip = 0.80f + 0.20f * gripFactor;', 'bounded spring grip modulation')
forbid(ffb_path, 'std::pow(speedNorm, 1.60f)', 'old speed-increasing spring removed')
require(ffb_path, '-0.06f, 0.08f);', 'weight-transfer torque modulation bounded')
require(ffb_path, '? 0.18f : 0.12f;', 'arcade lateral signal smoothed')

# There must be one visible FFB settings owner and one force-output backend.
require(ffb_path, 'setting->section() == "WheelFFB"', 'generic WheelFFB registry entries hidden')
require(ffb_path, 'setting->hidden(true);', 'generic settings page suppression')
require(ui_path, 'Single-owner wheel FFB: DirectInput COM only.', 'dedicated FFB page declares ownership')
require(ui_path, 'Settings > WheelFFB is hidden;', 'duplicate generic settings page documented hidden')
require(ui_path, 'Load MOZA R3 Natural SAT', 'natural R3 preset visible')
require(ui_path, 'Centering Spring (low speed)', 'spring role is explicit')
forbid(ui_path, 'ImGui::SliderFloat("Low-speed Aligning"', 'obsolete low-speed spring knob hidden')
forbid(ui_path, 'ImGui::SliderFloat("Spring Corner-load Boost"', 'obsolete spring load knob hidden')
require(input_ui_path, 'saved and applied only from Input Bindings.', 'multi-device input remains sole input owner')
forbid(input_ui_path, 'ImGui::BeginTabItem("Force Feedback")', 'no duplicate FFB tab in Input Bindings')

# The original Xbox/SDL gamepad rumble route must not run alongside native
# wheel FFB. The upstream RX78 wheel_force_feedback.cpp is intentionally absent.
require(vibration_path, 'extern Setting<bool> WheelFFBEnable;', 'vibration hook sees native FFB ownership')
require(vibration_path, 'if (Settings::UseNewInput && Settings::WheelFFBEnable)', 'SDL rumble blocked while wheel FFB active')
require(vibration_path, 'InputManager_SetVibration(vib.wLeftMotorSpeed, vib.wRightMotorSpeed);', 'gamepad rumble remains available when native FFB is off')
if Path('src/wheel_force_feedback.cpp').exists():
    raise SystemExit('ROUND16 VERIFY FAILED [competing RX78 wheel_force_feedback.cpp is present]')
print('ROUND16 VERIFY OK [no competing RX78 wheel_force_feedback.cpp]')

# The preset deliberately keeps strong holding torque but removes the snap.
require(ui_path, 'Settings::WheelFFBSpringSaturation = 0.95f;', 'spring saturation preserves usable low-speed centre')
require(ui_path, 'Settings::WheelFFBSteeringWeight = 1.75f;', 'strong natural SAT preset')
require(ui_path, 'Settings::WheelFFBWeightTransfer = 0.20f;', 'subtle weight transfer preset')
require(ui_path, 'Settings::WheelFFBSlewRate = 0.045f;', 'smooth structural slew preset')
require(ui_path, 'Settings::VibrationMode = 0;', 'preset disables independent gamepad rumble')
require(ui_path, 'Settings::WheelFFBInvertForce = true;', 'known R3 ConstantForce direction retained')
require(ui_path, 'Settings::WheelFFBInvertSpring = false;', 'known R3 spring direction retained')
require(ui_path, 'Settings::write(Module::UserIniPath);', 'natural preset persists to user.ini')


def smoothstep(v: float) -> float:
    v = max(0.0, min(1.0, v))
    return v * v * (3.0 - 2.0 * v)


def sat_output(steer: float, speed: float, corner: float, drift: float, steer_rate: float = 0.0) -> float:
    steer_abs = max(0.0, min(1.0, abs(steer)))
    angle = max(0.0, min(1.0, (steer_abs - 0.025) / 0.975))
    angle_curve = math.sin(angle * math.pi / 2.0)
    motion = smoothstep((speed - 0.015) / 0.085)
    speed_curve = motion * (0.20 + 0.80 * math.sqrt(max(speed, 0.0)))
    load = 0.72 + 0.38 * smoothstep(corner)
    slip = smoothstep((drift - 0.60) / 0.35)
    grip = 1.0 - 0.65 * 0.65 * slip
    returning = steer * steer_rate < 0.0
    return_t = min(abs(steer_rate) / 0.08, 1.0) if returning else 0.0
    return_relief = 1.0 - 0.45 * smoothstep(return_t)
    raw = angle_curve * speed_curve * load * grip * return_relief * 1.75
    return math.tanh(raw * 0.70)


tiny = sat_output(0.05, 0.50, 0.30, 0.0)
small = sat_output(0.10, 0.50, 0.30, 0.0)
mid = sat_output(0.30, 0.50, 0.50, 0.0)
high = sat_output(0.50, 0.75, 0.70, 0.0)
deep = sat_output(0.50, 0.75, 0.70, 1.0)
fast_return = sat_output(0.30, 0.50, 0.50, 0.0, -0.08)
if not (0.0 < tiny < 0.05 and tiny < small < mid < high):
    raise SystemExit(f'ROUND16 VERIFY FAILED [progressive SAT]: tiny={tiny:.3f} small={small:.3f} mid={mid:.3f} high={high:.3f}')
if not (mid > 0.30 and high > 0.58):
    raise SystemExit(f'ROUND16 VERIFY FAILED [loaded-corner SAT too weak]: mid={mid:.3f} high={high:.3f}')
if not (0.50 < deep / high < 0.80):
    raise SystemExit(f'ROUND16 VERIFY FAILED [deep-slide unload]: deep={deep:.3f} high={high:.3f}')
if not fast_return < mid * 0.70:
    raise SystemExit(f'ROUND16 VERIFY FAILED [return whip relief]: fast={fast_return:.3f} hold={mid:.3f}')
print(f'ROUND16 VERIFY OK [SAT numeric]: tiny={tiny:.3f} small={small:.3f} mid={mid:.3f} high={high:.3f} deep={deep:.3f} fast-return={fast_return:.3f}')


def spring_coeff(speed: float) -> float:
    fade = smoothstep((speed - 0.05) / 0.35)
    return 0.65 * (1.0 - 0.88 * fade) * 0.70

park = spring_coeff(0.0)
mid_speed_spring = spring_coeff(0.20)
high_speed_spring = spring_coeff(0.50)
if not (park > 0.40 and mid_speed_spring > high_speed_spring and high_speed_spring < park * 0.15):
    raise SystemExit(f'ROUND16 VERIFY FAILED [spring role]: park={park:.3f} mid={mid_speed_spring:.3f} high={high_speed_spring:.3f}')
print(f'ROUND16 VERIFY OK [spring numeric]: park={park:.3f} mid={mid_speed_spring:.3f} high={high_speed_spring:.3f}')

for path in [ffb_path, ui_path, vibration_path, input_ui_path]:
    data = read(path)
    if data.count('{') != data.count('}'):
        raise SystemExit(f'ROUND16 VERIFY FAILED [brace balance]: {path}')
    print(f'ROUND16 VERIFY OK [brace balance {path}]')

print('Round-16 natural single-owner wheel FFB verification passed')
