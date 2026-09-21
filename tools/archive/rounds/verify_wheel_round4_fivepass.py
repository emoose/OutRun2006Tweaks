from pathlib import Path
ffb=Path('src/hooks_wheel_ffb.cpp').read_text(encoding='utf-8')
dpad=Path('src/hooks_wheel_r3_menu_dpad.hpp').read_text(encoding='utf-8')
ab=Path('src/hooks_wheel_r3_menu_ab.hpp').read_text(encoding='utf-8')
checks = [
    ('wrap-safe tick helper', 'bool tick_before(DWORD now, DWORD deadline)' in ffb),
    ('wrap-safe reached helper', 'bool tick_reached(DWORD now, DWORD deadline)' in ffb),
    ('device reinit deadline safe', 'tick_before(updateNow, deviceReinitAfter_)' in ffb),
    ('FFB retry deadline safe', 'tick_before(GetTickCount(), retryAfter_)' in ffb),
    ('failed sibling deadline safe', 'tick_before(GetTickCount(), ctx->self->failedInterfaceUntil_)' in ffb),
    ('foreground seeded before exclusive acquire', 'appActive_ = (GetForegroundWindow() == gameHwnd_);' in ffb),
    ('reacquire restores actuators', 'reacquire SETACTUATORSON' in ffb and 'SendForceFeedbackCommand(DISFFC_SETACTUATORSON)' in ffb),
    ('persistent constant params', 'DICONSTANTFORCE constantParams_{};' in ffb),
    ('persistent spring params', 'DICONDITION springParams_{};' in ffb),
    ('persistent damper params', 'DICONDITION damperParams_{};' in ffb),
    ('persistent road periodic params', 'DIPERIODIC roadPeriodicParams_{};' in ffb),
    ('persistent tire periodic params', 'DIPERIODIC tireSlipPeriodicParams_{};' in ffb),
    ('no local constant params', 'DICONSTANTFORCE cf{};' not in ffb),
    ('no local condition params', 'DICONDITION condition{};' not in ffb),
    ('no local periodic params', 'DIPERIODIC periodic{};' not in ffb),
    ('hook supports startup disabled', 'makes F11 enable/disable genuinely' in ffb and 'return true;' in ffb[ffb.find('class WheelFFBHook'):]),
    ('test cancel does not reacquire in menu', 'deviceAcquired_ && inGameplay' in ffb),
    ('DPad retry wrap safe', 'static_cast<LONG>(now - retryAfter) < 0' in dpad),
    ('AB retry wrap safe', 'static_cast<LONG>(now - retryAfter) < 0' in ab),
]
failed=[name for name,ok in checks if not ok]
for name,ok in checks:
    print(('PASS' if ok else 'FAIL'), name)
if failed:
    raise SystemExit('round4 five-pass verification failed: ' + ', '.join(failed))
print('Round-4 five-pass source hardening verification passed')
