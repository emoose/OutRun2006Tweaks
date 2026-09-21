from pathlib import Path
p=Path('src/hooks_wheel_ffb.cpp')
s=p.read_text(encoding='utf-8')

def rep(old,new,label):
    global s
    if old not in s:
        raise SystemExit(f'missing {label}')
    s=s.replace(old,new,1)
    print('patched:',label)

# PASS 1: wrap-safe DWORD deadline comparisons.
rep('''    constexpr DWORD FFB_DEVICE_RETRY_MS = 750;\n''','''    constexpr DWORD FFB_DEVICE_RETRY_MS = 750;\n\n    // GetTickCount wraps roughly every 49.7 days. Compare deadlines by signed\n    // subtraction so retry/holdoff gates remain correct across the wrap.\n    bool tick_before(DWORD now, DWORD deadline)\n    {\n        return deadline != 0 && static_cast<LONG>(now - deadline) < 0;\n    }\n\n    bool tick_reached(DWORD now, DWORD deadline)\n    {\n        return deadline == 0 || static_cast<LONG>(now - deadline) >= 0;\n    }\n''','tick helpers')
for old,new,label in [
('if (updateNow < deviceReinitAfter_)','if (tick_before(updateNow, deviceReinitAfter_))','device reinit deadline'),
('if (GetTickCount() < retryAfter_)','if (tick_before(GetTickCount(), retryAfter_))','initialize retry deadline'),
('GetTickCount() >= recreateHoldoffUntil_)','tick_reached(GetTickCount(), recreateHoldoffUntil_))','periodic holdoff'),
('GetTickCount() >= springRecreateHoldoffUntil_)','tick_reached(GetTickCount(), springRecreateHoldoffUntil_))','spring holdoff'),
('GetTickCount() >= damperRecreateHoldoffUntil_)','tick_reached(GetTickCount(), damperRecreateHoldoffUntil_))','damper holdoff'),
('if (nextGainRetryTick_ != 0 && now < nextGainRetryTick_)','if (tick_before(now, nextGainRetryTick_))','gain retry deadline'),
('if (now < recreateHoldoffUntil_)','if (tick_before(now, recreateHoldoffUntil_))','constant recreate holdoff'),
('GetTickCount() < ctx->self->failedInterfaceUntil_','tick_before(GetTickCount(), ctx->self->failedInterfaceUntil_)','failed sibling holdoff'),
]:
    rep(old,new,label)

# PASS 2: do not assume foreground ownership when installing after game already lost focus.
rep('''            gameHwnd_ = Game::GameHwnd();\n            if (!gameHwnd_)\n''','''            gameHwnd_ = Game::GameHwnd();\n            if (!gameHwnd_)\n''','game hwnd anchor')
rep('''                release_directinput();\n                return false;\n            }\n\n            hr = device_->SetCooperativeLevel(\n''','''                release_directinput();\n                return false;\n            }\n\n            // initialize() can run while the game is already in the background,\n            // before our WM_ACTIVATEAPP subclass has seen a focus transition.\n            // Seed appActive_ from the real foreground window so an exclusive\n            // DD device is never acquired merely because the default was true.\n            appActive_ = (GetForegroundWindow() == gameHwnd_);\n            if (!appActive_)\n            {\n                release_device();\n                release_directinput();\n                retryAfter_ = GetTickCount() + FFB_DEVICE_RETRY_MS;\n                return false;\n            }\n\n            hr = device_->SetCooperativeLevel(\n''','foreground seed')

# PASS 3: reacquire must restore actuator state too.
rep('''            if (SUCCEEDED(acquireHr) || acquireHr == S_FALSE)\n            {\n                deviceAcquired_ = true;\n                clear_device_failure();\n                return true;\n            }\n''','''            if (SUCCEEDED(acquireHr) || acquireHr == S_FALSE)\n            {\n                deviceAcquired_ = true;\n                const HRESULT actuatorHr =\n                    device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n                if (FAILED(actuatorHr))\n                {\n                    device_->Unacquire();\n                    deviceAcquired_ = false;\n                    note_device_failure("reacquire SETACTUATORSON", actuatorHr);\n                    return false;\n                }\n                clear_device_failure();\n                return true;\n            }\n''','reacquire actuator restore')

# PASS 4: persistent type-specific buffers. DirectInput documents that these
# buffers must remain valid for effect lifetime / until replaced.
rep('''        IDirectInputEffect* tireSlipEffect_ = nullptr;\n        std::vector<DWORD> actuatorAxes_;\n''','''        IDirectInputEffect* tireSlipEffect_ = nullptr;\n\n        // DirectInput does not promise to copy lpvTypeSpecificParams. Keep the\n        // backing structures alive for as long as their effects exist.\n        DICONSTANTFORCE constantParams_{};\n        DICONDITION springParams_{};\n        DICONDITION damperParams_{};\n        DIPERIODIC roadPeriodicParams_{};\n        DIPERIODIC tireSlipPeriodicParams_{};\n\n        std::vector<DWORD> actuatorAxes_;\n''','persistent effect buffers')

