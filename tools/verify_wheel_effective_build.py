from pathlib import Path
import hashlib


def read(path: str) -> str:
    return Path(path).read_text(encoding="utf-8")


def require(path: str, needle: str, label: str) -> None:
    text = read(path)
    if needle not in text:
        raise SystemExit(f"VERIFY FAILED [{label}]: {needle!r} not found in {path}")
    print(f"VERIFY OK [{label}]")


def forbid(path: str, needle: str, label: str) -> None:
    text = read(path)
    if needle in text:
        raise SystemExit(f"VERIFY FAILED [{label}]: stale {needle!r} still present in {path}")
    print(f"VERIFY OK [{label}]")


def digest(path: str) -> None:
    data = Path(path).read_bytes()
    print(f"SHA256 {path} {hashlib.sha256(data).hexdigest()}")


# Effective FFB source after all build-time patches.
require("src/hooks_wheel_ffb.cpp", 'Setting<bool> WheelFFBUseHardwareDamper{', "GUID_Damper setting")
require("src/hooks_wheel_ffb.cpp", 'GUID_Damper, &effect, &damperEffect_', "GUID_Damper creation")
require("src/hooks_wheel_ffb.cpp", 'springStrategy_ = 1;', "spring persistent DIEP_START")
require("src/hooks_wheel_ffb.cpp", 'damperStrategy_ = 1;', "damper persistent DIEP_START")
require("src/hooks_wheel_ffb.cpp", 'int periodicStrategy_ = 1;', "periodic persistent DIEP_START")
require("src/hooks_wheel_ffb.cpp", 'bool deviceAcquired_ = false;', "DirectInput acquisition state")
require("src/hooks_wheel_ffb.cpp", 'if (device_ && !deviceAcquired_)', "menu/gameplay reacquire")
require("src/hooks_wheel_ffb.cpp", 'textureRoughness', "road baseline filtering")
require("src/hooks_wheel_ffb.cpp", 'bool ignoreName = false;', "strict-first FFB selection state")
require("src/hooks_wheel_ffb.cpp", 'no literal MOZA DirectInput name; trying first attached non-virtual FFB device', "R3 FFB fallback")
forbid("src/hooks_wheel_ffb.cpp", 'Keep the current software damper for this first comparison build.', "no stale software-damper block")

# Live DD-wheel safety and F11 tuning behavior.
require("src/hooks_wheel_ffb.cpp", 'disabled live; all effects zeroed immediately', "immediate live FFB disable")
require("src/hooks_wheel_ffb.cpp", 'void apply_live_effect_gain()', "live effect-gain helper")
require("src/hooks_wheel_ffb.cpp", 'effect->SetParameters(&params, DIEP_GAIN)', "DirectInput DIEP_GAIN update")
require("src/hooks_wheel_ffb.cpp", 'if (hr == DIERR_EFFECTPLAYING)', "gain update handles non-dynamic drivers")
require("src/hooks_wheel_ffb.cpp", 'DIEP_GAIN | DIEP_START', "gain update explicit restart fallback")
require("src/hooks_wheel_ffb.cpp", 'lastEffectGain_ = configured_effect_gain();', "initial gain cache")
require("src/hooks_wheel_ffb.cpp", 'hardware periodic effects disabled live; using ConstantForce fallback', "live periodic backend disable")
require("src/hooks_wheel_ffb.cpp", 'roadState_.lastMagnitude != 0', "watchdog covers road periodic")
require("src/hooks_wheel_ffb.cpp", 'slipState_.lastMagnitude != 0', "watchdog covers tire periodic")
require("src/hooks_wheel_ffb.cpp", 'DWORD lastEffectGain_ = 0xFFFFFFFFu;', "gain recovery sentinel")

# Second-round lifecycle/safety review.
require("src/hooks_wheel_ffb.cpp", 'std::fill_n(speedHistory_, SpeedHistoryCount, 0.0f);', "stale speed history reset")
require("src/hooks_wheel_ffb.cpp", 'lateralHistoryIndex_ = 0;', "stale lateral history reset")
require("src/hooks_wheel_ffb.cpp", 'prevCollisionFlags_ = 0;', "collision edge state reset")
require("src/hooks_wheel_ffb.cpp", 'if (!appActive_)', "background FFB reacquire gate")
require("src/hooks_wheel_ffb.cpp", 'self->device_->Unacquire();', "focus-loss exclusive release")
require("src/hooks_wheel_ffb.cpp", 'HWND subclassHwnd_ = nullptr;', "idempotent window subclass state")
require("src/hooks_wheel_ffb.cpp", 'if (!exitProcessHook_)', "idempotent ExitProcess hook")
require("src/hooks_wheel_ffb.cpp", 'DWORD nextGainRetryTick_ = 0;', "live gain failure backoff state")
require("src/hooks_wheel_ffb.cpp", 'nextGainRetryTick_ = GetTickCount() + 250;', "live gain retry backoff")
require("src/hooks_wheel_ffb.cpp", 'static_cast<float>(Settings::WheelFFBRoadTexture) / 0.20f', "road-detail scales splash")

