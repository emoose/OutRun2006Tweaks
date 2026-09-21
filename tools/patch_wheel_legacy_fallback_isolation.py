from pathlib import Path

# The modern multi-device hook and the legacy wheel helpers touch several of
# the same OutRun entry points (ReadIO/SwitchOn/SwitchNow/GetVolume).  Runtime
# guards are not enough: installing both SafetyHook chains at startup makes
# ordering dependent on static hook registration.  UseNewInput is restart-only,
# so do not install legacy hooks at all when the new path is active.

targets = [
    'src/hooks_wheel_input_compat_v2.hpp',
    'src/hooks_wheel_r3_menu_dpad.hpp',
    'src/hooks_wheel_r3_menu_ab.hpp',
    'src/hooks_wheel_menu_keyboard_back.hpp',
    'src/hooks_wheel_menu_keyboard_select.hpp',
    'src/hooks_wheel_legacy_blank_defaults.hpp',
]

for name in targets:
    path = Path(name)
    text = path.read_text(encoding='utf-8')
    old = '''        bool validate() override\n        {\n            return Settings::WheelInputCompatibility;\n        }'''
    new = '''        bool validate() override\n        {\n            return Settings::WheelInputCompatibility && !Settings::UseNewInput;\n        }'''
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{name}: expected one legacy validate block, found {count}')
    text = text.replace(old, new, 1)
    path.write_text(text, encoding='utf-8')
    print(f'isolated legacy hook: {name}')

# wheel_setup_ui uses a compact one-line validate method.
path = Path('src/overlay/wheel_setup_ui.cpp')
text = path.read_text(encoding='utf-8')
old = '        bool validate() override { return Settings::WheelInputCompatibility; }'
new = '        bool validate() override { return Settings::WheelInputCompatibility && !Settings::UseNewInput; }'
count = text.count(old)
if count != 1:
    raise SystemExit(f'wheel_setup_ui: expected one universal legacy hook validate, found {count}')
text = text.replace(old, new, 1)
path.write_text(text, encoding='utf-8')
print('isolated legacy hook: wheel_setup_ui DirectInput injector')

print('Legacy wheel hook isolation complete; new and legacy input paths no longer stack detours')