rep('''            DICONSTANTFORCE cf{};\n            cf.lMagnitude = 0;\n''','''            constantParams_ = {};\n            constantParams_.lMagnitude = 0;\n''','constant create buffer')
rep('''            effect.cbTypeSpecificParams = sizeof(cf);\n            effect.lpvTypeSpecificParams = &cf;\n''','''            effect.cbTypeSpecificParams = sizeof(constantParams_);\n            effect.lpvTypeSpecificParams = &constantParams_;\n''','constant create pointer')
rep('''            DICONSTANTFORCE cf{};\n            LONG directions[2] = { 1L, 0L };\n''','''            constantParams_ = {};\n            LONG directions[2] = { 1L, 0L };\n''','constant update buffer')
rep('''            params.cbTypeSpecificParams = sizeof(cf);\n            params.lpvTypeSpecificParams = &cf;\n''','''            params.cbTypeSpecificParams = sizeof(constantParams_);\n            params.lpvTypeSpecificParams = &constantParams_;\n''','constant update pointer')
rep('cf.lMagnitude = std::abs(requestedLevel);','constantParams_.lMagnitude = std::abs(requestedLevel);','constant polar magnitude')
rep('cf.lMagnitude = requestedLevel;','constantParams_.lMagnitude = requestedLevel;','constant cart magnitude')

rep('''            DICONDITION condition{};\n            condition.lOffset = 0;\n            condition.lPositiveCoefficient = 0;\n            condition.lNegativeCoefficient = 0;\n            condition.dwPositiveSaturation = saturation;\n            condition.dwNegativeSaturation = saturation;\n            condition.lDeadBand = 0;\n''','''            springParams_ = {};\n            springParams_.lOffset = 0;\n            springParams_.lPositiveCoefficient = 0;\n            springParams_.lNegativeCoefficient = 0;\n            springParams_.dwPositiveSaturation = saturation;\n            springParams_.dwNegativeSaturation = saturation;\n            springParams_.lDeadBand = 0;\n''','spring create buffer')
rep('''            effect.cbTypeSpecificParams = sizeof(condition);\n            effect.lpvTypeSpecificParams = &condition;\n''','''            effect.cbTypeSpecificParams = sizeof(springParams_);\n            effect.lpvTypeSpecificParams = &springParams_;\n''','spring create pointer')
rep('''            DICONDITION condition{};\n            condition.lOffset = 0;\n            condition.lPositiveCoefficient = coefficient;\n            condition.lNegativeCoefficient = coefficient;\n            condition.dwPositiveSaturation = saturation;\n            condition.dwNegativeSaturation = saturation;\n            condition.lDeadBand = 0;\n\n            DIEFFECT params{};\n            params.dwSize = sizeof(params);\n            params.cbTypeSpecificParams = sizeof(condition);\n            params.lpvTypeSpecificParams = &condition;\n''','''            springParams_ = {};\n            springParams_.lOffset = 0;\n            springParams_.lPositiveCoefficient = coefficient;\n            springParams_.lNegativeCoefficient = coefficient;\n            springParams_.dwPositiveSaturation = saturation;\n            springParams_.dwNegativeSaturation = saturation;\n            springParams_.lDeadBand = 0;\n\n            DIEFFECT params{};\n            params.dwSize = sizeof(params);\n            params.cbTypeSpecificParams = sizeof(springParams_);\n            params.lpvTypeSpecificParams = &springParams_;\n''','spring update buffer')

rep('''            DICONDITION condition{};\n            condition.lOffset = 0;\n            condition.lPositiveCoefficient = 0;\n            condition.lNegativeCoefficient = 0;\n            condition.dwPositiveSaturation = DI_FFNOMINALMAX;\n            condition.dwNegativeSaturation = DI_FFNOMINALMAX;\n            condition.lDeadBand = 0;\n''','''            damperParams_ = {};\n            damperParams_.lOffset = 0;\n            damperParams_.lPositiveCoefficient = 0;\n            damperParams_.lNegativeCoefficient = 0;\n            damperParams_.dwPositiveSaturation = DI_FFNOMINALMAX;\n            damperParams_.dwNegativeSaturation = DI_FFNOMINALMAX;\n            damperParams_.lDeadBand = 0;\n''','damper create buffer')
rep('''            effect.cbTypeSpecificParams = sizeof(condition);\n            effect.lpvTypeSpecificParams = &condition;\n''','''            effect.cbTypeSpecificParams = sizeof(damperParams_);\n            effect.lpvTypeSpecificParams = &damperParams_;\n''','damper create pointer')
rep('''            DICONDITION condition{};\n            condition.lOffset = 0;\n            condition.lPositiveCoefficient = coefficient;\n            condition.lNegativeCoefficient = coefficient;\n            condition.dwPositiveSaturation = DI_FFNOMINALMAX;\n            condition.dwNegativeSaturation = DI_FFNOMINALMAX;\n            condition.lDeadBand = 0;\n\n            DIEFFECT params{};\n            params.dwSize = sizeof(params);\n            params.cbTypeSpecificParams = sizeof(condition);\n            params.lpvTypeSpecificParams = &condition;\n''','''            damperParams_ = {};\n            damperParams_.lOffset = 0;\n            damperParams_.lPositiveCoefficient = coefficient;\n            damperParams_.lNegativeCoefficient = coefficient;\n            damperParams_.dwPositiveSaturation = DI_FFNOMINALMAX;\n            damperParams_.dwNegativeSaturation = DI_FFNOMINALMAX;\n            damperParams_.lDeadBand = 0;\n\n            DIEFFECT params{};\n            params.dwSize = sizeof(params);\n            params.cbTypeSpecificParams = sizeof(damperParams_);\n            params.lpvTypeSpecificParams = &damperParams_;\n''','damper update buffer')

