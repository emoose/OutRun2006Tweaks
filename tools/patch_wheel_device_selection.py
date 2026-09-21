from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match in {path}, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")
    print(f"patched: {label}")


# Keep the configured/default name intact.  Earlier R3 auto-select blanked the
# name before enumeration, which meant a connected gamepad/pedal device could
# become the menu reader simply because it happened to enumerate first.  The
# readers below now try the configured name first and use an FFB-only fallback
# only for the shipped "MOZA" default when Windows exposes the R3 under a
# vendor-less product name.
replace_once(
    "src/hooks_wheel_r3_device_autoselect.hpp",
    '                Settings::WheelMenuR3DeviceName = "";\n',
    '                // Keep the name: strict-first/fallback selection happens in the reader.\n',
    "preserve R3 menu device name",
)
replace_once(
    "src/hooks_wheel_r3_device_autoselect.hpp",
    '                Settings::WheelFFBDeviceName = "";\n',
    '                // Keep the name: strict-first/fallback selection happens in the FFB engine.\n',
    "preserve R3 FFB device name",
)
replace_once(
    "src/hooks_wheel_r3_device_autoselect.hpp",
    '                    "WheelR3DeviceAutoSelect: Windows did not reliably expose the R3 with a MOZA-prefixed DirectInput name; relaxed default device-name filters to auto-select");\n',
    '                    "WheelR3DeviceAutoSelect: MOZA default uses strict-name-first selection with an FFB-only fallback");\n',
    "autoselect diagnostic",
)


# FFB engine: strict configured-name match first; only the stock MOZA default
# may fall back, and even then EnumDevices is restricted to force-feedback
# devices and still rejects known virtual devices in the callback.
replace_once(
    "src/hooks_wheel_ffb.cpp",
'''        struct EnumContext\n        {\n            WheelFFBEngine* self = nullptr;\n            GUID selectedGuid{};\n            std::string selectedName;\n            bool found = false;\n        };\n''',
'''        struct EnumContext\n        {\n            WheelFFBEngine* self = nullptr;\n            GUID selectedGuid{};\n            std::string selectedName;\n            bool found = false;\n            bool ignoreName = false;\n        };\n''',
    "FFB enum fallback state",
)
replace_once(
    "src/hooks_wheel_ffb.cpp",
'''            if (!wanted.empty() &&\n                instanceName.find(wanted) == std::string::npos &&\n                productName.find(wanted) == std::string::npos)\n''',
'''            if (!ctx->ignoreName && !wanted.empty() &&\n                instanceName.find(wanted) == std::string::npos &&\n                productName.find(wanted) == std::string::npos)\n''',
    "FFB strict-first callback",
)
replace_once(
    "src/hooks_wheel_ffb.cpp",
'''            hr = directInput_->EnumDevices(\n                DI8DEVCLASS_GAMECTRL,\n                enum_devices_callback,\n                &ctx,\n                DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n\n            if (FAILED(hr) || !ctx.found)\n            {\n                spdlog::error(\n                    "WheelFFB: no FFB wheel matched DeviceName='{}'",\n                    Settings::WheelFFBDeviceName.get());\n                release_directinput();\n                return false;\n            }\n''',
'''            hr = directInput_->EnumDevices(\n                DI8DEVCLASS_GAMECTRL,\n                enum_devices_callback,\n                &ctx,\n                DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n\n            if (SUCCEEDED(hr) && !ctx.found &&\n                lower_copy(Settings::WheelFFBDeviceName.get().c_str()) == "moza")\n            {\n                ctx.ignoreName = true;\n                spdlog::warn(\n                    "WheelFFB: no literal MOZA DirectInput name; trying first attached non-virtual FFB device");\n                hr = directInput_->EnumDevices(\n                    DI8DEVCLASS_GAMECTRL,\n                    enum_devices_callback,\n                    &ctx,\n                    DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n            }\n\n            if (FAILED(hr) || !ctx.found)\n            {\n                spdlog::error(\n                    "WheelFFB: no FFB wheel matched DeviceName='{}'",\n                    Settings::WheelFFBDeviceName.get());\n                release_directinput();\n                return false;\n            }\n''',
    "FFB strict-first fallback enumeration",
)


