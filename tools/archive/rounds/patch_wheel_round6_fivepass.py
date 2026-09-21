from pathlib import Path

ffb_path = Path('src/hooks_wheel_ffb.cpp')
input_path = Path('src/input_manager.hpp')
setup_path = Path('src/overlay/wheel_setup_ui.cpp')
ffb = ffb_path.read_text(encoding='utf-8')
ims = input_path.read_text(encoding='utf-8')
setup = setup_path.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str, expected: int = 1) -> str:
    count = text.count(old)
    if count != expected:
        raise SystemExit(f'round6 {label}: expected {expected} match(es), got {count}')
    print(f'ROUND6 patched: {label}')
    return text.replace(old, new, expected)

# PASS 1 - FFB lifecycle / focus safety.
# An initialize attempt made while backgrounded sets appActive_=false before the
# window subclass exists. Do not let that state permanently suppress all future
# initialization attempts; initialize() itself verifies foreground ownership.
ffb = rep(ffb,
'''            if (!appActive_)\n            {\n                if (initialized_)\n                    zero_all_forces();\n                return;\n            }\n''',
'''            if (!appActive_)\n            {\n                // initialize() can set appActive_=false before the window\n                // subclass exists. Recover from that startup/background case\n                // only after the real game window is foreground again.\n                if (!initialized_ && gameHwnd_ && GetForegroundWindow() == gameHwnd_)\n                {\n                    appActive_ = true;\n                    warmupFrames_ = 0;\n                }\n                else\n                {\n                    if (initialized_)\n                        zero_all_forces();\n                    return;\n                }\n            }\n''',
'background startup can recover initialization')

# Normal menu->game Acquire needs the same foreground race guards as the
# input-loss recovery path because EXCLUSIVE|BACKGROUND can otherwise succeed
# after focus has moved to another application.
ffb = rep(ffb,
'''            if (device_ && !deviceAcquired_)\n            {\n                const HRESULT acquireHr = device_->Acquire();\n                if (FAILED(acquireHr) && acquireHr != S_FALSE)\n                {\n                    note_device_failure("gameplay Acquire", acquireHr);\n                    return;\n                }\n                deviceAcquired_ = true;\n                clear_device_failure();\n                const HRESULT actuatorHr =\n                    device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n''',
'''            if (device_ && !deviceAcquired_)\n            {\n                if (!gameHwnd_ || GetForegroundWindow() != gameHwnd_)\n                    return;\n\n                const HRESULT acquireHr = device_->Acquire();\n                if (FAILED(acquireHr) && acquireHr != S_FALSE)\n                {\n                    note_device_failure("gameplay Acquire", acquireHr);\n                    return;\n                }\n\n                if (GetForegroundWindow() != gameHwnd_)\n                {\n                    device_->Unacquire();\n                    deviceAcquired_ = false;\n                    return;\n                }\n\n                deviceAcquired_ = true;\n                clear_device_failure();\n                const HRESULT actuatorHr =\n                    device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n''',
'foreground-safe normal gameplay Acquire')

# zero_all_forces() is also called from menus and live-disable paths. If an
# effect update reports INPUTLOST there, never reacquire an exclusive DD device
# merely to write zero: loss/reacquire belongs to gameplay only.
ffb = rep(ffb,
'''            if (!appActive_ || !gameHwnd_ || GetForegroundWindow() != gameHwnd_)\n            {\n                deviceAcquired_ = false;\n                return false;\n            }\n\n            deviceAcquired_ = false;\n            const HRESULT acquireHr = device_->Acquire();\n''',
'''            const bool inGameplay =\n                Game::current_mode && (*Game::current_mode == STATE_GAME);\n            if (!inGameplay)\n            {\n                deviceAcquired_ = false;\n                return false;\n            }\n            if (!appActive_ || !gameHwnd_ || GetForegroundWindow() != gameHwnd_)\n            {\n                deviceAcquired_ = false;\n                return false;\n            }\n\n            deviceAcquired_ = false;\n            const HRESULT acquireHr = device_->Acquire();\n''',
'input-loss reacquire is gameplay-only')

