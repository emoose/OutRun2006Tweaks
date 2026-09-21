from pathlib import Path

path = Path("src/hooks_wheel_ffb.cpp")
text = path.read_text(encoding="utf-8")


def replace_once(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    text = text.replace(old, new, 1)


replace_once(
'''    Setting<float> WheelFFBDamperStrength{\n        "WheelFFB", "DamperStrength", 0.10f,\n        "Resistance to rapid steering movement.", Range<float>{ 0.0f, 1.0f }\n    };\n\n    Setting<float> WheelFFBSteeringWeight{\n''',
'''    Setting<float> WheelFFBDamperStrength{\n        "WheelFFB", "DamperStrength", 0.10f,\n        "Dynamic steering damping. For DD wheels try 0.25-0.45.", Range<float>{ 0.0f, 1.0f }\n    };\n\n    Setting<bool> WheelFFBUseHardwareDamper{\n        "WheelFFB", "UseHardwareDamper", true,\n        "Use DirectInput GUID_Damper when the wheel supports it; otherwise use the software fallback."\n    };\n\n    Setting<float> WheelFFBLowSpeedSpring{\n        "WheelFFB", "LowSpeedSpring", 0.08f,\n        "Fraction of aligning/centering force retained at very low speed.", Range<float>{ 0.0f, 0.5f }\n    };\n\n    Setting<float> WheelFFBSpringLoadBoost{\n        "WheelFFB", "SpringLoadBoost", 0.35f,\n        "Extra aligning force under cornering load before grip-loss unloading.", Range<float>{ 0.0f, 1.0f }\n    };\n\n    Setting<float> WheelFFBSteeringWeight{\n''',
"simulation settings"
)

replace_once(
'''            const float speedCurve =\n                std::clamp(speedNorm / 0.25f, 0.0f, 1.0f) *\n                (0.35f + 0.65f * speedNorm);\n\n            const float springStrength =\n                std::clamp(\n                    static_cast<float>(Settings::WheelFFBSpringStrength) *\n                    speedCurve * gripFactor,\n                    0.0f, 1.0f);\n''',
'''            // Simulation-style aligning backbone: light at parking speed,\n            // progressively stronger with vehicle speed, then additionally\n            // loaded by cornering force. Deep slip unloads the wheel again.\n            const float lowSpeedSpring = std::clamp(\n                static_cast<float>(Settings::WheelFFBLowSpeedSpring), 0.0f, 0.5f);\n            const float speedCurve = std::clamp(\n                lowSpeedSpring + (1.0f - lowSpeedSpring) *\n                    std::pow(speedNorm, 1.60f),\n                0.0f, 1.0f);\n            const float cornerLoad = std::clamp(std::abs(latNorm), 0.0f, 1.0f);\n            const float loadBoost = 1.0f +\n                cornerLoad * static_cast<float>(Settings::WheelFFBSpringLoadBoost);\n\n            const float springStrength =\n                std::clamp(\n                    static_cast<float>(Settings::WheelFFBSpringStrength) *\n                    speedCurve * loadBoost * gripFactor,\n                    0.0f, 1.0f);\n''',
"dynamic spring curve"
)

replace_once(
'''            // Keep the current software damper for this first comparison build.\n            // A separate GUID_Damper can be tested after the hardware spring is\n            // validated on the R3, so the two changes are not confounded.\n            constexpr float SteerRateScale = 10.0f;\n            const float damper =\n                -steerRate * SteerRateScale *\n                static_cast<float>(Settings::WheelFFBDamperStrength) *\n                (0.4f + 0.6f * speedNorm);\n''',
'''            // ACC/AMS2-inspired dynamic damping. It resists steering velocity\n            // rather than pulling toward centre, grows with vehicle speed, and\n            // relaxes when the tyres are deeply sliding so counter-steer is not\n            // smothered. Prefer a native DirectInput GUID_Damper on wheels that\n            // implement it and keep the old software term as a fallback.\n            const float dampingSpeed =\n                0.10f + 0.90f * std::pow(speedNorm, 1.30f);\n            const float dampingGrip = 0.25f + 0.75f * gripFactor;\n            const float dynamicDamperStrength = std::clamp(\n                static_cast<float>(Settings::WheelFFBDamperStrength) *\n                    dampingSpeed * dampingGrip,\n                0.0f, 1.0f);\n\n            if (!Settings::WheelFFBUseHardwareDamper && damperEffect_)\n            {\n                update_damper(0.0f);\n                damperEffect_->Stop();\n                safe_release_effect(damperEffect_, "hardware damper disabled");\n                prevDamperCoefficient_ = 0;\n                damperStrategy_ = -1;\n                spdlog::info("WheelFFB: hardware damper disabled live; using software damping");\n            }\n\n            if (damperEffect_)\n                update_damper(dynamicDamperStrength * warmupScale * recreateScale);\n\n            constexpr float SteerRateScale = 10.0f;\n            const float damper = damperEffect_\n                ? 0.0f\n                : -steerRate * SteerRateScale * dynamicDamperStrength;\n''',
"dynamic damper block"
)

replace_once(
'''            if (Settings::WheelFFBUseHardwareSpring &&\n                !springEffect_ &&\n                (updateCounter_ % 60) == 0 &&\n                GetTickCount() >= springRecreateHoldoffUntil_)\n            {\n                create_spring_effect();\n            }\n\n            prevGear_ = curGear;\n''',
'''            if (Settings::WheelFFBUseHardwareSpring &&\n                !springEffect_ &&\n                (updateCounter_ % 60) == 0 &&\n                GetTickCount() >= springRecreateHoldoffUntil_)\n            {\n                create_spring_effect();\n            }\n\n            if (Settings::WheelFFBUseHardwareDamper &&\n                !damperEffect_ &&\n                (updateCounter_ % 60) == 0 &&\n                GetTickCount() >= damperRecreateHoldoffUntil_)\n            {\n                create_damper_effect();\n            }\n\n            prevGear_ = curGear;\n''',
"damper recreation"
)

replace_once(
'''            if (elapsed > 250 &&\n                (prevConstantLevel_ != 0 || prevSpringCoefficient_ != 0))\n''',
'''            if (elapsed > 250 &&\n                (prevConstantLevel_ != 0 || prevSpringCoefficient_ != 0 ||\n                 prevDamperCoefficient_ != 0))\n''',
"watchdog damper"
)

replace_once(
'''            if (springEffect_)\n            {\n                const HRESULT springHr = springEffect_->Stop();\n                spdlog::info("WheelFFB: PanicStop spring Stop => 0x{:08X}", (unsigned)springHr);\n            }\n\n            if (roadTextureEffect_)\n''',
'''            if (springEffect_)\n            {\n                const HRESULT springHr = springEffect_->Stop();\n                spdlog::info("WheelFFB: PanicStop spring Stop => 0x{:08X}", (unsigned)springHr);\n            }\n\n            if (damperEffect_)\n            {\n                const HRESULT damperHr = damperEffect_->Stop();\n                spdlog::info("WheelFFB: PanicStop damper Stop => 0x{:08X}", (unsigned)damperHr);\n            }\n\n            if (roadTextureEffect_)\n''',
"panic damper"
)

replace_once(
'''            if (Settings::WheelFFBUseHardwareSpring && !create_spring_effect())\n            {\n                spdlog::warn(\n                    "WheelFFB: hardware GUID_Spring unavailable; retaining software spring fallback");\n            }\n\n            create_periodic_effects();\n''',
'''            if (Settings::WheelFFBUseHardwareSpring && !create_spring_effect())\n            {\n                spdlog::warn(\n                    "WheelFFB: hardware GUID_Spring unavailable; retaining software spring fallback");\n            }\n\n            if (Settings::WheelFFBUseHardwareDamper && !create_damper_effect())\n            {\n                spdlog::warn(\n                    "WheelFFB: hardware GUID_Damper unavailable; retaining software damper fallback");\n            }\n\n            create_periodic_effects();\n''',
"initialize damper"
)

replace_once(
'''            spdlog::info(\n                "WheelFFB: ready on '{}' (DirectInput COM, axes={}, buttons={}, global={}%, spring={})",\n                selectedName_,\n                caps.dwAxes,\n                caps.dwButtons,\n                static_cast<int>(static_cast<float>(Settings::WheelFFBGlobalStrength) * 100.0f),\n                springEffect_ ? "GUID_Spring" : "software");\n''',
'''            spdlog::info(\n                "WheelFFB: ready on '{}' (DirectInput COM, axes={}, buttons={}, global={}%, spring={}, damper={})",\n                selectedName_,\n                caps.dwAxes,\n                caps.dwButtons,\n                static_cast<int>(static_cast<float>(Settings::WheelFFBGlobalStrength) * 100.0f),\n                springEffect_ ? "GUID_Spring" : "software",\n                damperEffect_ ? "GUID_Damper" : "software");\n''',
"ready log damper"
)

# Insert GUID_Damper implementation immediately before the periodic-effect helper.
needle = '''        IDirectInputEffect* create_periodic_effect(const char* label, float initialHz)\n'''
if text.count(needle) != 1:
    raise SystemExit("damper insertion point not found exactly once")

damper_impl = r'''        bool create_damper_effect()
        {
            if (!device_ || !Settings::WheelFFBUseHardwareDamper)
                return false;

            DWORD axes[1] = { DIJOFS_X };
            LONG directions[1] = { 0 };
            DICONDITION condition{};
            condition.lOffset = 0;
            condition.lPositiveCoefficient = 0;
            condition.lNegativeCoefficient = 0;
            condition.dwPositiveSaturation = DI_FFNOMINALMAX;
            condition.dwNegativeSaturation = DI_FFNOMINALMAX;
            condition.lDeadBand = 0;

            DIEFFECT effect{};
            effect.dwSize = sizeof(effect);
            effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
            effect.dwDuration = INFINITE;
            effect.dwSamplePeriod = 0;
            effect.dwGain = static_cast<DWORD>(
                std::clamp(static_cast<float>(Settings::WheelFFBGlobalStrength), 0.0f, 1.0f) *
                static_cast<float>(DI_FFNOMINALMAX));
            effect.dwTriggerButton = DIEB_NOTRIGGER;
            effect.dwTriggerRepeatInterval = 0;
            effect.cAxes = 1;
            effect.rgdwAxes = axes;
            effect.rglDirection = directions;
            effect.cbTypeSpecificParams = sizeof(condition);
            effect.lpvTypeSpecificParams = &condition;

            HRESULT hr = device_->CreateEffect(
                GUID_Damper, &effect, &damperEffect_, nullptr);
            if (FAILED(hr) || !damperEffect_)
            {
                spdlog::warn(
                    "WheelFFB: CreateEffect(GUID_Damper) failed (0x{:08X})",
                    (unsigned)hr);
                damperRecreateHoldoffUntil_ = GetTickCount() + 1000;
                return false;
            }

            hr = damperEffect_->Start(1, 0);
            if (FAILED(hr))
                spdlog::warn("WheelFFB: GUID_Damper initial Start failed (0x{:08X})", (unsigned)hr);

            prevDamperCoefficient_ = 0;
            damperStrategy_ = -1;
            spdlog::info("WheelFFB: GUID_Damper created (dynamic speed/grip damping)");
            return true;
        }

        void update_damper(float strength)
        {
            if (!damperEffect_ || !device_ || panicStopped_)
                return;

            // Positive coefficients are the conventional DirectInput condition
            // representation for GUID_Damper; the effect type itself applies
            // force opposite steering velocity.
            const LONG coefficient = static_cast<LONG>(
                std::clamp(strength, 0.0f, 1.0f) *
                static_cast<float>(DI_FFNOMINALMAX));

            const bool silence = coefficient == 0 && prevDamperCoefficient_ != 0;
            const bool changed = std::abs(coefficient - prevDamperCoefficient_) > 40;
            if (!silence && !changed)
                return;

            DICONDITION condition{};
            condition.lOffset = 0;
            condition.lPositiveCoefficient = coefficient;
            condition.lNegativeCoefficient = coefficient;
            condition.dwPositiveSaturation = DI_FFNOMINALMAX;
            condition.dwNegativeSaturation = DI_FFNOMINALMAX;
            condition.lDeadBand = 0;

            DIEFFECT params{};
            params.dwSize = sizeof(params);
            params.cbTypeSpecificParams = sizeof(condition);
            params.lpvTypeSpecificParams = &condition;

            DWORD flags = DIEP_TYPESPECIFICPARAMS |
                (damperStrategy_ == 1 ? DIEP_START : 0);
            HRESULT hr = damperEffect_->SetParameters(&params, flags);

            if (damperStrategy_ == -1)
            {
                if (SUCCEEDED(hr))
                {
                    damperStrategy_ = 0;
                    spdlog::info("WheelFFB: GUID_Damper updates work without DIEP_START");
                }
                else
                {
                    hr = damperEffect_->SetParameters(
                        &params, DIEP_TYPESPECIFICPARAMS | DIEP_START);
                    if (SUCCEEDED(hr))
                    {
                        damperStrategy_ = 1;
                        spdlog::info("WheelFFB: GUID_Damper driver requires DIEP_START");
                    }
                }
            }

            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
            {
                device_->Acquire();
                hr = damperEffect_->SetParameters(
                    &params, DIEP_TYPESPECIFICPARAMS | DIEP_START);
            }

            if (FAILED(hr))
            {
                spdlog::warn(
                    "WheelFFB: GUID_Damper update failed (0x{:08X}); using software fallback",
                    (unsigned)hr);
                damperEffect_->Stop();
                safe_release_effect(damperEffect_, "stale damper");
                prevDamperCoefficient_ = 0;
                damperStrategy_ = -1;
                damperRecreateHoldoffUntil_ = GetTickCount() + 1000;
                return;
            }

            prevDamperCoefficient_ = coefficient;
        }

'''
text = text.replace(needle, damper_impl + needle, 1)

replace_once(
'''            if (springEffect_ && prevSpringCoefficient_ != 0)\n                update_spring(0.0f);\n\n            prevStructuralLevel_ = 0;\n''',
'''            if (springEffect_ && prevSpringCoefficient_ != 0)\n                update_spring(0.0f);\n\n            if (damperEffect_ && prevDamperCoefficient_ != 0)\n                update_damper(0.0f);\n\n            prevStructuralLevel_ = 0;\n''',
"zero damper"
)

replace_once(
'''            prevStructuralLevel_ = 0;\n            prevSpringCoefficient_ = 0;\n            crashImpulseTimer_ = 0;\n''',
'''            prevStructuralLevel_ = 0;\n            prevSpringCoefficient_ = 0;\n            prevDamperCoefficient_ = 0;\n            crashImpulseTimer_ = 0;\n''',
"reset damper state"
)

replace_once(
'''                "WheelFFB DIAG: spd={:.2f} steer={:.3f} rate={:.4f} lat={:.2f} drift={:.2f} rough={:.2f} out={} spring={} coeff={} periodic={}",\n''',
'''                "WheelFFB DIAG: spd={:.2f} steer={:.3f} rate={:.4f} lat={:.2f} drift={:.2f} rough={:.2f} out={} spring={} coeff={} damper={} dcoeff={} periodic={}",\n''',
"diagnostic format"
)

replace_once(
'''                springEffect_ ? "HW" : "SW",\n                static_cast<int>(prevSpringCoefficient_),\n                periodicsActive_);\n''',
'''                springEffect_ ? "HW" : "SW",\n                static_cast<int>(prevSpringCoefficient_),\n                damperEffect_ ? "HW" : "SW",\n                static_cast<int>(prevDamperCoefficient_),\n                periodicsActive_);\n''',
"diagnostic args"
)

replace_once(
'''        IDirectInputEffect* constantEffect_ = nullptr;\n        IDirectInputEffect* springEffect_ = nullptr;\n        IDirectInputEffect* roadTextureEffect_ = nullptr;\n''',
'''        IDirectInputEffect* constantEffect_ = nullptr;\n        IDirectInputEffect* springEffect_ = nullptr;\n        IDirectInputEffect* damperEffect_ = nullptr;\n        IDirectInputEffect* roadTextureEffect_ = nullptr;\n''',
"damper member"
)

replace_once(
'''        int periodicStrategy_ = -1;\n        int springStrategy_ = -1;\n\n        DWORD retryAfter_ = 0;\n''',
'''        int periodicStrategy_ = -1;\n        int springStrategy_ = -1;\n        int damperStrategy_ = -1;\n\n        DWORD retryAfter_ = 0;\n''',
"damper strategy"
)

replace_once(
'''        DWORD recreateHoldoffUntil_ = 0;\n        DWORD springRecreateHoldoffUntil_ = 0;\n        DWORD lastUpdateTick_ = 0;\n''',
'''        DWORD recreateHoldoffUntil_ = 0;\n        DWORD springRecreateHoldoffUntil_ = 0;\n        DWORD damperRecreateHoldoffUntil_ = 0;\n        DWORD lastUpdateTick_ = 0;\n''',
"damper holdoff"
)

replace_once(
'''        LONG prevStructuralLevel_ = 0;\n        LONG prevSpringCoefficient_ = 0;\n        DWORD prevSpringSaturation_ = 0;\n''',
'''        LONG prevStructuralLevel_ = 0;\n        LONG prevSpringCoefficient_ = 0;\n        LONG prevDamperCoefficient_ = 0;\n        DWORD prevSpringSaturation_ = 0;\n''',
"damper coefficient member"
)

path.write_text(text, encoding="utf-8")
print("Applied simulation-style dynamic spring + GUID_Damper patch")
