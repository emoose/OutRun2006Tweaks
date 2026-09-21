from pathlib import Path

path = Path("src/hooks_wheel_ffb.cpp")
text = path.read_text(encoding="utf-8")


def replace_once(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    text = text.replace(old, new, 1)
    print(f"patched: {label}")


# Runtime disable must release torque immediately.  Previously update() returned
# before touching the effects and depended on the 250 ms watchdog.  A DD wheel
# should never keep stale torque merely because a live setting was toggled off.
replace_once(
'''        void update(EVWORK_CAR* car)\n        {\n            if (!Settings::WheelFFBEnable || !car || panicStopped_)\n                return;\n\n            lastUpdateTick_ = GetTickCount();\n''',
'''        void update(EVWORK_CAR* car)\n        {\n            if (!car || panicStopped_)\n                return;\n\n            if (!Settings::WheelFFBEnable)\n            {\n                if (initialized_ && enabledLastTick_)\n                {\n                    zero_all_forces();\n                    reset_signal_state();\n                    enabledLastTick_ = false;\n                    spdlog::info("WheelFFB: disabled live; all effects zeroed immediately");\n                }\n                return;\n            }\n            enabledLastTick_ = true;\n\n            lastUpdateTick_ = GetTickCount();\n''',
    "immediate live disable",
)

# Once gameplay owns the exclusive DirectInput handle again, apply changes to
# GlobalStrength to every already-created hardware effect.  All create_* paths
# already use the current setting, so this closes only the live-update gap.
replace_once(
'''            if (device_ && !deviceAcquired_)\n            {\n                const HRESULT acquireHr = device_->Acquire();\n                if (FAILED(acquireHr))\n                    return;\n                deviceAcquired_ = true;\n                device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n            }\n\n            float steer = read_game_steering();\n''',
'''            if (device_ && !deviceAcquired_)\n            {\n                const HRESULT acquireHr = device_->Acquire();\n                if (FAILED(acquireHr))\n                    return;\n                deviceAcquired_ = true;\n                device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n            }\n\n            apply_live_effect_gain();\n\n            float steer = read_game_steering();\n''',
    "live DirectInput gain update",
)

# UsePeriodicEffects is a backend-selection switch, not merely a recreate gate.
# Stop/release live GUID_Sine effects as soon as it is turned off.  The existing
# ConstantForce sine fallback then takes over in the same gameplay path.
replace_once(
'''            // Warm-up ramp prevents the first few garbage/settling frames from\n            // producing a DD-wheel spike.\n''',
'''            if (!Settings::WheelFFBUsePeriodicEffects &&\n                (roadTextureEffect_ || tireSlipEffect_))\n            {\n                disable_periodics();\n                spdlog::info(\n                    "WheelFFB: hardware periodic effects disabled live; using ConstantForce fallback");\n            }\n\n            // Warm-up ramp prevents the first few garbage/settling frames from\n            // producing a DD-wheel spike.\n''',
    "live periodic disable",
)

# The watchdog originally only looked at ConstantForce/Spring/Damper state.  If
# both condition effects are disabled while a hardware sine is active, a stalled
# game tick could otherwise leave road/tire vibration running indefinitely.
replace_once(
'''            if (elapsed > 250 &&\n                (prevConstantLevel_ != 0 || prevSpringCoefficient_ != 0 ||\n                 prevDamperCoefficient_ != 0))\n''',
'''            if (elapsed > 250 &&\n                (prevConstantLevel_ != 0 || prevSpringCoefficient_ != 0 ||\n                 prevDamperCoefficient_ != 0 || roadState_.lastMagnitude != 0 ||\n                 slipState_.lastMagnitude != 0))\n''',
    "watchdog covers hardware periodic effects",
)

# Insert a single gain helper before create_constant_effect().  Do not change
# effect-specific type parameters here; DIEP_GAIN is orthogonal to spring,
# damper, periodic and constant-force magnitudes.
needle = '''        bool create_constant_effect()\n        {\n'''
if text.count(needle) != 1:
    raise SystemExit("live gain helper insertion point not found exactly once")

gain_helper = r'''        DWORD configured_effect_gain() const
        {
            return static_cast<DWORD>(
                std::clamp(static_cast<float>(Settings::WheelFFBGlobalStrength), 0.0f, 1.0f) *
                static_cast<float>(DI_FFNOMINALMAX));
        }

        void apply_live_effect_gain()
        {
            if (!device_ || !deviceAcquired_ || panicStopped_)
                return;

            const DWORD gain = configured_effect_gain();
            if (gain == lastEffectGain_)
                return;

            DIEFFECT params{};
            params.dwSize = sizeof(params);
            params.dwGain = gain;

            bool failed = false;
            auto updateGain = [&](IDirectInputEffect* effect, const char* label)
            {
                if (!effect)
                    return;

                HRESULT hr = effect->SetParameters(&params, DIEP_GAIN);
                if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
                {
                    if (SUCCEEDED(device_->Acquire()))
                        deviceAcquired_ = true;
                    hr = effect->SetParameters(&params, DIEP_GAIN);
                }

                // Some DirectInput drivers do not advertise DIEP_GAIN as a
                // dynamic parameter.  DirectInput often restarts implicitly,
                // but drivers are also allowed to return DIERR_EFFECTPLAYING.
                // Stop only that effect and explicitly restart it with the new
                // gain rather than making the F11 Overall Strength slider fail.
                if (hr == DIERR_EFFECTPLAYING)
                {
                    effect->Stop();
                    hr = effect->SetParameters(&params, DIEP_GAIN | DIEP_START);
                }

                if (FAILED(hr))
                {
                    failed = true;
                    const DWORD now = GetTickCount();
                    if (now - lastGainErrorLog_ >= 2000)
                    {
                        lastGainErrorLog_ = now;
                        spdlog::warn(
                            "WheelFFB: live gain update failed for {} (0x{:08X})",
                            label, (unsigned)hr);
                    }
                }
            };

            updateGain(constantEffect_, "ConstantForce");
            updateGain(springEffect_, "GUID_Spring");
            updateGain(damperEffect_, "GUID_Damper");
            updateGain(roadTextureEffect_, "RoadTexture");
            updateGain(tireSlipEffect_, "TireSlip");

            if (!failed)
            {
                lastEffectGain_ = gain;
                spdlog::info(
                    "WheelFFB: live GlobalStrength applied to active effects ({}%)",
                    static_cast<unsigned>((gain * 100u) / DI_FFNOMINALMAX));
            }
        }

'''
text = text.replace(needle, gain_helper + needle, 1)

# The effects created during initialize() already carry the current gain.  Seed
# the cache after creation to avoid issuing a redundant gain command on frame 1.
replace_once(
'''            spdlog::info(\n                "WheelFFB: ready on '{}' (DirectInput COM, axes={}, buttons={}, global={}%, spring={}, damper={})",\n''',
'''            lastEffectGain_ = configured_effect_gain();\n\n            spdlog::info(\n                "WheelFFB: ready on '{}' (DirectInput COM, axes={}, buttons={}, global={}%, spring={}, damper={})",\n''',
    "seed effect gain cache",
)

# State for live controls.  0xFFFFFFFF is outside DirectInput's valid 0..10000
# gain range, guaranteeing the first post-recovery update can synchronize it.
replace_once(
'''        bool initialized_ = false;\n        bool panicStopped_ = false;\n        bool deviceAcquired_ = false;\n        bool periodicsActive_ = false;\n''',
'''        bool initialized_ = false;\n        bool panicStopped_ = false;\n        bool deviceAcquired_ = false;\n        bool enabledLastTick_ = true;\n        bool periodicsActive_ = false;\n''',
    "live enable state",
)
replace_once(
'''        DWORD lastUpdateTick_ = 0;\n        DWORD lastLogTick_ = 0;\n''',
'''        DWORD lastUpdateTick_ = 0;\n        DWORD lastLogTick_ = 0;\n        DWORD lastGainErrorLog_ = 0;\n        DWORD lastEffectGain_ = 0xFFFFFFFFu;\n''',
    "live gain state",
)

path.write_text(text, encoding="utf-8")
print("Applied live FFB controls: immediate disable, live gain, periodic switching/watchdog")