# R3-specific legacy defaults proven by physical testing.
require("src/hooks_input.cpp", 'Setting<float> SteeringDeadZone{ "Controls", "SteeringDeadZone", 0.0f,', "0-percent wheel deadzone source default")
require("src/hooks_input.cpp", 'Setting<bool> WheelAccelerationInvert{ "Controls", "WheelAccelerationInvert", false,', "R3 accelerator default")
require("src/hooks_wheel_input_compat_v2.hpp", '"Controls", "WheelPedalSplitFix", false,', "separate-pedal default")
require("OutRun2006Tweaks.ini", 'SteeringDeadZone = 0.0', "shipped 0-percent deadzone")
require("OutRun2006Tweaks.ini", 'WheelAccelerationInvert = false', "shipped accelerator INI")

# Fixed R3 menu helpers must follow the same wheel, not the first random pad,
# and must recover after a stale/replugged DirectInput handle.
require("src/hooks_wheel_r3_menu_dpad.hpp", 'DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK', "D-pad FFB-only enumeration")
require("src/hooks_wheel_r3_menu_dpad.hpp", 'no literal MOZA name; trying first attached FFB wheel', "D-pad R3 fallback")
require("src/hooks_wheel_r3_menu_dpad.hpp", 'retryAfter = now + 500;', "D-pad stale-handle recovery")
require("src/hooks_wheel_r3_menu_ab.hpp", 'DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK', "A/B FFB-only enumeration")
require("src/hooks_wheel_r3_menu_ab.hpp", 'no literal MOZA name; trying first attached FFB wheel', "A/B R3 fallback")
require("src/hooks_wheel_r3_menu_ab.hpp", 'retryAfter = now + 500;', "A/B stale-handle recovery")
forbid("src/hooks_wheel_r3_device_autoselect.hpp", 'Settings::WheelFFBDeviceName = "";', "do not erase configured FFB name")
forbid("src/hooks_wheel_r3_device_autoselect.hpp", 'Settings::WheelMenuR3DeviceName = "";', "do not erase configured menu name")

# Universal profile must own menu input when enabled and never silently jump to
# another cached device or assume its filtered combo index equals a game slot.
require("src/hooks_wheel_r3_menu_dpad.hpp", '!Settings::WheelUniversalSetupEnable &&', "universal excludes fixed R3 D-pad helper")
require("src/hooks_wheel_r3_menu_ab.hpp", '!Settings::WheelUniversalSetupEnable &&', "universal excludes fixed R3 A/B helper")
require("src/overlay/wheel_setup_ui.cpp", 'bool select_by_identity(const std::string& wantedGuid, const std::string& wantedName)', "GUID-aware universal selection")
require("src/overlay/wheel_setup_ui.cpp", 'if (index < 0 && guid.empty() && wantedName.empty() && !devices_.empty()) index=0;', "strict saved universal device")
require("src/overlay/wheel_setup_ui.cpp", 'release_device();\n                devices_.clear();', "universal stale-device re-enumeration")
require("src/overlay/wheel_setup_ui.cpp", 'is_virtual_name(product) || is_virtual_name(instanceName)', "virtual instance filtering")
require("src/overlay/wheel_setup_ui.cpp", 'static int regular_device_count()', "legacy slot count")
require("src/overlay/wheel_setup_ui.cpp", 'ImGui::BeginCombo("Legacy input slot"', "explicit legacy input slot")
forbid("src/overlay/wheel_setup_ui.cpp", 'WheelUniversalLegacyDeviceIndex = std::clamp(index, 0, 2);', "do not equate filtered device index with game slot")
require("src/overlay/wheel_setup_ui.cpp", 'pure_universal_menu_query(switches)', "universal menu query scope")
require("src/overlay/wheel_setup_ui.cpp", '(menuHeld_ & switches) == switches', "universal SwitchNow exact mask")
require("src/overlay/wheel_setup_ui.cpp", '(menuPressed_ & switches) == switches', "universal SwitchOn exact mask")
require("src/overlay/wheel_setup_ui.cpp", 'const int regularCount = std::max(device_count() - 1, 0);', "live legacy-slot clamp")
require("src/overlay/wheel_setup_ui.cpp", 'Settings::WheelUniversalLegacyDeviceIndex = currentSlot;', "UI slot clamp persistence")
require("src/overlay/wheel_setup_ui.cpp", 'ImGui::SliderInt("Steering Deadzone", &deadzonePercent, 0, 20, "%d%%")', "0-percent wheel deadzone UI")
require("src/overlay/wheel_setup_ui.cpp", 'ImGui::SeparatorText("Simulation FFB")', "combined simulation FFB panel")

for path in (
    "src/hooks_wheel_ffb.cpp",
    "src/hooks_wheel_input_compat_v2.hpp",
    "src/hooks_wheel_r3_menu_dpad.hpp",
    "src/hooks_wheel_r3_menu_ab.hpp",
    "src/hooks_wheel_r3_device_autoselect.hpp",
    "src/overlay/wheel_setup_ui.cpp",
    "OutRun2006Tweaks.ini",
):
    digest(path)

print("Wheel effective-build verification passed")
