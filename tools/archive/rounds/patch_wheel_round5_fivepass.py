from pathlib import Path

ffb_path = Path('src/hooks_wheel_ffb.cpp')
input_path = Path('src/input_manager.hpp')
ffb = ffb_path.read_text(encoding='utf-8')
ims = input_path.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str, expected: int = 1) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(f'round5 {label}: expected {expected} match(es), got {count}')
    print(f'ROUND5 patched: {label}')
    return text.replace(old, new, expected)


def repair_or_require(text: str, broken: str, fixed: str, label: str) -> str:
    if broken in text:
        print(f'ROUND5 repaired: {label}')
        return text.replace(broken, fixed, 1)
    if fixed not in text:
        raise SystemExit(f'round5 {label}: neither broken nor fixed form found')
    print(f'ROUND5 already fixed: {label}')
    return text


# ---------------------------------------------------------------------------
# Build-blocker found by the previous five-pass run.
# round4 replaced an inner deadline expression but accidentally consumed the
# closing ')' belonging to the outer if statement. Repair the three generated
# conditions before doing the next source-level review.
# ---------------------------------------------------------------------------
for deadline, label in [
    ('recreateHoldoffUntil_', 'periodic holdoff outer parenthesis'),
    ('springRecreateHoldoffUntil_', 'spring holdoff outer parenthesis'),
    ('damperRecreateHoldoffUntil_', 'damper holdoff outer parenthesis'),
]:
    broken = f'tick_reached(GetTickCount(), {deadline})\n            {{'
    fixed = f'tick_reached(GetTickCount(), {deadline}))\n            {{'
    ffb = repair_or_require(ffb, broken, fixed, label)


# ---------------------------------------------------------------------------
# PASS 1 - Initialization failures must participate in sibling-interface
# fallback, and GetCapabilities() itself must be checked rather than treating
# an HRESULT failure as a device that simply lacks FFB capability.
# ---------------------------------------------------------------------------
ffb = rep(
    ffb,
'''            hr = directInput_->CreateDevice(selectedGuid_, &device_, nullptr);\n            if (FAILED(hr) || !device_)\n            {\n                spdlog::error(\n                    "WheelFFB: CreateDevice('{}') failed (0x{:08X})",\n                    selectedName_, (unsigned)hr);\n                release_directinput();\n                return false;\n            }\n\n            DIDEVCAPS caps{};\n            caps.dwSize = sizeof(caps);\n            device_->GetCapabilities(&caps);\n\n            if ((caps.dwFlags & DIDC_FORCEFEEDBACK) == 0)\n            {\n                spdlog::error("WheelFFB: '{}' does not report force-feedback capability", selectedName_);\n                release_device();\n                release_directinput();\n                return false;\n            }\n''',
'''            hr = directInput_->CreateDevice(selectedGuid_, &device_, nullptr);\n            if (FAILED(hr) || !device_)\n            {\n                const HRESULT failHr = FAILED(hr) ? hr : E_FAIL;\n                mark_selected_interface_failed("CreateDevice", failHr);\n                spdlog::error(\n                    "WheelFFB: CreateDevice('{}') failed (0x{:08X})",\n                    selectedName_, (unsigned)failHr);\n                release_directinput();\n                return false;\n            }\n\n            DIDEVCAPS caps{};\n            caps.dwSize = sizeof(caps);\n            hr = device_->GetCapabilities(&caps);\n            if (FAILED(hr))\n            {\n                mark_selected_interface_failed("GetCapabilities", hr);\n                spdlog::error(\n                    "WheelFFB: GetCapabilities('{}') failed (0x{:08X})",\n                    selectedName_, (unsigned)hr);\n                release_device();\n                release_directinput();\n                return false;\n            }\n\n            if ((caps.dwFlags & DIDC_FORCEFEEDBACK) == 0)\n            {\n                mark_selected_interface_failed("force-feedback capability", E_NOINTERFACE);\n                spdlog::error("WheelFFB: '{}' does not report force-feedback capability", selectedName_);\n                release_device();\n                release_directinput();\n                return false;\n            }\n''',
    'capability HRESULT and CreateDevice failover')