# Remove an accidental duplicate live hardware-spring disable block. The first
# block releases springEffect_, making the second dead code, but keeping two
# variants made later lifecycle changes easy to apply to the wrong copy.
duplicate = '''\n            if (!Settings::WheelFFBUseHardwareSpring && springEffect_)\n            {\n                update_spring(0.0f);\n                springEffect_->Stop();\n                safe_release_effect(springEffect_, "hardware spring disabled");\n                prevSpringCoefficient_ = 0;\n                prevSpringSaturation_ = 0;\n                springStrategy_ = 1;\n                spdlog::info("WheelFFB: hardware spring disabled live; using software centering");\n            }\n'''
ffb = rep(ffb, duplicate, '', 'remove duplicate hardware spring disable block')
ffb = rep(ffb, 'using software spring', 'using software centering', 'normalize spring fallback diagnostic')

# PASS 2 - Exact DirectInput identity / hotplug safety.
# A saved GUID is a pin, not a hint. If it disappears, do not silently attach
# torque to another same-name wheel. The sole exception is the short holdoff
# after that exact interface failed initialization, where sibling fallback is
# intentional; successful selection then clears the rejected-interface state.
ffb = rep(ffb,
'''                if (!ctx.found)\n                {\n                    spdlog::warn(\n                        "WheelFFB: saved DirectInput GUID '{}' unavailable/rejected; falling back to DeviceName",\n                        Settings::WheelFFBDeviceGuid.get());\n                    ctx = {};\n                    ctx.self = this;\n                }\n''',
'''                if (!ctx.found)\n                {\n                    const bool rejectedExactInterface =\n                        failedInterfaceGuid_ == configuredGuid &&\n                        tick_before(GetTickCount(), failedInterfaceUntil_);\n                    if (!rejectedExactInterface)\n                    {\n                        spdlog::warn(\n                            "WheelFFB: saved DirectInput GUID '{}' is unavailable; refusing DeviceName fallback",\n                            Settings::WheelFFBDeviceGuid.get());\n                        release_directinput();\n                        return false;\n                    }\n\n                    spdlog::warn(\n                        "WheelFFB: saved DirectInput GUID '{}' was rejected during initialization; trying a sibling DeviceName interface",\n                        Settings::WheelFFBDeviceGuid.get());\n                    ctx = {};\n                    ctx.self = this;\n                }\n''',
'exact FFB GUID only falls back after explicit interface rejection')

ffb = rep(ffb,
'''            lastEffectGain_ = configured_effect_gain();\n\n            spdlog::info(\n''',
'''            lastEffectGain_ = configured_effect_gain();\n            failedInterfaceGuid_.clear();\n            failedInterfaceUntil_ = 0;\n\n            spdlog::info(\n''',
'clear stale rejected-interface state after successful init')

ffb = rep(ffb,
'''        "Exact DirectInput instance GUID selected by F11 Wheel Setup; DeviceName remains the fallback."\n''',
'''        "Exact DirectInput instance GUID selected by F11 Wheel Setup; a saved GUID never falls back unless that exact interface failed initialization."\n''',
'DeviceGuid setting documents strict pinning')

