from pathlib import Path

path = Path("src/hooks_wheel_ffb.cpp")
text = path.read_text(encoding="utf-8")

old = '''            // DirectInput's condition equation uses A(q-q0). A spring must use\n            // negative coefficients so displacement produces force back toward\n            // the zero offset rather than accelerating away from center.\n            const LONG coefficient = -coefficientMagnitude;\n'''
new = '''            // R3 testing showed the old negative GUID_Spring coefficient pushes\n            // the wheel farther in the direction of steering instead of back to\n            // center. Keep hardware-spring direction consistent with the global\n            // InvertForce option: normal (false) uses the R3 centering sign, and\n            // inverted (true) reverses it together with ConstantForce.\n            const LONG coefficient = Settings::WheelFFBInvertForce\n                ? -coefficientMagnitude\n                : coefficientMagnitude;\n'''

count = text.count(old)
if count != 1:
    raise SystemExit(f"spring coefficient: expected exactly one match, found {count}")
text = text.replace(old, new, 1)

old_log = '"WheelFFB: GUID_Spring created (saturation={} / {}, negative coefficients pull toward center)"'
new_log = '"WheelFFB: GUID_Spring created (saturation={} / {}, R3 normal sign=positive, follows InvertForce)"'
if text.count(old_log) != 1:
    raise SystemExit("spring log string not found exactly once")
text = text.replace(old_log, new_log, 1)

path.write_text(text, encoding="utf-8")
print("Applied R3 GUID_Spring direction patch")