for old, new, label in [
    (
'''            if (FAILED(hr))\n            {\n                spdlog::error("WheelFFB: SetDataFormat failed (0x{:08X})", (unsigned)hr);\n                release_device();\n''',
'''            if (FAILED(hr))\n            {\n                mark_selected_interface_failed("SetDataFormat", hr);\n                spdlog::error("WheelFFB: SetDataFormat failed (0x{:08X})", (unsigned)hr);\n                release_device();\n''',
        'SetDataFormat sibling failover'),
    (
'''            if (FAILED(hr))\n            {\n                spdlog::error(\n                    "WheelFFB: SetCooperativeLevel(EXCLUSIVE|BACKGROUND) failed (0x{:08X})",\n                    (unsigned)hr);\n                release_device();\n''',
'''            if (FAILED(hr))\n            {\n                mark_selected_interface_failed("SetCooperativeLevel", hr);\n                spdlog::error(\n                    "WheelFFB: SetCooperativeLevel(EXCLUSIVE|BACKGROUND) failed (0x{:08X})",\n                    (unsigned)hr);\n                release_device();\n''',
        'SetCooperativeLevel sibling failover'),
    (
'''            if (FAILED(hr))\n            {\n                spdlog::error("WheelFFB: Acquire('{}') failed (0x{:08X})", selectedName_, (unsigned)hr);\n                release_device();\n''',
'''            if (FAILED(hr))\n            {\n                mark_selected_interface_failed("Acquire", hr);\n                spdlog::error("WheelFFB: Acquire('{}') failed (0x{:08X})", selectedName_, (unsigned)hr);\n                release_device();\n''',
        'Acquire sibling failover'),
    (
'''            if (FAILED(actuatorOnHr))\n            {\n                spdlog::error(\n                    "WheelFFB: SETACTUATORSON during initialization failed (0x{:08X})",\n                    (unsigned)actuatorOnHr);\n                release_device();\n''',
'''            if (FAILED(actuatorOnHr))\n            {\n                mark_selected_interface_failed("initial SETACTUATORSON", actuatorOnHr);\n                spdlog::error(\n                    "WheelFFB: SETACTUATORSON during initialization failed (0x{:08X})",\n                    (unsigned)actuatorOnHr);\n                release_device();\n''',
        'SETACTUATORSON sibling failover'),
]:
    ffb = rep(ffb, old, new, label)


# ---------------------------------------------------------------------------
# PASS 2 - Track ownership of the driver's autocenter property. We disable it
# for our custom spring, so every partial-init / device-release path must make a
# best effort to restore it instead of leaving the wheel driver altered.
# ---------------------------------------------------------------------------
ffb = rep(
    ffb,
'''            hr = device_->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);\n            if (FAILED(hr))\n                spdlog::warn("WheelFFB: disabling driver autocenter failed (0x{:08X})", (unsigned)hr);\n''',
'''            hr = device_->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);\n            if (FAILED(hr))\n                spdlog::warn("WheelFFB: disabling driver autocenter failed (0x{:08X})", (unsigned)hr);\n            else\n                driverAutocenterDisabled_ = true;\n''',
    'track driver autocenter disable')

# Replace the duplicate reinitialize autocenter block with the shared helper.
ffb = rep(
    ffb,
'''            if (device_)\n            {\n                __try\n                {\n                    device_->Unacquire();\n                    deviceAcquired_ = false;\n\n                    DIPROPDWORD autocenter{};\n                    autocenter.diph.dwSize = sizeof(autocenter);\n                    autocenter.diph.dwHeaderSize = sizeof(DIPROPHEADER);\n                    autocenter.diph.dwObj = 0;\n                    autocenter.diph.dwHow = DIPH_DEVICE;\n                    autocenter.dwData = DIPROPAUTOCENTER_ON;\n                    device_->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);\n                }\n                __except (EXCEPTION_EXECUTE_HANDLER)\n                {\n                    deviceAcquired_ = false;\n                }\n            }\n\n            release_device();\n''',
'''            restore_driver_autocenter("device reinitialize");\n            release_device();\n''',
    'shared reinitialize autocenter restore')