# F11's DirectInput reader must obey the same identity rule; otherwise the UI
# can display/live-read a same-name device even while FFB correctly refuses it.
setup = rep(setup,
'''            if (!guid.empty())\n                for (size_t i=0; i<devices_.size(); ++i)\n                    if (devices_[i].guidKey == guid) { index=int(i); break; }\n            if (index < 0 && !wantedName.empty())\n                for (size_t i=0; i<devices_.size(); ++i)\n                    if (devices_[i].name == wantedName) { index=int(i); break; }\n            if (index < 0 && guid.empty() && wantedName.empty() && !devices_.empty()) index=0;\n''',
'''            if (!guid.empty())\n            {\n                for (size_t i=0; i<devices_.size(); ++i)\n                    if (devices_[i].guidKey == guid) { index=int(i); break; }\n                if (index < 0)\n                    return false;\n            }\n            else if (!wantedName.empty())\n            {\n                for (size_t i=0; i<devices_.size(); ++i)\n                    if (devices_[i].name == wantedName) { index=int(i); break; }\n            }\n            if (index < 0 && guid.empty() && wantedName.empty() && !devices_.empty()) index=0;\n''',
'F11 reader keeps saved GUID strict')

# Stable USB identities must be authoritative. A serial/path mismatch must not
# fall through to weak GUID+occurrence and accidentally swap two identical
# devices after reconnect/reorder.
ims = rep(ims,
'''\t\tif (!binding.deviceSerial.empty() && usbIdentityMatches && device.serial == binding.deviceSerial)\n\t\t\treturn true;\n\t\tif (!binding.devicePath.empty() && usbIdentityMatches && device.path == binding.devicePath)\n\t\t\treturn true;\n\t\treturn device.guid == binding.deviceGuid && device.occurrence == binding.deviceOccurrence;\n''',
'''\t\tif (!binding.deviceSerial.empty())\n\t\t\treturn usbIdentityMatches && device.serial == binding.deviceSerial;\n\t\tif (!binding.devicePath.empty())\n\t\t\treturn usbIdentityMatches && device.path == binding.devicePath;\n\t\treturn device.guid == binding.deviceGuid && device.occurrence == binding.deviceOccurrence;\n''',
'serial/path identity is authoritative')

# PASS 3 - FFB math parity and ramp safety.
# Software spring fallback must preserve the same corner-load boost and
# grip-loss unloading as GUID_Spring; only the backend should change.
ffb = rep(ffb,
'''            const float softwareSpring =\n                springEffect_\n                    ? 0.0f\n                    : -steer * static_cast<float>(Settings::WheelFFBSpringStrength) * speedCurve;\n''',
'''            const float softwareSpring =\n                springEffect_\n                    ? 0.0f\n                    : -steer * springStrength;\n''',
'software spring matches hardware spring model')

# Warm-up/recreate ramps previously protected structural force but not hardware
# or fallback road/slip texture. Scale those actuation paths too.
ffb = rep(ffb,
'''            // Hardware periodics are preferred. If unavailable, inject a capped\n            // low-frequency sine after the tanh compressor.\n            float fallbackVibration = 0.0f;\n            if (!periodicsActive_)\n            {\n                fallbackVibration += synth_fallback(\n                    roadPhase_, roadAmp, std::min(roadFreq, 15.0f));\n                fallbackVibration += synth_fallback(\n                    slipPhase_, slipAmp, std::min(slipFreq, 15.0f));\n            }\n''',
'''            // Hardware periodics are preferred. If unavailable, inject a capped\n            // low-frequency sine after the tanh compressor. Road/slip signals\n            // share the same startup/recreate ramp as structural force.\n            const float effectRampScale = warmupScale * recreateScale;\n            float fallbackVibration = 0.0f;\n            if (!periodicsActive_)\n            {\n                fallbackVibration += synth_fallback(\n                    roadPhase_, roadAmp * effectRampScale, std::min(roadFreq, 15.0f));\n                fallbackVibration += synth_fallback(\n                    slipPhase_, slipAmp * effectRampScale, std::min(slipFreq, 15.0f));\n            }\n''',
'road and slip fallback obey warmup/recreate ramp')

ffb = rep(ffb,
'''                update_periodic(roadTextureEffect_, roadState_, roadAmp, roadFreq);\n                update_periodic(tireSlipEffect_, slipState_, slipAmp, slipFreq);\n''',
'''                update_periodic(roadTextureEffect_, roadState_, roadAmp * effectRampScale, roadFreq);\n                update_periodic(tireSlipEffect_, slipState_, slipAmp * effectRampScale, slipFreq);\n''',
'hardware periodics obey warmup/recreate ramp')

