from pathlib import Path

p = Path('src/hooks_wheel_ffb.cpp')
text = p.read_text(encoding='utf-8')


def rep(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 match, got {count}')
    text = text.replace(old, new, 1)
    print(f'patched: {label}')


rep(
'''    constexpr UINT FFB_WATCHDOG_INTERVAL_MS = 100;\n''',
'''    constexpr UINT FFB_WATCHDOG_INTERVAL_MS = 100;\n    constexpr DWORD FFB_DEVICE_FAILURE_GRACE_MS = 750;\n    constexpr DWORD FFB_DEVICE_RETRY_MS = 750;\n''',
'device recovery timing')

rep(
'''            lastUpdateTick_ = GetTickCount();\n\n            if (!initialized_)\n''',
'''            const DWORD updateNow = GetTickCount();\n\n            if (deviceReinitPending_)\n            {\n                if (updateNow < deviceReinitAfter_)\n                    return;\n                teardown_for_reinitialize();\n                deviceReinitPending_ = false;\n                deviceFailureSince_ = 0;\n                initialized_ = false;\n                retryAfter_ = updateNow + FFB_DEVICE_RETRY_MS;\n                spdlog::info(\n                    "WheelFFB: DirectInput device released; waiting to re-enumerate after device loss");\n                return;\n            }\n\n            lastUpdateTick_ = updateNow;\n\n            if (!initialized_)\n''',
'process pending device reinitialization')

rep(
'''            if (device_ && !deviceAcquired_)\n            {\n                const HRESULT acquireHr = device_->Acquire();\n                if (FAILED(acquireHr))\n                    return;\n                deviceAcquired_ = true;\n                device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n            }\n''',
'''            if (device_ && !deviceAcquired_)\n            {\n                const HRESULT acquireHr = device_->Acquire();\n                if (FAILED(acquireHr) && acquireHr != S_FALSE)\n                {\n                    note_device_failure("gameplay Acquire", acquireHr);\n                    return;\n                }\n                deviceAcquired_ = true;\n                clear_device_failure();\n                const HRESULT actuatorHr =\n                    device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n                if (FAILED(actuatorHr))\n                    note_device_failure("SETACTUATORSON", actuatorHr);\n            }\n''',
'gameplay acquire recovery')

needle = '''        bool initialize()\n        {\n'''
if text.count(needle) != 1:
    raise SystemExit('recovery helper insertion point not found exactly once')

helpers = r'''        void clear_device_failure()
        {
            deviceFailureSince_ = 0;
        }

        void request_device_reinitialize(const char* reason, HRESULT hr)
        {
            if (panicStopped_ || deviceReinitPending_)
                return;

            deviceReinitPending_ = true;
            deviceReinitAfter_ = GetTickCount();
            spdlog::warn(
                "WheelFFB: scheduling DirectInput device reinitialization after {} (0x{:08X})",
                reason, (unsigned)hr);
        }

        void note_device_failure(const char* where, HRESULT hr)
        {
            if (panicStopped_ || !initialized_)
                return;

            const DWORD now = GetTickCount();
            if (deviceFailureSince_ == 0)
            {
                deviceFailureSince_ = now;
                spdlog::warn(
                    "WheelFFB: DirectInput device access failed at {} (0x{:08X}); allowing {}ms for transient recovery",
                    where, (unsigned)hr, (unsigned)FFB_DEVICE_FAILURE_GRACE_MS);
                return;
            }

            if (now - deviceFailureSince_ >= FFB_DEVICE_FAILURE_GRACE_MS)
                request_device_reinitialize(where, hr);
        }

        bool reacquire_after_input_loss(const char* where, HRESULT originalHr)
        {
            if (!device_)
            {
                note_device_failure(where, originalHr);
                return false;
            }

            deviceAcquired_ = false;
            const HRESULT acquireHr = device_->Acquire();
            if (SUCCEEDED(acquireHr) || acquireHr == S_FALSE)
            {
                deviceAcquired_ = true;
                clear_device_failure();
                return true;
            }

            note_device_failure(where, acquireHr);
            return false;
        }

        void release_effects_for_reinitialize()
        {
            safe_release_effect(constantEffect_, "constant during device reinit");
            safe_release_effect(springEffect_, "spring during device reinit");
            safe_release_effect(damperEffect_, "damper during device reinit");
            safe_release_effect(roadTextureEffect_, "road during device reinit");
            safe_release_effect(tireSlipEffect_, "tire during device reinit");

            roadState_ = {};
            slipState_ = {};
            periodicsActive_ = false;
            springStrategy_ = 1;
            damperStrategy_ = 1;
            periodicStrategy_ = 1;
        }

        void teardown_for_reinitialize()
        {
            if (device_)
            {
                __try
                {
                    device_->SendForceFeedbackCommand(DISFFC_STOPALL);
                    device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSOFF);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    spdlog::warn(
                        "WheelFFB: exception stopping stale DirectInput device during reinit (0x{:X})",
                        GetExceptionCode());
                }
            }

            release_effects_for_reinitialize();

            if (device_)
            {
                __try
                {
                    device_->Unacquire();
                    deviceAcquired_ = false;

                    DIPROPDWORD autocenter{};
                    autocenter.diph.dwSize = sizeof(autocenter);
                    autocenter.diph.dwHeaderSize = sizeof(DIPROPHEADER);
                    autocenter.diph.dwObj = 0;
                    autocenter.diph.dwHow = DIPH_DEVICE;
                    autocenter.dwData = DIPROPAUTOCENTER_ON;
                    device_->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    deviceAcquired_ = false;
                }
            }

            release_device();
            release_directinput();
            selectedName_.clear();
            selectedGuid_ = {};
            lastEffectGain_ = 0xFFFFFFFFu;
            nextGainRetryTick_ = 0;
            recreateHoldoffUntil_ = 0;
            springRecreateHoldoffUntil_ = 0;
            damperRecreateHoldoffUntil_ = 0;
            reset_signal_state();
        }

'''
text = text.replace(needle, helpers + needle, 1)

rep(
'''            initialized_ = true;\n            reset_signal_state();\n\n            lastEffectGain_ = configured_effect_gain();\n''',
'''            initialized_ = true;\n            deviceReinitPending_ = false;\n            deviceFailureSince_ = 0;\n            deviceReinitAfter_ = 0;\n            reset_signal_state();\n\n            lastEffectGain_ = configured_effect_gain();\n''',
'reset recovery state after initialization')

rep(
'''                if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n                {\n                    if (SUCCEEDED(device_->Acquire()))\n                        deviceAcquired_ = true;\n                    hr = effect->SetParameters(&params, DIEP_GAIN);\n                }\n''',
'''                if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n                {\n                    if (reacquire_after_input_loss("live gain", hr))\n                        hr = effect->SetParameters(&params, DIEP_GAIN);\n                }\n''',
'live gain input-loss recovery')

rep(
'''            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n            {\n                if (SUCCEEDED(device_->Acquire()))\n                    deviceAcquired_ = true;\n                hr = springEffect_->SetParameters(\n                    &params, DIEP_TYPESPECIFICPARAMS | DIEP_START);\n            }\n''',
'''            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n            {\n                if (reacquire_after_input_loss("GUID_Spring", hr))\n                    hr = springEffect_->SetParameters(\n                        &params, DIEP_TYPESPECIFICPARAMS | DIEP_START);\n            }\n''',
'spring input-loss recovery')

rep(
'''            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n            {\n                if (SUCCEEDED(device_->Acquire()))\n                    deviceAcquired_ = true;\n                hr = damperEffect_->SetParameters(\n                    &params, DIEP_TYPESPECIFICPARAMS | DIEP_START);\n            }\n''',
'''            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n            {\n                if (reacquire_after_input_loss("GUID_Damper", hr))\n                    hr = damperEffect_->SetParameters(\n                        &params, DIEP_TYPESPECIFICPARAMS | DIEP_START);\n            }\n''',
'damper input-loss recovery')

rep(
'''            if (FAILED(hr))\n            {\n                spdlog::warn(\n                    "WheelFFB: periodic update failed (0x{:08X}); falling back to ConstantForce vibration",\n''',
'''            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n            {\n                if (reacquire_after_input_loss("GUID_Sine periodic", hr))\n                    hr = effect->SetParameters(\n                        &params, DIEP_TYPESPECIFICPARAMS | DIEP_START);\n            }\n\n            if (FAILED(hr))\n            {\n                spdlog::warn(\n                    "WheelFFB: periodic update failed (0x{:08X}); falling back to ConstantForce vibration",\n''',
'periodic input-loss recovery')

rep(
'''            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n            {\n                if (SUCCEEDED(device_->Acquire()))\n                    deviceAcquired_ = true;\n                if (constantEffect_)\n                {\n                    hr = constantEffect_->SetParameters(\n                        &params, DIEP_TYPESPECIFICPARAMS | DIEP_START);\n                }\n            }\n''',
'''            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n            {\n                if (reacquire_after_input_loss("ConstantForce", hr) && constantEffect_)\n                {\n                    hr = constantEffect_->SetParameters(\n                        &params, DIEP_TYPESPECIFICPARAMS | DIEP_START);\n                }\n            }\n''',
'constant-force input-loss recovery')

rep(
'''            prevSpringCoefficient_ = coefficient;\n            prevSpringSaturation_ = saturation;\n''',
'''            clear_device_failure();\n            prevSpringCoefficient_ = coefficient;\n            prevSpringSaturation_ = saturation;\n''',
'spring success clears transient fault')
rep(
'''            prevDamperCoefficient_ = coefficient;\n''',
'''            clear_device_failure();\n            prevDamperCoefficient_ = coefficient;\n''',
'damper success clears transient fault')
rep(
'''            state.lastMagnitude = mag;\n            state.lastPeriod = period;\n''',
'''            clear_device_failure();\n            state.lastMagnitude = mag;\n            state.lastPeriod = period;\n''',
'periodic success clears transient fault')
rep(
'''            prevConstantLevel_ = requestedLevel;\n''',
'''            clear_device_failure();\n            prevConstantLevel_ = requestedLevel;\n''',
'constant success clears transient fault')

rep(
'''        DWORD nextGainRetryTick_ = 0;\n''',
'''        DWORD nextGainRetryTick_ = 0;\n        DWORD deviceFailureSince_ = 0;\n        DWORD deviceReinitAfter_ = 0;\n        bool deviceReinitPending_ = false;\n''',
'device recovery state')

p.write_text(text, encoding='utf-8')
print('Applied DirectInput device-loss/replug recovery state machine')