ffb = rep(
    ffb,
'''        void release_device()\n        {\n            if (!device_)\n                return;\n            __try\n            {\n                device_->Unacquire();\n                deviceAcquired_ = false;\n                device_->Release();\n            }\n            __except (EXCEPTION_EXECUTE_HANDLER)\n            {\n                spdlog::warn(\n                    "WheelFFB: exception releasing DirectInput device (0x{:X})",\n                    GetExceptionCode());\n            }\n            device_ = nullptr;\n        }\n''',
'''        void restore_driver_autocenter(const char* where)\n        {\n            if (!device_ || !driverAutocenterDisabled_)\n                return;\n\n            deviceAcquired_ = false;\n            __try\n            {\n                device_->Unacquire();\n\n                DIPROPDWORD autocenter{};\n                autocenter.diph.dwSize = sizeof(autocenter);\n                autocenter.diph.dwHeaderSize = sizeof(DIPROPHEADER);\n                autocenter.diph.dwObj = 0;\n                autocenter.diph.dwHow = DIPH_DEVICE;\n                autocenter.dwData = DIPROPAUTOCENTER_ON;\n                const HRESULT restoreHr =\n                    device_->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);\n                if (FAILED(restoreHr))\n                {\n                    spdlog::warn(\n                        "WheelFFB: autocenter restore during {} failed (0x{:08X})",\n                        where, (unsigned)restoreHr);\n                }\n                else\n                {\n                    driverAutocenterDisabled_ = false;\n                }\n            }\n            __except (EXCEPTION_EXECUTE_HANDLER)\n            {\n                spdlog::warn(\n                    "WheelFFB: exception restoring autocenter during {} (0x{:X})",\n                    where, GetExceptionCode());\n            }\n        }\n\n        void release_device()\n        {\n            if (!device_)\n            {\n                deviceAcquired_ = false;\n                driverAutocenterDisabled_ = false;\n                return;\n            }\n\n            // Clear logical acquisition state before touching a possibly stale\n            // COM object. Even a driver exception must not leave a phantom\n            // acquired device behind in the engine state.\n            deviceAcquired_ = false;\n            restore_driver_autocenter("device release");\n\n            IDirectInputDevice8A* staleDevice = device_;\n            device_ = nullptr;\n            driverAutocenterDisabled_ = false;\n            __try\n            {\n                staleDevice->Unacquire();\n                staleDevice->Release();\n            }\n            __except (EXCEPTION_EXECUTE_HANDLER)\n            {\n                spdlog::warn(\n                    "WheelFFB: exception releasing DirectInput device (0x{:X})",\n                    GetExceptionCode());\n            }\n        }\n''',
    'autocenter-safe fail-state-first device release')

ffb = rep(
    ffb,
    '        bool deviceAcquired_ = false;\n',
    '        bool deviceAcquired_ = false;\n        bool driverAutocenterDisabled_ = false;\n',
    'driver autocenter ownership state')

ffb = rep(
    ffb,
'''            hr = device_->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);\n            spdlog::info("WheelFFB: PanicStop autocenter restore => 0x{:08X}", (unsigned)hr);\n''',
'''            hr = device_->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);\n            if (SUCCEEDED(hr))\n                driverAutocenterDisabled_ = false;\n            spdlog::info("WheelFFB: PanicStop autocenter restore => 0x{:08X}", (unsigned)hr);\n''',
    'PanicStop clears autocenter ownership on success')


# ---------------------------------------------------------------------------
# PASS 3 - Because compatibility uses EXCLUSIVE|BACKGROUND, DirectInput itself
# will allow Acquire while the application is unfocused. Put an explicit
# foreground check both before and immediately after Acquire to close the focus
# transition race without changing wheel-driver cooperative-level behaviour.
# ---------------------------------------------------------------------------
ffb = rep(
    ffb,
'''        bool reacquire_after_input_loss(const char* where, HRESULT originalHr)\n        {\n            if (!device_)\n            {\n                note_device_failure(where, originalHr);\n                return false;\n            }\n\n            deviceAcquired_ = false;\n            const HRESULT acquireHr = device_->Acquire();\n''',
'''        bool reacquire_after_input_loss(const char* where, HRESULT originalHr)\n        {\n            if (!device_)\n            {\n                note_device_failure(where, originalHr);\n                return false;\n            }\n\n            if (!appActive_ || !gameHwnd_ || GetForegroundWindow() != gameHwnd_)\n            {\n                deviceAcquired_ = false;\n                return false;\n            }\n\n            deviceAcquired_ = false;\n            const HRESULT acquireHr = device_->Acquire();\n''',
    'foreground-safe input-loss reacquire')

ffb = rep(
    ffb,
'''            if (SUCCEEDED(acquireHr) || acquireHr == S_FALSE)\n            {\n                deviceAcquired_ = true;\n                const HRESULT actuatorHr =\n''',
'''            if (SUCCEEDED(acquireHr) || acquireHr == S_FALSE)\n            {\n                if (!appActive_ || GetForegroundWindow() != gameHwnd_)\n                {\n                    device_->Unacquire();\n                    deviceAcquired_ = false;\n                    return false;\n                }\n\n                deviceAcquired_ = true;\n                const HRESULT actuatorHr =\n''',
    'post-Acquire foreground race guard')