# PASS 4 - Numeric hardening. Reject NaN/Inf before float->integer actuator
# conversions; malformed INI or corrupt transient game values must fail safe to
# zero force rather than invoking implementation-defined casts.
ffb = rep(ffb,
'''            const float speed = car->field_1C4;\n            const float speedNorm = std::clamp(speed / 2.0f, 0.0f, 1.0f);\n            const float outputStrength = std::clamp(\n                static_cast<float>(Settings::WheelFFBGlobalStrength), 0.0f, 1.5f);\n\n            const uint32_t stateFlags = car->field_8;\n            const uint32_t curGear = car->cur_gear_208;\n            const float lateralRaw = car->field_264 + car->field_268;\n''',
'''            const float speedRaw = car->field_1C4;\n            const float speed = std::isfinite(speedRaw) ? speedRaw : 0.0f;\n            const float speedNorm = std::clamp(speed / 2.0f, 0.0f, 1.0f);\n            const float configuredStrength =\n                static_cast<float>(Settings::WheelFFBGlobalStrength);\n            const float outputStrength = std::isfinite(configuredStrength)\n                ? std::clamp(configuredStrength, 0.0f, 1.5f)\n                : 0.0f;\n\n            const uint32_t stateFlags = car->field_8;\n            const uint32_t curGear = car->cur_gear_208;\n            const float lateralSum = car->field_264 + car->field_268;\n            const float lateralRaw = std::isfinite(lateralSum) ? lateralSum : 0.0f;\n''',
'sanitize speed, master gain, and lateral signal')

ffb = rep(ffb,
'''                roughness = std::max(\n                    roughness,\n                    static_cast<float>(sub_1149C0(\n                        car->water_flag_24C[i],\n                        static_cast<int>(car->OnRoadPlace_5C.loadColiType_0),\n                        &waterFlag)));\n''',
'''                const float surfaceRoughness = static_cast<float>(sub_1149C0(\n                    car->water_flag_24C[i],\n                    static_cast<int>(car->OnRoadPlace_5C.loadColiType_0),\n                    &waterFlag));\n                if (std::isfinite(surfaceRoughness))\n                    roughness = std::max(roughness, surfaceRoughness);\n''',
'sanitize surface roughness')

ffb = rep(ffb,
'''            float total = (structural + events) * outputStrength;\n            if (Settings::WheelFFBInvertForce)\n                total = -total;\n\n            total *= warmupScale * recreateScale;\n''',
'''            float total = (structural + events) * outputStrength;\n            if (Settings::WheelFFBInvertForce)\n                total = -total;\n\n            total *= warmupScale * recreateScale;\n            if (!std::isfinite(total))\n                total = 0.0f;\n''',
'non-finite structural/event force fails safe to zero')

ffb = rep(ffb,
'''            const LONG maxSlew = static_cast<LONG>(\n                std::clamp(static_cast<float>(Settings::WheelFFBSlewRate), 0.01f, 1.0f) *\n                static_cast<float>(DI_FFNOMINALMAX));\n''',
'''            const float configuredSlew = static_cast<float>(Settings::WheelFFBSlewRate);\n            const float safeSlew = std::isfinite(configuredSlew)\n                ? std::clamp(configuredSlew, 0.01f, 1.0f)\n                : 0.06f;\n            const LONG maxSlew = static_cast<LONG>(\n                safeSlew * static_cast<float>(DI_FFNOMINALMAX));\n''',
'sanitize slew rate before integer conversion')

