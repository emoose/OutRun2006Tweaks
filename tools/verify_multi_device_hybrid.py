from pathlib import Path


def text(path: str) -> str:
    return Path(path).read_text(encoding='utf-8')


def require(path: str, needle: str, label: str) -> None:
    data = text(path)
    if needle not in data:
        raise SystemExit(f'FAIL [{label}]: {needle!r} missing from {path}')
    print(f'PASS [{label}]')


def forbid(path: str, needle: str, label: str) -> None:
    data = text(path)
    if needle in data:
        raise SystemExit(f'FAIL [{label}]: stale {needle!r} present in {path}')
    print(f'PASS [{label}]')


# 1. Raw multi-device binding kinds exist.
require('src/input_manager.hpp', 'JoyButton, JoyAxis, JoyHat', 'raw joystick binding kinds')
# 2. Wheel/pedal calibration survives in bindings.
require('src/input_manager.hpp', 'axisMinimum', 'axis calibration minimum')
require('src/input_manager.hpp', 'axisRest', 'axis calibration rest')
require('src/input_manager.hpp', 'axisMaximum', 'axis calibration maximum')
# 3. Physical identity is stronger than display name alone.
require('src/input_manager.hpp', 'deviceSerial', 'serial identity')
require('src/input_manager.hpp', 'devicePath', 'path identity')
require('src/input_manager.hpp', 'deviceVendor', 'USB vendor identity')
# 4. Every SDL joystick is enumerated, not only mapped gamepads.
require('src/input_manager.cpp', 'SDL_GetJoysticks', 'all-device startup enumeration')
require('src/input_manager.cpp', 'onJoystickAdded', 'raw joystick registration')
# 5. Hot-plug is part of the new path.
require('src/input_manager.hpp', 'SDL_EVENT_JOYSTICK_ADDED', 'hotplug add event')
require('src/input_manager.hpp', 'SDL_EVENT_JOYSTICK_REMOVED', 'hotplug remove event')
# 6. Automatic backend can see a Windows FFB wheel before SDL starts.
require('src/input_manager.cpp', 'has_attached_ffb_wheel()', 'DirectInput pre-SDL FFB probe')
# 7. The imported simple FFB backend must not compete with our COM engine.
forbid('src/input_manager.cpp', 'WheelForceFeedback::', 'single FFB owner in input manager')
forbid('src/overlay/input_bindings_ui.cpp', 'WheelForceFeedback::', 'single FFB owner in UI')
# 8. Quick Setup/calibration remains exposed in the in-game binding UI.
require('src/overlay/input_bindings_ui.cpp', 'Quick Setup', 'quick setup UI')
require('src/overlay/input_bindings_ui.cpp', 'Save calibration', 'calibration UI')
require('src/overlay/input_bindings_ui.cpp', 'Multi-device input architecture adapted from hyp36rmax (MIT)', 'source attribution')
# 9. Advanced FFB controls are combined with the multi-device UI.
require('src/overlay/input_bindings_ui.cpp', 'Advanced DirectInput COM FFB', 'advanced FFB binding tab')
require('src/overlay/input_bindings_ui.cpp', 'Hardware GUID_Damper', 'hardware damper UI')
# 10. New input is preferred, while exact GUID + legacy fallback remain available.
forbid('src/dllmain.cpp', 'Settings::UseNewInput = false;', 'no forced legacy startup')
require('src/dllmain.cpp', 'multi-device SDL raw input enabled', 'new-input startup diagnostic')
require('src/hooks_wheel_ffb.cpp', 'WheelFFBDeviceGuid', 'exact FFB GUID setting')
require('src/overlay/wheel_setup_ui.cpp', 'WheelUniversalDeviceGuid', 'exact setup GUID setting')

print('Multi-device hybrid 10-pass verification complete')
