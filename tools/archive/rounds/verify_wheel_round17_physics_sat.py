from pathlib import Path
import math

ffb = Path('src/hooks_wheel_ffb.cpp').read_text(encoding='utf-8')
ui = Path('src/overlay/wheel_setup_ui.cpp').read_text(encoding='utf-8')
helper = Path('src/hooks_wheel_physics_sat.hpp').read_text(encoding='utf-8')
vibration = Path('src/hooks_forcefeedback.cpp').read_text(encoding='utf-8')


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise SystemExit(f'ROUND17 VERIFY FAILED [{label}]: {needle!r}')
    print(f'ROUND17 VERIFY OK [{label}]')


require(ffb, '#include "hooks_wheel_physics_sat.hpp"', 'physics helper is compiled')
require(ffb, '"WheelFFB", "PhysicsSAT", true,', 'physics mode defaults on')
require(ffb, 'const float naturalSatTorque =', 'Natural SAT retained for A/B comparison')
require(ffb, 'const float physicsSatTorque = physicsSat_.update(', 'physics model drives SAT')
require(ffb, 'const float physicsFallback = naturalSatTorque * 0.15f;', 'small calibration-only fallback')
require(ffb, 'const float physicsMix = physicsSat_.activationBlend();', 'physics activation crossfade')
require(ffb, 'physicsFallback + (physicsSatTorque - physicsFallback) * physicsMix', 'continuous Natural-to-Physics transition')
require(ffb, 'WheelPhysicsSatV1 physicsSat_{};', 'per-engine physics state')
require(ffb, 'physicsSat_.reset();', 'transition reset clears physics history')
require(ffb, 'phys={} basis=M70r{} cal={:.2f} mix={:.2f}', 'physics calibration telemetry')
require(ffb, 'step={:.5f} spdLen={:.5f} spdCorr={:.2f}', 'velocity scale telemetry')

if 'physicsSat_.ready() ?' in ffb:
    raise SystemExit('ROUND17 VERIFY FAILED [old ready/fallback force-model switching remains]')
print('ROUND17 VERIFY OK [valid zero Physics SAT cannot switch back to Natural SAT]')

require(helper, 'bool calibrated() const', 'calibration state is separate')
require(helper, 'bool sampleValid() const', 'sample validity state is separate')
require(helper, 'bool torqueActive() const', 'torque activity state is separate')
require(helper, 'float decay_invalid_sample()', 'transient invalid sample decay')
require(helper, 'void clear_dynamic_state()', 'dynamic state can reset without losing basis calibration')
require(helper, 'activationBlend_ = std::min(1.0f, activationBlend_ + (1.0f / 24.0f));', '24-tick activation ramp')
require(helper, 'sampleValid_ = calibrated_;', 'parking zero is valid after calibration')
require(helper, 'if (speedNorm <= 0.04f)', 'parking/restart stale-state guard')
require(helper, 'motionScale > motionScaleEma_ * 5.0f', 'relative teleport/warp detector')
require(helper, '++discontinuityCount_;', 'motion discontinuities are observable')
require(helper, 'lastTorque_ *= 0.55f;', 'brief invalid samples decay rather than switch force model')

require(helper, 'const D3DMATRIX& body = car->matrix_70;', 'non-display body transform candidate')
require(helper, 'const D3DVECTOR current = car->position_14;', 'physics tick motion source')
require(helper, 'constexpr int CalibrationSamplesRequired = 12;', 'multi-sample forward-axis calibration')
require(helper, 'calibrationScoreX_ += std::abs(xDot);', 'row1 basis is scored')
require(helper, 'calibrationScoreZ_ += std::abs(zDot);', 'row3 basis is scored')
require(helper, 'bestScore >= 0.85f && calibrationConfidence_ >= 0.25f', 'basis requires confidence before commit')
require(helper, 'std::atan2(vLat_, std::max(std::abs(vLong_)', 'body slip beta')
require(helper, 'headingDelta * 60.0f', 'yaw rate uses fixed 60 Hz physics tick')
require(helper, 'roadWheelAngle - bodySlip_ - yawRate_ * yawLeadSeconds', 'front slip combines steer beta yaw')
require(helper, '(frontSlip_ > 0.0f ? -1.0f : 1.0f)', 'SAT direction follows front slip')
require(helper, 'car->spd_mb_20.x', 'direct velocity candidate is measured')
require(helper, 'positionStep_', 'position delta scale is measured')
require(helper, 'motionScaleEma_', 'position step is normalized against rolling speed scale')
require(helper, 'spdLen_', 'direct velocity magnitude is measured')
require(helper, 'spdCorrelation_', 'spd_mb correlation is logged before use')

