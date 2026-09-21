from pathlib import Path

path = 'src/overlay/wheel_setup_ui.cpp'
text = Path(path).read_text(encoding='utf-8')


def require(needle: str, label: str) -> None:
    if needle not in text:
        raise SystemExit(f'ROUND13 VERIFY FAILED [{label}]: {needle!r}')
    print(f'ROUND13 VERIFY OK [{label}]')


require('void WheelFFB_RequestDirectionTest(int direction);', 'safe test function declared')
require('ImGui::Checkbox("Enable Force Feedback", Settings::WheelFFBEnable.ptr());', 'FFB enable visible')
require('Hardware road/slip sine effects', 'periodic effect toggle visible')
require('Diagnostic logging', 'diagnostic logging toggle visible')
require('Test Left (20%)', 'left direction test visible')
require('WheelFFB_RequestDirectionTest(-1);', 'left direction test wired')
require('Test Right (20%)', 'right direction test visible')
require('WheelFFB_RequestDirectionTest(1);', 'right direction test wired')
require('WheelFFB_RequestDirectionTest(0);', 'stop direction test wired')
if 'Load MOZA R3 Natural SAT' in text:
    require('Load MOZA R3 Natural SAT', 'Round16 natural SAT preset retained')
elif 'Load MOZA R3 Strong SAT' in text:
    require('Load MOZA R3 Strong SAT', 'Round15 strong SAT preset retained')
else:
    require('Load MOZA R3 SAT test', 'SAT preset retained')
require('Self-aligning Torque (SAT)', 'SAT slider retained')

if text.count('{') != text.count('}'):
    raise SystemExit('ROUND13 VERIFY FAILED [brace balance]')
print('ROUND13 VERIFY OK [brace balance]')
print('Round-13 complete single-page FFB controls verification passed')