# R3 menu D-pad reader.  Always enumerate FFB-capable game controllers so an
# ordinary pad can never steal the R3 fixed-button helper.  Retry without the
# name filter only for the stock MOZA default.  If a once-valid handle becomes
# unreadable, release it so the existing retry loop can recover after replug.
replace_once(
    "src/hooks_wheel_r3_menu_dpad.hpp",
'''        struct EnumContext\n        {\n            GUID guid{};\n            std::string name;\n            bool found = false;\n        };\n''',
'''        struct EnumContext\n        {\n            GUID guid{};\n            std::string name;\n            bool found = false;\n            bool ignoreName = false;\n        };\n''',
    "D-pad enum fallback state",
)
replace_once(
    "src/hooks_wheel_r3_menu_dpad.hpp",
'''            if (!wanted.empty() &&\n                instanceName.find(wanted) == std::string::npos &&\n                productName.find(wanted) == std::string::npos)\n''',
'''            if (!ctx->ignoreName && !wanted.empty() &&\n                instanceName.find(wanted) == std::string::npos &&\n                productName.find(wanted) == std::string::npos)\n''',
    "D-pad strict-first callback",
)
replace_once(
    "src/hooks_wheel_r3_menu_dpad.hpp",
'''            hr = directInput->EnumDevices(\n                DI8DEVCLASS_GAMECTRL,\n                enumDevicesCallback,\n                &ctx,\n                DIEDFL_ATTACHEDONLY);\n            if (FAILED(hr) || !ctx.found)\n            {\n                spdlog::warn(\n                    "WheelMenuR3DirectDPad: no DirectInput device matched '{}'",\n                    Settings::WheelMenuR3DeviceName.get());\n                retryAfter = now + 2000;\n                releaseDevice();\n                return false;\n            }\n''',
'''            hr = directInput->EnumDevices(\n                DI8DEVCLASS_GAMECTRL,\n                enumDevicesCallback,\n                &ctx,\n                DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n            if (SUCCEEDED(hr) && !ctx.found &&\n                r3_lower_copy(Settings::WheelMenuR3DeviceName.get().c_str()) == "moza")\n            {\n                ctx.ignoreName = true;\n                spdlog::warn(\n                    "WheelMenuR3DirectDPad: no literal MOZA name; trying first attached FFB wheel");\n                hr = directInput->EnumDevices(\n                    DI8DEVCLASS_GAMECTRL,\n                    enumDevicesCallback,\n                    &ctx,\n                    DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n            }\n            if (FAILED(hr) || !ctx.found)\n            {\n                spdlog::warn(\n                    "WheelMenuR3DirectDPad: no DirectInput FFB wheel matched '{}'",\n                    Settings::WheelMenuR3DeviceName.get());\n                retryAfter = now + 2000;\n                releaseDevice();\n                return false;\n            }\n''',
    "D-pad FFB-only fallback enumeration",
)
replace_once(
    "src/hooks_wheel_r3_menu_dpad.hpp",
'''                haveDirectState = false;\n                return false;\n            }\n\n            for (const auto& direction : Directions)\n''',
'''                haveDirectState = false;\n                releaseDevice();\n                retryAfter = now + 500;\n                return false;\n            }\n\n            for (const auto& direction : Directions)\n''',
    "D-pad stale-handle recovery",
)


