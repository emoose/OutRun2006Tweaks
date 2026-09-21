from pathlib import Path

p = Path('src/hooks_wheel_ffb.cpp')
s = p.read_text(encoding='utf-8')


def rep(old: str, new: str, label: str):
    global s
    n = s.count(old)
    if n != 1:
        raise SystemExit(f'ROUND7 {label}: expected exactly one match, got {n}')
    s = s.replace(old, new, 1)
    print(f'ROUND7 patched: {label}')

# 1) A focus-loss zero must also cancel transient event/test state. Otherwise a
# 20% direction test (or crash/gear impulse) can resume after Alt-Tab.
rep(
'''                else
                {
                    if (initialized_)
                        zero_all_forces();
                    return;
                }
            }

            const DWORD updateNow = GetTickCount();
''',
'''                else
                {
                    if (initialized_)
                    {
                        zero_all_forces();
                        reset_signal_state();
                    }
                    return;
                }
            }

            // WM_ACTIVATEAPP is normally authoritative, but keep a direct
            // foreground guard as a second DD-wheel safety net. A delayed or
            // missed activation message must never leave background torque
            // running merely because the device uses DISCL_BACKGROUND.
            if (initialized_ && gameHwnd_ && GetForegroundWindow() != gameHwnd_)
            {
                appActive_ = false;
                zero_all_forces();
                reset_signal_state();
                if (device_ && deviceAcquired_)
                {
                    device_->Unacquire();
                    deviceAcquired_ = false;
                }
                return;
            }

            const DWORD updateNow = GetTickCount();
''',
'focus-loss state cancellation and foreground safety net')

# 2) Make the 20% test a pure, bounded test. Hardware road/slip effects are
# independent effect objects, so they must be explicitly silenced while the
# ConstantForce test is running. Reset physics history at completion to avoid a
# false crash/damper kick caused by the intentionally skipped test frames.
rep(
'''            if (manualTestFrames_ > 0)
            {
                if (springEffect_)
                    update_spring(0.0f);
                if (damperEffect_)
                    update_damper(0.0f);

                LONG testLevel = manualTestDirection_ * 2000L;
                if (Settings::WheelFFBInvertForce)
                    testLevel = -testLevel;
                set_constant_force(testLevel);
                --manualTestFrames_;
                if (manualTestFrames_ == 0)
                {
                    set_constant_force(0);
                    warmupFrames_ = 0;
                }
                return;
            }
''',
'''            if (manualTestFrames_ > 0)
            {
                if (springEffect_)
                    update_spring(0.0f);
                if (damperEffect_)
                    update_damper(0.0f);
                if (roadTextureEffect_)
                    update_periodic(roadTextureEffect_, roadState_, 0.0f, 30.0f);
                if (tireSlipEffect_)
                    update_periodic(tireSlipEffect_, slipState_, 0.0f, 35.0f);

                crashImpulseTimer_ = 0;
                crashImpulseForce_ = 0.0f;
                gearShiftTimer_ = 0;
                splashTimer_ = 0;
                splashAmp_ = 0.0f;

                LONG testLevel = manualTestDirection_ * 2000L;
                if (Settings::WheelFFBInvertForce)
                    testLevel = -testLevel;
                set_constant_force(testLevel);
                --manualTestFrames_;
                if (manualTestFrames_ == 0)
                {
                    zero_all_forces();
                    reset_signal_state();
                }
                return;
            }
''',
'direction test silences periodic/event effects and resets history')

# 3) Never queue a future torque request. The test may only start while the
# exact FFB device is currently initialized, acquired, enabled and foreground.
rep(
'''        void request_direction_test(int direction)
        {
            if (direction == 0)
            {
                manualTestFrames_ = 0;
                manualTestDirection_ = 1;
                const bool inGameplay =
                    Game::current_mode && (*Game::current_mode == STATE_GAME);
                if (initialized_ && !panicStopped_ && deviceAcquired_ && inGameplay)
                    set_constant_force(0);
                return;
            }

            const bool inGameplay =
                Game::current_mode && (*Game::current_mode == STATE_GAME);
            if (!inGameplay)
            {
                manualTestFrames_ = 0;
                manualTestDirection_ = 1;
                spdlog::warn(
                    "WheelFFB: ignored direction test outside gameplay; no torque was queued");
                return;
            }

            manualTestDirection_ = direction < 0 ? -1 : 1;
            manualTestFrames_ = 18;
            spdlog::info(
                "WheelFFB: queued safe {} direction test at fixed 20% output",
                manualTestDirection_ < 0 ? "left" : "right");
        }
''',
'''        void request_direction_test(int direction)
        {
            if (direction == 0)
            {
                const bool hadPendingTest = manualTestFrames_ > 0;
                manualTestFrames_ = 0;
                manualTestDirection_ = 1;
                const bool inGameplay =
                    Game::current_mode && (*Game::current_mode == STATE_GAME);
                if (hadPendingTest && initialized_ && !panicStopped_ &&
                    deviceAcquired_ && inGameplay && appActive_ && gameHwnd_ &&
                    GetForegroundWindow() == gameHwnd_)
                {
                    zero_all_forces();
                }
                if (hadPendingTest)
                    reset_signal_state();
                return;
            }

            const bool inGameplay =
                Game::current_mode && (*Game::current_mode == STATE_GAME);
            if (!inGameplay)
            {
                manualTestFrames_ = 0;
                manualTestDirection_ = 1;
                spdlog::warn(
                    "WheelFFB: ignored direction test outside gameplay; no torque was queued");
                return;
            }

            if (!Settings::WheelFFBEnable || !initialized_ || panicStopped_ ||
                !device_ || !deviceAcquired_ || !appActive_ || !gameHwnd_ ||
                GetForegroundWindow() != gameHwnd_)
            {
                manualTestFrames_ = 0;
                manualTestDirection_ = 1;
                spdlog::warn(
                    "WheelFFB: ignored direction test while FFB device was not active and foreground; no torque was queued");
                return;
            }

            manualTestDirection_ = direction < 0 ? -1 : 1;
            manualTestFrames_ = 18;
            spdlog::info(
                "WheelFFB: queued safe {} direction test at fixed 20% output",
                manualTestDirection_ < 0 ? "left" : "right");
        }
''',
'direction test cannot queue while FFB is inactive/background')

# 4) WM_ACTIVATEAPP itself must cancel transient state before exclusive release.
rep(
'''                    if (!self->appActive_)
                    {
                        self->zero_all_forces();
                        if (self->device_ && self->deviceAcquired_)
''',
'''                    if (!self->appActive_)
                    {
                        self->zero_all_forces();
                        self->reset_signal_state();
                        if (self->device_ && self->deviceAcquired_)
''',
'WM_ACTIVATEAPP cancels pending test and stale physics state')

p.write_text(s, encoding='utf-8')
print('Applied round-7 final DD-wheel runtime safety hardening')