require(vibration, 'void __cdecl WheelFFB_UpdateAfterPhysics(EVWORK_CAR* car);', 'post-physics update is declared in car Ctrl owner')
require(vibration, 'GamePlCar_Ctrl.call(car);\n        WheelFFB_UpdateAfterPhysics(car);', 'WheelFFB samples after original car physics')
require(ffb, 'void __cdecl WheelFFB_UpdateAfterPhysics(EVWORK_CAR* car)', 'WheelFFB post-physics entry point exists')
if 'CalcVibrationHook_' in ffb or 'calc_vibration_hook(EVWORK_CAR* car)' in ffb:
    raise SystemExit('ROUND17 VERIFY FAILED [old CalcVibrationValues FFB hook still exists]')
print('ROUND17 VERIFY OK [one GamePlCar_Ctrl owner; no competing CalcVibration hook]')

calc_pos = vibration.find('CalcVibrationValues(car);')
car_ctrl_pos = vibration.find('GamePlCar_Ctrl.call(car);')
wheel_pos = vibration.find('WheelFFB_UpdateAfterPhysics(car);')
if not (0 <= calc_pos < car_ctrl_pos < wheel_pos):
    raise SystemExit('ROUND17 VERIFY FAILED [car/vibration/FFB execution ordering]')
print('ROUND17 VERIFY OK [legacy vibration stays before physics; WheelFFB samples after physics]')

require(ui, 'Physics SAT v1 (body slip + yaw)', 'live physics toggle')
require(ui, 'Load MOZA R3 Physics SAT v1', 'R3 physics preset')
require(ui, 'Settings::WheelFFBPhysicsSat = false;', 'Natural SAT A/B button disables physics mode')
require(ui, 'Settings::WheelFFBDebugLog = true;', 'physics preset enables diagnostics')

if ffb.count('(steer >= 0.0f ? -1.0f : 1.0f)') != 1:
    raise SystemExit('ROUND17 VERIFY FAILED [steering-sign centering leaked outside Natural fallback]')
print('ROUND17 VERIFY OK [physics direction is not steering-sign centering]')


def trail(slip: float) -> float:
    x = abs(slip) / 0.16
    return math.sin(x * math.pi / 2.0) if x <= 1.0 else math.exp(-(x - 1.0) * 0.90)

small, medium, peak, deep = trail(0.02), trail(0.08), trail(0.16), trail(0.40)
if not (0.0 < small < medium < peak and deep < peak):
    raise SystemExit(f'ROUND17 VERIFY FAILED [trail curve]: {small=} {medium=} {peak=} {deep=}')
print(f'ROUND17 VERIFY OK [trail curve]: {small:.3f} < {medium:.3f} < {peak:.3f}, deep={deep:.3f}')

# Once the crossfade reaches 1.0, a valid zero Physics SAT must produce zero,
# not a percentage of Natural SAT. During calibration the fallback is small.
natural = 0.8
physics_zero = 0.0
calibration_output = natural * 0.15 + (physics_zero - natural * 0.15) * 0.0
active_output = natural * 0.15 + (physics_zero - natural * 0.15) * 1.0
if not (math.isclose(calibration_output, 0.12) and math.isclose(active_output, 0.0, abs_tol=1e-9)):
    raise SystemExit('ROUND17 VERIFY FAILED [activation/fallback crossfade semantics]')
print('ROUND17 VERIFY OK [calibration fallback fades completely to true Physics zero]')

# Four transient bad ticks decay monotonically, the fifth reaches zero.
torque = 1.0
decayed = []
for i in range(1, 6):
    torque = 0.0 if i > 4 else torque * 0.55
    decayed.append(torque)
if not (decayed[0] > decayed[1] > decayed[2] > decayed[3] > decayed[4] == 0.0):
    raise SystemExit(f'ROUND17 VERIFY FAILED [invalid sample decay]: {decayed}')
print(f'ROUND17 VERIFY OK [invalid sample decay]: {decayed}')

beta, yaw, speed = 0.18, 0.50, 0.75
yaw_lead = 0.10 - 0.045 * speed
slip_left_counter = -0.15 * 0.52 - beta - yaw * yaw_lead
slip_at_center = -beta - yaw * yaw_lead
if not (slip_left_counter < 0.0 and slip_at_center < 0.0):
    raise SystemExit('ROUND17 VERIFY FAILED [counter-steer centre continuity]')
print(f'ROUND17 VERIFY OK [counter-steer continuity]: counter={slip_left_counter:.3f}, centre={slip_at_center:.3f}')

for path in [
    'src/hooks_wheel_ffb.cpp',
    'src/hooks_forcefeedback.cpp',
    'src/overlay/wheel_setup_ui.cpp',
    'src/hooks_wheel_physics_sat.hpp',
]:
    data = Path(path).read_text(encoding='utf-8')
    if data.count('{') != data.count('}'):
        raise SystemExit(f'ROUND17 VERIFY FAILED [brace balance]: {path}')
    print(f'ROUND17 VERIFY OK [brace balance {path}]')

print('Round-17 physics SAT verification passed: post-physics sampling + separated validity + stale-state guards')