# R3 A/B reader: same strict-first, FFB-only fallback and stale-handle recovery.
replace_once(
    "src/hooks_wheel_r3_menu_ab.hpp",
'''        struct EnumContext\n        {\n            GUID guid{};\n            std::string name;\n            bool found = false;\n        };\n''',
'''        struct EnumContext\n        {\n            GUID guid{};\n            std::string name;\n            bool found = false;\n            bool ignoreName = false;\n        };\n''',
    "A/B enum fallback state",
)
replace_once(
    "src/hooks_wheel_r3_menu_ab.hpp",
'''            if (!wanted.empty() &&\n                instanceName.find(wanted) == std::string::npos &&\n                productName.find(wanted) == std::string::npos)\n''',
'''            if (!ctx->ignoreName && !wanted.empty() &&\n                instanceName.find(wanted) == std::string::npos &&\n                productName.find(wanted) == std::string::npos)\n''',
    "A/B strict-first callback",
)
replace_once(
    "src/hooks_wheel_r3_menu_ab.hpp",
'''            hr = directInput->EnumDevices(\n                DI8DEVCLASS_GAMECTRL,\n                enumDevicesCallback,\n                &ctx,\n                DIEDFL_ATTACHEDONLY);\n            if (FAILED(hr) || !ctx.found)\n            {\n                retryAfter = now + 2000;\n                releaseDevice();\n                return false;\n            }\n''',
'''            hr = directInput->EnumDevices(\n                DI8DEVCLASS_GAMECTRL,\n                enumDevicesCallback,\n                &ctx,\n                DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n            if (SUCCEEDED(hr) && !ctx.found &&\n                lowerCopy(Settings::WheelMenuR3DeviceName.get().c_str()) == "moza")\n            {\n                ctx.ignoreName = true;\n                spdlog::warn(\n                    "WheelMenuR3DirectAB: no literal MOZA name; trying first attached FFB wheel");\n                hr = directInput->EnumDevices(\n                    DI8DEVCLASS_GAMECTRL,\n                    enumDevicesCallback,\n                    &ctx,\n                    DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);\n            }\n            if (FAILED(hr) || !ctx.found)\n            {\n                retryAfter = now + 2000;\n                releaseDevice();\n                return false;\n            }\n''',
    "A/B FFB-only fallback enumeration",
)
replace_once(
    "src/hooks_wheel_r3_menu_ab.hpp",
'''                haveDirectState = false;\n                return false;\n            }\n\n            a = buttonHeld(state, int(Settings::WheelMenuR3AButton));\n''',
'''                haveDirectState = false;\n                releaseDevice();\n                retryAfter = now + 500;\n                return false;\n            }\n\n            a = buttonHeld(state, int(Settings::WheelMenuR3AButton));\n''',
    "A/B stale-handle recovery",
)


# A 20% gamepad-oriented steering deadzone is enormous on the user's 270-degree
# R3 (about +/-27 degrees around centre).  This wheel branch should start at zero
# and let F11 add 1-2% only if the hardware actually jitters.
replace_once(
    "src/hooks_input.cpp",
'''\tSetting<float> SteeringDeadZone{ "Controls", "SteeringDeadZone", 0.2f,\n\t\t"Allows overriding the steering deadzone. Game default is 0.2 / 20%.", Range<float>{ 0.f, 1.f } };\n''',
'''\tSetting<float> SteeringDeadZone{ "Controls", "SteeringDeadZone", 0.0f,\n\t\t"Allows overriding the steering deadzone. Original game default is 0.2 / 20%; wheel branch default is 0%.", Range<float>{ 0.f, 1.f } };\n''',
    "wheel deadzone source default",
)
replace_once(
    "OutRun2006Tweaks.ini",
'''# Allows overriding the steering deadzone\n# Game default is 0.2 / 20%\nSteeringDeadZone = 0.2\n''',
'''# Allows overriding the steering deadzone\n# Original game default is 0.2 / 20%; wheel branch defaults to zero.\nSteeringDeadZone = 0.0\n''',
    "wheel deadzone shipped default",
)

print("Applied strict-first wheel selection, FFB-only R3 fallback, reader recovery, and 0% wheel deadzone")
