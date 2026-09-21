from pathlib import Path

ffb = Path('src/hooks_wheel_ffb.cpp')
ui = Path('src/overlay/wheel_setup_ui.cpp')
text = ffb.read_text(encoding='utf-8')
uitext = ui.read_text(encoding='utf-8')


def rep(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 match, got {count}')
    text = text.replace(old, new, 1)
    print(f'PASS OK: {label}')


def urep(old: str, new: str, label: str) -> None:
    global uitext
    count = uitext.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 UI match, got {count}')
    uitext = uitext.replace(old, new, 1)
    print(f'PASS OK: {label}')


# PASS 1/10 - Direction semantics.
# InvertForce is for the game's signed ConstantForce signal.  Coupling that toggle
# to GUID_Spring can turn a perfectly good centering spring into an unstable
# runaway spring.  Give spring direction its own explicit setting instead.
rep(
'''    Setting<bool> WheelFFBInvertForce{\n        "WheelFFB", "InvertForce", false,\n        "Reverse steering force direction."\n    };\n\n    Setting<bool> WheelFFBDebugLog{\n''',
'''    Setting<bool> WheelFFBInvertForce{\n        "WheelFFB", "InvertForce", false,\n        "Reverse ConstantForce steering/event direction without changing the centering spring."\n    };\n\n    Setting<bool> WheelFFBInvertSpring{\n        "WheelFFB", "InvertSpring", false,\n        "Reverse only the DirectInput GUID_Spring condition direction. Leave off when the wheel returns toward centre normally."\n    };\n\n    Setting<bool> WheelFFBDebugLog{\n''',
'decouple ConstantForce and spring inversion')

rep(
'''            // R3 testing showed the old negative GUID_Spring coefficient pushes\n            // the wheel farther in the direction of steering instead of back to\n            // center. Keep hardware-spring direction consistent with the global\n            // InvertForce option: normal (false) uses the R3 centering sign, and\n            // inverted (true) reverses it together with ConstantForce.\n            const LONG coefficient = Settings::WheelFFBInvertForce\n                ? -coefficientMagnitude\n                : coefficientMagnitude;\n''',
'''            // R3 hardware testing established positive condition coefficients\n            // as the normal centering sign for this backend.  Keep this independent\n            // from ConstantForce inversion so fixing corner-force direction can\n            // never accidentally turn the centering spring into a runaway force.\n            const LONG coefficient = Settings::WheelFFBInvertSpring\n                ? -coefficientMagnitude\n                : coefficientMagnitude;\n''',
'independent GUID_Spring direction')

rep(
'''                "WheelFFB: GUID_Spring created (saturation={} / {}, R3 normal sign=positive, follows InvertForce)",\n''',
'''                "WheelFFB: GUID_Spring created (saturation={} / {}, R3 normal sign=positive, independent direction toggle)",\n''',
'accurate spring direction diagnostic')

# PASS 2/10 - F11 must expose both direction controls so physical testing does not
# require hand-editing INI files between runs.
urep(
'''    extern Setting<bool> WheelFFBUseHardwareSpring;\n    extern Setting<bool> WheelFFBUseHardwareDamper;\n\n    Setting<bool> WheelUniversalSetupEnable{\n''',
'''    extern Setting<bool> WheelFFBUseHardwareSpring;\n    extern Setting<bool> WheelFFBUseHardwareDamper;\n    extern Setting<bool> WheelFFBInvertForce;\n    extern Setting<bool> WheelFFBInvertSpring;\n\n    Setting<bool> WheelUniversalSetupEnable{\n''',
'F11 direction-setting declarations')

urep(
'''            ImGui::Checkbox("Hardware GUID_Spring", Settings::WheelFFBUseHardwareSpring.ptr());\n            ImGui::SameLine();\n            ImGui::Checkbox("Hardware GUID_Damper", Settings::WheelFFBUseHardwareDamper.ptr());\n\n            if (ImGui::Button("Load simulation baseline"))\n''',
'''            ImGui::Checkbox("Hardware GUID_Spring", Settings::WheelFFBUseHardwareSpring.ptr());\n            ImGui::SameLine();\n            ImGui::Checkbox("Hardware GUID_Damper", Settings::WheelFFBUseHardwareDamper.ptr());\n            ImGui::Checkbox("Reverse ConstantForce", Settings::WheelFFBInvertForce.ptr());\n            ImGui::SameLine();\n            ImGui::Checkbox("Reverse Spring", Settings::WheelFFBInvertSpring.ptr());\n            if (ImGui::IsItemHovered())\n                ImGui::SetTooltip("Use Reverse Spring only if the wheel pushes farther away from centre. ConstantForce direction is independent.");\n\n            if (ImGui::Button("Load simulation baseline"))\n''',
'F11 independent direction controls')

# PASS 3/10 - Hardware spring live-disable semantics.
# Previously disabling UseHardwareSpring left springEffect_ alive; because
# softwareSpring tests springEffect_ != nullptr, the checkbox effectively did
# nothing and could not switch to software centering until restart/recreate.
rep(
'''            if (springEffect_)\n            {\n                update_spring(\n                    suppressSpringForImpact\n                        ? 0.0f\n                        : springStrength * warmupScale * recreateScale);\n            }\n\n            const float softwareSpring =\n''',
'''            if (!Settings::WheelFFBUseHardwareSpring && springEffect_)\n            {\n                update_spring(0.0f);\n                springEffect_->Stop();\n                safe_release_effect(springEffect_, "hardware spring disabled");\n                prevSpringCoefficient_ = 0;\n                prevSpringSaturation_ = 0;\n                springStrategy_ = 1;\n                spdlog::info("WheelFFB: hardware spring disabled live; using software centering");\n            }\n\n            if (springEffect_)\n            {\n                update_spring(\n                    suppressSpringForImpact\n                        ? 0.0f\n                        : springStrength * warmupScale * recreateScale);\n            }\n\n            const float softwareSpring =\n''',
'live GUID_Spring disable actually switches backend')

# PASS 4/10 - Actuator command failure must not be masked by later successful
# SetParameters calls.  Retry acquisition/SETACTUATORSON on the next tick.
rep(
'''                const HRESULT actuatorHr =\n                    device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n                if (FAILED(actuatorHr))\n                    note_device_failure("SETACTUATORSON", actuatorHr);\n            }\n''',
'''                const HRESULT actuatorHr =\n                    device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n                if (FAILED(actuatorHr))\n                {\n                    note_device_failure("SETACTUATORSON", actuatorHr);\n                    device_->Unacquire();\n                    deviceAcquired_ = false;\n                    return;\n                }\n                clear_device_failure();\n            }\n''',
'actuator-on retry is not masked')

# PASS 5/10 - Initial actuator enable is part of successful initialization.
rep(
'''            deviceAcquired_ = true;\n\n            device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n\n            if (!create_constant_effect())\n''',
'''            deviceAcquired_ = true;\n\n            const HRESULT actuatorOnHr =\n                device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n            if (FAILED(actuatorOnHr))\n            {\n                spdlog::error(\n                    "WheelFFB: SETACTUATORSON during initialization failed (0x{:08X})",\n                    (unsigned)actuatorOnHr);\n                release_device();\n                release_directinput();\n                return false;\n            }\n\n            if (!create_constant_effect())\n''',
'initial actuator enable is checked')

# PASS 6/10 - Periodic effects are a pair.  Running one GUID_Sine while the
# fallback synthesizes both channels doubles whichever hardware effect survived.
rep(
'''            roadState_ = {};\n            slipState_ = {};\n            periodicsActive_ =\n                roadTextureEffect_ != nullptr && tireSlipEffect_ != nullptr;\n\n            if (!periodicsActive_)\n                spdlog::warn("WheelFFB: hardware periodic effects unavailable; using constant-force fallback");\n''',
'''            roadState_ = {};\n            slipState_ = {};\n            periodicsActive_ =\n                roadTextureEffect_ != nullptr && tireSlipEffect_ != nullptr;\n\n            if (!periodicsActive_)\n            {\n                // Treat the two sines atomically. A half-created pair plus the\n                // software fallback would double one signal and distort tuning.\n                disable_periodics();\n                recreateHoldoffUntil_ = GetTickCount() + 500;\n                spdlog::warn(\n                    "WheelFFB: complete hardware periodic pair unavailable; using ConstantForce fallback for both signals");\n            }\n''',
'atomic road/tire periodic backend')

# PASS 7/10 - Device-loss teardown should explicitly leave the acquisition flag
# false even if a stale driver object throws before release_device reaches it.
rep(
'''            release_effects_for_reinitialize();\n\n            if (device_)\n''',
'''            release_effects_for_reinitialize();\n            deviceAcquired_ = false;\n\n            if (device_)\n''',
'reinit acquisition state is fail-safe false')

# PASS 8/10 - Reinitialization must not wait on an obsolete gain retry timer.
# (The recovery patch already clears it; assert by rewriting the explanatory
# block to make the invariant explicit and visible in diagnostics.)
rep(
'''            lastEffectGain_ = 0xFFFFFFFFu;\n            nextGainRetryTick_ = 0;\n            recreateHoldoffUntil_ = 0;\n''',
'''            lastEffectGain_ = 0xFFFFFFFFu;\n            nextGainRetryTick_ = 0;\n            lastGainErrorLog_ = 0;\n            recreateHoldoffUntil_ = 0;\n''',
'reinit clears stale live-gain error state')

# PASS 9/10 - A successful full initialize should start with a fresh device fault
# window, otherwise a pre-replug timestamp can immediately retrigger teardown.
rep(
'''            deviceReinitPending_ = false;\n            deviceFailureSince_ = 0;\n            deviceReinitAfter_ = 0;\n            reset_signal_state();\n''',
'''            deviceReinitPending_ = false;\n            deviceFailureSince_ = 0;\n            deviceReinitAfter_ = 0;\n            retryAfter_ = 0;\n            reset_signal_state();\n''',
'fresh recovery window after successful initialize')

# PASS 10/10 - Improve the 2-second diagnostic so physical R3 testing can tell
# direction/backend state without guessing from the wheel feel alone.
rep(
'''                "WheelFFB DIAG: spd={:.2f} steer={:.3f} rate={:.4f} lat={:.2f} drift={:.2f} rough={:.2f} out={} spring={} coeff={} damper={} dcoeff={} periodic={}",\n''',
'''                "WheelFFB DIAG: spd={:.2f} steer={:.3f} rate={:.4f} lat={:.2f} drift={:.2f} rough={:.2f} out={} invCF={} spring={} invSpring={} coeff={} damper={} dcoeff={} periodic={}",\n''',
'FFB diagnostic direction/backend visibility')

rep(
'''                static_cast<int>(level),\n                springEffect_ ? "HW" : "SW",\n                static_cast<int>(prevSpringCoefficient_),\n                damperEffect_ ? "HW" : "SW",\n                static_cast<int>(prevDamperCoefficient_),\n                periodicsActive_);\n''',
'''                static_cast<int>(level),\n                bool(Settings::WheelFFBInvertForce),\n                springEffect_ ? "HW" : "SW",\n                bool(Settings::WheelFFBInvertSpring),\n                static_cast<int>(prevSpringCoefficient_),\n                damperEffect_ ? "HW" : "SW",\n                static_cast<int>(prevDamperCoefficient_),\n                periodicsActive_);\n''',
'FFB diagnostic direction/backend arguments')

ffb.write_text(text, encoding='utf-8')
ui.write_text(uitext, encoding='utf-8')
print('Completed round-3 ten-pass wheel/FFB correctness hardening')
