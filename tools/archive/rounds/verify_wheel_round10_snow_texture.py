from pathlib import Path
import runpy

path = Path('src/hooks_wheel_ffb.cpp')
text = path.read_text(encoding='utf-8')


def require(needle: str, label: str) -> None:
    if needle not in text:
        raise SystemExit(f'ROUND10 VERIFY FAILED [{label}]: {needle!r}')
    print(f'ROUND10 VERIFY OK [{label}]')

require('uniqueStage == 4 || uniqueStage == 19 ||', 'forward snow/ice stage IDs')
require('uniqueStage == 34 || uniqueStage == 49;', 'reverse snow/ice stage IDs')
require('constexpr float SnowIceRoadTextureScale = 0.04f;', 'snow/ice road texture attenuation')
require('stageRoadTextureScale;', 'road sine uses stage attenuation')
require('outputStrength * stageRoadTextureScale;', 'splash periodic uses stage attenuation')
require('magnitudeClamped < 0.01f', 'existing tiny-periodic snap-to-zero remains active')

max_default_snow_road = 1.0 * 1.0 * 0.30 * 0.70 * 0.04
if not max_default_snow_road < 0.01:
    raise SystemExit(f'ROUND10 VERIFY FAILED [default snow road silence]: {max_default_snow_road}')
print(f'ROUND10 VERIFY OK [default snow road silence]: max={max_default_snow_road:.4f}')

pairs = {'(': ')', '{': '}', '[': ']'}
stack = []
in_string = False
escape = False
for ch in text:
    if in_string:
        if escape:
            escape = False
        elif ch == '\\':
            escape = True
        elif ch == '"':
            in_string = False
        continue
    if ch == '"':
        in_string = True
    elif ch in pairs:
        stack.append(ch)
    elif ch in pairs.values():
        if not stack or pairs[stack.pop()] != ch:
            raise SystemExit('ROUND10 VERIFY FAILED [delimiter balance]')
if stack:
    raise SystemExit('ROUND10 VERIFY FAILED [delimiter balance]')
print('ROUND10 VERIFY OK [delimiter balance]')
print('Round-10 snow/ice vibration suppression verification passed')

# Round-11 through Round-17 are chained by the Round-10 patch runner, so verify
# the final effective source in the same order.
runpy.run_path('tools/verify_wheel_round11_strong_sat.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round12_sat_input_ui.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round13_ffb_ui_complete.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round14_input_ui_roles.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round15_strong_r3_sat.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round16_natural_single_owner.py', run_name='__main__')
runpy.run_path('tools/verify_wheel_round17_physics_sat.py', run_name='__main__')