old='''            DIPERIODIC periodic{};\n            periodic.dwMagnitude = 0;\n            periodic.lOffset = 0;\n            periodic.dwPhase = 0;\n            periodic.dwPeriod = static_cast<DWORD>(1000000.0f / initialHz);\n'''
new='''            DIPERIODIC& periodic = (std::strcmp(label, "RoadTexture") == 0)\n                ? roadPeriodicParams_\n                : tireSlipPeriodicParams_;\n            periodic = {};\n            periodic.dwMagnitude = 0;\n            periodic.lOffset = 0;\n            periodic.dwPhase = 0;\n            periodic.dwPeriod = static_cast<DWORD>(1000000.0f / initialHz);\n'''
rep(old,new,'periodic create persistent buffer')
old='''            DIPERIODIC periodic{};\n            periodic.dwMagnitude = mag;\n            periodic.dwPeriod = period;\n'''
new='''            DIPERIODIC& periodic = (effect == roadTextureEffect_)\n                ? roadPeriodicParams_\n                : tireSlipPeriodicParams_;\n            periodic = {};\n            periodic.dwMagnitude = mag;\n            periodic.dwPeriod = period;\n'''
rep(old,new,'periodic update persistent buffer')

rep('''                DICONSTANTFORCE cf{};\n                DIEFFECT eff{};\n                eff.dwSize = sizeof(eff);\n                eff.cbTypeSpecificParams = sizeof(cf);\n                eff.lpvTypeSpecificParams = &cf;\n''','''                constantParams_ = {};\n                DIEFFECT eff{};\n                eff.dwSize = sizeof(eff);\n                eff.cbTypeSpecificParams = sizeof(constantParams_);\n                eff.lpvTypeSpecificParams = &constantParams_;\n''','panic constant persistent buffer')

# PASS 5a: hook must exist even when config starts disabled, otherwise F11 live enable is impossible.
rep('''        bool validate() override\n        {\n            return Settings::WheelFFBEnable;\n        }\n''','''        bool validate() override\n        {\n            // Always install the lightweight update hook. The engine itself\n            // gates on WheelFFBEnable, which makes F11 enable/disable genuinely\n            // live even when the game was launched with Enable=false.\n            return true;\n        }\n''','startup-disabled live enable')

# PASS 5b: cancelling a test outside gameplay must not reacquire exclusive FFB.
rep('''            if (direction == 0)\n            {\n                manualTestFrames_ = 0;\n                manualTestDirection_ = 1;\n                if (initialized_ && !panicStopped_)\n                    set_constant_force(0);\n                return;\n            }\n''','''            if (direction == 0)\n            {\n                manualTestFrames_ = 0;\n                manualTestDirection_ = 1;\n                const bool inGameplay =\n                    Game::current_mode && (*Game::current_mode == STATE_GAME);\n                if (initialized_ && !panicStopped_ && deviceAcquired_ && inGameplay)\n                    set_constant_force(0);\n                return;\n            }\n''','direction cancel no menu reacquire')

# PASS 1b: menu helper retry timers use wrap-safe signed subtraction.
for menu_path, cls in [(Path("src/hooks_wheel_r3_menu_dpad.hpp"), "DPad"), (Path("src/hooks_wheel_r3_menu_ab.hpp"), "AB")]:
    m = menu_path.read_text(encoding="utf-8")
    old = "            if (now < retryAfter)\n                return false;"
    new = "            if (retryAfter != 0 && static_cast<LONG>(now - retryAfter) < 0)\n                return false;"
    if old not in m:
        raise SystemExit(f"missing {cls} wrap-safe retry gate")
    m = m.replace(old, new, 1)
    menu_path.write_text(m, encoding="utf-8")
    print("patched:", cls, "wrap-safe retry deadline")

p.write_text(s,encoding="utf-8")
print("Applied round-4 five-pass source hardening")