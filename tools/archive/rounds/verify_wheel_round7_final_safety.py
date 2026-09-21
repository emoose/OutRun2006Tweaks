from pathlib import Path

p = Path('src/hooks_wheel_ffb.cpp')
s = p.read_text(encoding='utf-8')

checks = {
    'background branch resets transient state': '''zero_all_forces();\n                        reset_signal_state();''',
    'direct foreground torque guard': '''if (initialized_ && gameHwnd_ && GetForegroundWindow() != gameHwnd_)''',
    'manual test zeros road periodic': '''update_periodic(roadTextureEffect_, roadState_, 0.0f, 30.0f);''',
    'manual test zeros tire periodic': '''update_periodic(tireSlipEffect_, slipState_, 0.0f, 35.0f);''',
    'manual test completion resets signal history': '''if (manualTestFrames_ == 0)\n                {\n                    zero_all_forces();\n                    reset_signal_state();''',
    'test requires enabled FFB': '''if (!Settings::WheelFFBEnable || !initialized_ || panicStopped_ ||''',
    'test requires active exact device': '''!device_ || !deviceAcquired_ || !appActive_ || !gameHwnd_ ||''',
    'inactive test request explicitly rejected': '''ignored direction test while FFB device was not active and foreground; no torque was queued''',
    'cancel only zeroes an actual pending test': '''const bool hadPendingTest = manualTestFrames_ > 0;''',
    'WM_ACTIVATEAPP resets transient state': '''self->zero_all_forces();\n                        self->reset_signal_state();''',
}
for label, needle in checks.items():
    if needle not in s:
        raise SystemExit(f'ROUND7 VERIFY FAILED [{label}]')
    print(f'ROUND7 VERIFY OK [{label}]')

# Lightweight delimiter balance catches the class of compiler failure that
# escaped earlier string-only verification. Ignore strings/comments enough for
# this generated source by using a tiny lexer.
def strip_noncode(text: str) -> str:
    out=[]; i=0; n=len(text); state='code'
    while i<n:
        c=text[i]; d=text[i+1] if i+1<n else ''
        if state=='code':
            if c=='/' and d=='/': state='line'; out.extend('  '); i+=2; continue
            if c=='/' and d=='*': state='block'; out.extend('  '); i+=2; continue
            if c=='"': state='str'; out.append(' '); i+=1; continue
            if c=="'": state='char'; out.append(' '); i+=1; continue
            out.append(c); i+=1; continue
        if state=='line':
            if c=='\n': state='code'; out.append('\n')
            else: out.append(' ')
            i+=1; continue
        if state=='block':
            if c=='*' and d=='/': state='code'; out.extend('  '); i+=2
            else: out.append('\n' if c=='\n' else ' '); i+=1
            continue
        if state in ('str','char'):
            if c=='\\' and i+1<n: out.extend('  '); i+=2; continue
            end='"' if state=='str' else "'"
            if c==end: state='code'
            out.append(' '); i+=1
    return ''.join(out)

code=strip_noncode(s)
pairs={')':'(',']':'[','}':'{'}; stack=[]
for pos,c in enumerate(code):
    if c in '([{': stack.append((c,pos))
    elif c in pairs:
        if not stack or stack[-1][0]!=pairs[c]:
            raise SystemExit(f'ROUND7 VERIFY FAILED [delimiter mismatch at {pos}]')
        stack.pop()
if stack:
    raise SystemExit(f'ROUND7 VERIFY FAILED [unclosed delimiter {stack[-1]}]')
print('ROUND7 VERIFY OK [C++ delimiter balance]')
print('Round-7 final DD-wheel runtime safety verification passed')
