from pathlib import Path

ffb = Path('src/hooks_wheel_ffb.cpp').read_text(encoding='utf-8')
ui = Path('src/overlay/wheel_setup_ui.cpp').read_text(encoding='utf-8')

checks = [
    ('independent spring invert setting', 'Setting<bool> WheelFFBInvertSpring{' in ffb),
    ('spring no longer follows InvertForce', 'const LONG coefficient = Settings::WheelFFBInvertSpring' in ffb and 'const LONG coefficient = Settings::WheelFFBInvertForce' not in ffb),
    ('F11 ConstantForce direction control', ('Reverse ConstantForce' in ui or 'Reverse SAT / ConstantForce' in ui)),
    ('F11 spring direction control', 'Reverse Spring' in ui),
    ('hardware spring live-disable fallback', 'hardware spring disabled live; using software centering' in ffb),
    ('gameplay actuator retry path', 'note_device_failure("SETACTUATORSON", actuatorHr);' in ffb and 'deviceAcquired_ = false;' in ffb),
    ('initial actuator command checked', 'SETACTUATORSON during initialization failed' in ffb),
    ('periodic pair is atomic', 'complete hardware periodic pair unavailable; using ConstantForce fallback for both signals' in ffb),
    ('device reinit resets gain failure state', 'lastGainErrorLog_ = 0;' in ffb),
    ('diagnostic exposes both direction states', 'invCF={} spring={} invSpring={}' in ffb),
]

for label, ok in checks:
    if not ok:
        raise SystemExit(f'ROUND3 VERIFY FAILED: {label}')
    print(f'ROUND3 VERIFY OK: {label}')

# Recovery patch must actually be part of the effective source, not merely exist
# as a dormant helper script in the repository.
recovery_markers = [
    'FFB_DEVICE_FAILURE_GRACE_MS',
    'void request_device_reinitialize(',
    'void teardown_for_reinitialize()',
    'DirectInput device released; waiting to re-enumerate after device loss',
]
for marker in recovery_markers:
    if marker not in ffb:
        raise SystemExit(f'ROUND3 VERIFY FAILED: recovery marker missing: {marker}')

print('ROUND3 VERIFY OK: DirectInput recovery is active in effective source')
print('Round-3 ten-pass verification passed')
