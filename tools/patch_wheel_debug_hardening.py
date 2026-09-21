from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match in {path}, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# R3 testing with the native separate accelerator/brake rows showed that both
# pedals are correct without the legacy compatibility inversions/split hack.
# Keep those workarounds opt-in for older combined-axis wheels instead of
# enabling them on a fresh universal-wheel profile.
replace_once(
    "src/hooks_input.cpp",
    'Setting<bool> WheelAccelerationInvert{ "Controls", "WheelAccelerationInvert", true,',
    'Setting<bool> WheelAccelerationInvert{ "Controls", "WheelAccelerationInvert", false,',
    "acceleration invert default",
)

# The shipped INI explicitly overrides the C++ default, so patch it too. Without
# this, a clean install would still boot with the R3 accelerator backwards.
replace_once(
    "OutRun2006Tweaks.ini",
    "WheelAccelerationInvert = true\n",
    "WheelAccelerationInvert = false\n",
    "shipped acceleration invert default",
)

replace_once(
    "src/hooks_wheel_input_compat_v2.hpp",
    '        "Controls", "WheelPedalSplitFix", true,\n',
    '        "Controls", "WheelPedalSplitFix", false,\n',
    "pedal split default",
)

# A saved/imported universal profile can become enabled without the user
# clicking the checkbox during this run.  Make the old R3 fixed-button readers
# self-disable whenever the universal profile is active, so the two input paths
# can never inject duplicate/conflicting menu actions.
replace_once(
    "src/hooks_wheel_r3_menu_dpad.hpp",
    'namespace Settings\n{\n    Setting<bool> WheelMenuR3DirectDPad{',
    'namespace Settings\n{\n    extern Setting<bool> WheelUniversalSetupEnable;\n\n    Setting<bool> WheelMenuR3DirectDPad{',
    "D-pad universal extern",
)
replace_once(
    "src/hooks_wheel_r3_menu_dpad.hpp",
    '                !Settings::UseNewInput &&\n                Settings::WheelMenuR3DirectDPad &&',
    '                !Settings::UseNewInput &&\n                !Settings::WheelUniversalSetupEnable &&\n                Settings::WheelMenuR3DirectDPad &&',
    "D-pad universal exclusion",
)

replace_once(
    "src/hooks_wheel_r3_menu_ab.hpp",
    'namespace Settings\n{\n    Setting<bool> WheelMenuR3DirectAB{',
    'namespace Settings\n{\n    extern Setting<bool> WheelUniversalSetupEnable;\n\n    Setting<bool> WheelMenuR3DirectAB{',
    "A/B universal extern",
)
replace_once(
    "src/hooks_wheel_r3_menu_ab.hpp",
    '                !Settings::UseNewInput &&\n                Settings::WheelMenuR3DirectAB &&',
    '                !Settings::UseNewInput &&\n                !Settings::WheelUniversalSetupEnable &&\n                Settings::WheelMenuR3DirectAB &&',
    "A/B universal exclusion",
)

# DirectInput unacquire/reacquire (menus/F11 -> gameplay) stops effects on many
# drivers even when the effect object remains valid.  The mature DirectInput
# FFB implementations start updated effects explicitly.  Force DIEP_START on
# spring/damper/periodic SetParameters calls so returning from a menu cannot
# leave an apparently-valid but stopped effect behind.
replace_once(
    "src/hooks_wheel_ffb.cpp",
    '            springStrategy_ = -1;\n\n            spdlog::info(\n                "WheelFFB: GUID_Spring created',
    '            springStrategy_ = 1; // Always include DIEP_START on dynamic updates.\n\n            spdlog::info(\n                "WheelFFB: GUID_Spring created',
    "spring DIEP_START strategy",
)
replace_once(
    "src/hooks_wheel_ffb.cpp",
    '            prevDamperCoefficient_ = 0;\n            damperStrategy_ = -1;\n            spdlog::info("WheelFFB: GUID_Damper created',
    '            prevDamperCoefficient_ = 0;\n            damperStrategy_ = 1; // Always include DIEP_START after menu reacquire.\n            spdlog::info("WheelFFB: GUID_Damper created',
    "damper DIEP_START strategy",
)
replace_once(
    "src/hooks_wheel_ffb.cpp",
    '        int periodicStrategy_ = -1;\n',
    '        int periodicStrategy_ = 1; // Explicitly restart sine effects on every update.\n',
    "periodic DIEP_START strategy",
)

print("Applied wheel runtime hardening: pedal defaults/INI, universal-menu exclusion, persistent DIEP_START")