# Clamp/sanitize the three hardware effect update boundaries and fallback sine.
ffb = rep(ffb,
'''            const LONG coefficientMagnitude = static_cast<LONG>(\n                std::clamp(strength, 0.0f, 1.0f) *\n                static_cast<float>(DI_FFNOMINALMAX));\n''',
'''            const float safeStrength = std::isfinite(strength)\n                ? std::clamp(strength, 0.0f, 1.0f)\n                : 0.0f;\n            const LONG coefficientMagnitude = static_cast<LONG>(\n                safeStrength * static_cast<float>(DI_FFNOMINALMAX));\n''',
'sanitize spring strength')

# Spring saturation exists in create + update; sanitize both occurrences.
old_sat = '''            const DWORD saturation = static_cast<DWORD>(\n                std::clamp(static_cast<float>(Settings::WheelFFBSpringSaturation), 0.1f, 1.0f) *\n                static_cast<float>(DI_FFNOMINALMAX));\n'''
new_sat = '''            const float configuredSaturation =\n                static_cast<float>(Settings::WheelFFBSpringSaturation);\n            const float safeSaturation = std::isfinite(configuredSaturation)\n                ? std::clamp(configuredSaturation, 0.1f, 1.0f)\n                : 0.775f;\n            const DWORD saturation = static_cast<DWORD>(\n                safeSaturation * static_cast<float>(DI_FFNOMINALMAX));\n'''
ffb = rep(ffb, old_sat, new_sat, 'sanitize spring saturation', expected=2)

ffb = rep(ffb,
'''            const LONG coefficient = static_cast<LONG>(\n                std::clamp(strength, 0.0f, 1.0f) *\n                static_cast<float>(DI_FFNOMINALMAX));\n''',
'''            const float safeStrength = std::isfinite(strength)\n                ? std::clamp(strength, 0.0f, 1.0f)\n                : 0.0f;\n            const LONG coefficient = static_cast<LONG>(\n                safeStrength * static_cast<float>(DI_FFNOMINALMAX));\n''',
'sanitize damper strength')

ffb = rep(ffb,
'''            const float magnitudeClamped = std::clamp(magnitude, 0.0f, 1.0f);\n            const DWORD mag = magnitudeClamped < 0.01f\n''',
'''            const float magnitudeClamped = std::isfinite(magnitude)\n                ? std::clamp(magnitude, 0.0f, 1.0f)\n                : 0.0f;\n            const DWORD mag = magnitudeClamped < 0.01f\n''',
'sanitize periodic magnitude')
ffb = rep(ffb,
'''            frequency = std::clamp(frequency, 1.0f, 100.0f);\n            const DWORD period = static_cast<DWORD>(1000000.0f / frequency);\n''',
'''            frequency = std::isfinite(frequency)\n                ? std::clamp(frequency, 1.0f, 100.0f)\n                : 30.0f;\n            const DWORD period = static_cast<DWORD>(1000000.0f / frequency);\n''',
'sanitize periodic frequency')

ffb = rep(ffb,
'''        float synth_fallback(float& phase, float amplitude, float frequency)\n        {\n            if (amplitude <= 0.005f)\n''',
'''        float synth_fallback(float& phase, float amplitude, float frequency)\n        {\n            if (!std::isfinite(amplitude) || !std::isfinite(frequency) ||\n                amplitude <= 0.005f)\n''',
'sanitize software sine input')

# PASS 5 - Runtime/F11 identity safety is covered by the strict UI reader above.
# Keep selected-device status unambiguous when exact GUID is absent.
setup = rep(setup,
'''            ImGui::TextWrapped(\n                "Universal legacy DirectInput wheel setup. Bind here from a game menu; gameplay FFB uses the same selected product name.");\n''',
'''            ImGui::TextWrapped(\n                "Universal DirectInput wheel setup. Bind here from a game menu; gameplay FFB follows the exact selected DirectInput GUID.");\n''',
'F11 text documents exact GUID ownership')

ffb_path.write_text(ffb, encoding='utf-8')
input_path.write_text(ims, encoding='utf-8')
setup_path.write_text(setup, encoding='utf-8')
print('Applied round-6 five-pass lifecycle, identity, math, and runtime hardening')