# ---------------------------------------------------------------------------
# PASS 4 - Make DirectInput root teardown state-first and SEH-protected, just as
# device/effect teardown already is. A stale driver COM object must not leave a
# non-null engine pointer after an exception during process/device recovery.
# ---------------------------------------------------------------------------
ffb = rep(
    ffb,
'''        void release_directinput()\n        {\n            if (!directInput_)\n                return;\n            directInput_->Release();\n            directInput_ = nullptr;\n        }\n''',
'''        void release_directinput()\n        {\n            if (!directInput_)\n                return;\n\n            IDirectInput8A* staleDirectInput = directInput_;\n            directInput_ = nullptr;\n            __try\n            {\n                staleDirectInput->Release();\n            }\n            __except (EXCEPTION_EXECUTE_HANDLER)\n            {\n                spdlog::warn(\n                    "WheelFFB: exception releasing DirectInput root object (0x{:X})",\n                    GetExceptionCode());\n            }\n        }\n''',
    'exception-safe DirectInput root release')


# ---------------------------------------------------------------------------
# PASS 5 - Multi-device hotplug identity stability. Counting same-GUID devices
# can generate duplicate occurrence IDs after one identical device is removed.
# Allocate the lowest unused occurrence instead. Also preserve the selected
# primary gamepad when an earlier vector element disappears.
# ---------------------------------------------------------------------------
if '#include <algorithm>' not in ims:
    ims = ims.replace('#include <unordered_map>\n', '#include <unordered_map>\n#include <algorithm>\n', 1)
    print('ROUND5 patched: explicit algorithm include')

ims = rep(
    ims,
'''\t\tconst std::string guid(guidText);\n\t\tconst int occurrence = int(std::count_if(devices.begin(), devices.end(), [&guid](const InputDevice& device)\n\t\t\t{\n\t\t\t\treturn device.guid == guid;\n\t\t\t}));\n''',
'''\t\tconst std::string guid(guidText);\n\t\tint occurrence = 0;\n\t\twhile (std::any_of(devices.begin(), devices.end(), [&guid, occurrence](const InputDevice& device)\n\t\t\t{\n\t\t\t\treturn device.guid == guid && device.occurrence == occurrence;\n\t\t\t}))\n\t\t{\n\t\t\t++occurrence;\n\t\t}\n''',
    'unique same-GUID occurrence after hotplug')

ims = rep(
    ims,
'''\t\tif (it != controllers.end())\n\t\t{\n\t\t\tGame::CurrentPadType = Game::GamepadType::PC;\n\n\t\t\tSDL_CloseGamepad(*it);\n\t\t\tcontrollers.erase(it);\n\n\t\t\tspdlog::debug(__FUNCTION__ "(instance {}): removed instance", instanceId);\n\n\t\t\tif (primaryControllerIndex >= controllers.size())\n\t\t\t\tsetPrimaryGamepad(controllers.empty() ? -1 : 0);\n\t\t}\n''',
'''\t\tif (it != controllers.end())\n\t\t{\n\t\t\tconst int removedIndex = int(std::distance(controllers.begin(), it));\n\t\t\tconst bool removedPrimary = (removedIndex == primaryControllerIndex);\n\n\t\t\tSDL_CloseGamepad(*it);\n\t\t\tcontrollers.erase(it);\n\n\t\t\tspdlog::debug(__FUNCTION__ "(instance {}): removed instance", instanceId);\n\n\t\t\tif (controllers.empty())\n\t\t\t{\n\t\t\t\tGame::CurrentPadType = Game::GamepadType::PC;\n\t\t\t\tsetPrimaryGamepad(-1);\n\t\t\t}\n\t\t\telse if (removedPrimary)\n\t\t\t{\n\t\t\t\tsetPrimaryGamepad(std::min(removedIndex, int(controllers.size()) - 1));\n\t\t\t}\n\t\t\telse if (primaryControllerIndex > removedIndex)\n\t\t\t{\n\t\t\t\t--primaryControllerIndex;\n\t\t\t\tif (auto* pad = getPrimaryGamepad())\n\t\t\t\t\tsetupGamepad(pad);\n\t\t\t}\n\t\t}\n''',
    'stable primary controller index on removal')

ffb_path.write_text(ffb, encoding='utf-8')
input_path.write_text(ims, encoding='utf-8')
print('Applied round-5 five-pass lifecycle/hotplug hardening')
