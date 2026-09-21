from pathlib import Path
import math


def read(path):
    return Path(path).read_text(encoding='utf-8')

def req(path, needle, label):
    data=read(path)
    if needle not in data:
        raise SystemExit(f'ROUND6 VERIFY FAILED [{label}]: missing {needle!r}')
    print(f'ROUND6 VERIFY OK [{label}]')

def forbid(path, needle, label):
    data=read(path)
    if needle in data:
        raise SystemExit(f'ROUND6 VERIFY FAILED [{label}]: stale {needle!r}')
    print(f'ROUND6 VERIFY OK [{label}]')

ffb='src/hooks_wheel_ffb.cpp'
ims='src/input_manager.hpp'
setup='src/overlay/wheel_setup_ui.cpp'

# PASS 1 lifecycle/focus
req(ffb, 'if (!initialized_ && gameHwnd_ && GetForegroundWindow() == gameHwnd_)', 'background-startup state can recover on foreground')
req(ffb, 'if (!gameHwnd_ || GetForegroundWindow() != gameHwnd_)\n                    return;\n\n                const HRESULT acquireHr = device_->Acquire();', 'normal gameplay Acquire foreground precheck')
req(ffb, 'if (GetForegroundWindow() != gameHwnd_)\n                {\n                    device_->Unacquire();', 'normal gameplay Acquire foreground postcheck')
req(ffb, 'if (!inGameplay)\n            {\n                deviceAcquired_ = false;\n                return false;\n            }\n            if (!appActive_ || !gameHwnd_', 'input-loss reacquire gameplay gate')
if read(ffb).count('if (!Settings::WheelFFBUseHardwareSpring && springEffect_)') != 1:
    raise SystemExit('ROUND6 VERIFY FAILED [spring lifecycle duplicate]: expected one live-disable block')
print('ROUND6 VERIFY OK [spring lifecycle duplicate removed]')

# PASS 2 identity/hotplug
req(ffb, 'is unavailable; refusing DeviceName fallback', 'missing exact GUID cannot redirect torque')
req(ffb, 'was rejected during initialization; trying a sibling DeviceName interface', 'explicit sibling fallback retained')
req(ffb, 'failedInterfaceGuid_.clear();\n            failedInterfaceUntil_ = 0;', 'stale sibling rejection state cleared')
forbid(ffb, "unavailable/rejected; falling back to DeviceName", 'old unconditional FFB name fallback removed')
req(setup, 'if (index < 0)\n                    return false;', 'F11 reader refuses missing exact GUID')
req(ims, 'if (!binding.deviceSerial.empty())\n\t\t\treturn usbIdentityMatches && device.serial == binding.deviceSerial;', 'serial identity authoritative')
req(ims, 'if (!binding.devicePath.empty())\n\t\t\treturn usbIdentityMatches && device.path == binding.devicePath;', 'path identity authoritative')

# PASS 3 math/backend parity
req(ffb, ': -steer * springStrength;', 'software spring preserves load/grip model')
req(ffb, 'const float effectRampScale = warmupScale * recreateScale;', 'road/slip effect ramp scale')
req(ffb, 'roadAmp * effectRampScale', 'road texture ramped')
req(ffb, 'slipAmp * effectRampScale', 'tire slip ramped')

# Verify the intended speed curve and load/unload relationships numerically.
def curve(speed, low=.08):
    return max(0.0, min(1.0, low + (1.0-low) * speed**1.60))
vals=[curve(x) for x in (0.0,.25,.5,.75,1.0)]
if not (abs(vals[0]-.08)<1e-9 and .17 < vals[1] < .21 and .36 < vals[2] < .41 and .63 < vals[3] < .68 and abs(vals[4]-1.0)<1e-9):
    raise SystemExit(f'ROUND6 VERIFY FAILED [speed curve]: {vals}')
base=.60*curve(.8)
loaded=min(1.0, base*(1.0+.7*.35))
drift=min(1.0, base*(1.0+.7*.35)*(1.0-.65*.9))
if not loaded > base > drift:
    raise SystemExit(f'ROUND6 VERIFY FAILED [software spring load/grip parity]: {base}, {loaded}, {drift}')
print(f'ROUND6 VERIFY OK [speed/load/grip numeric]: curve={vals}, base={base:.4f}, loaded={loaded:.4f}, drift={drift:.4f}')

# PASS 4 finite safety
for needle,label in [
    ('std::isfinite(speedRaw)', 'speed finite guard'),
    ('std::isfinite(configuredStrength)', 'master gain finite guard'),
    ('std::isfinite(lateralSum)', 'lateral finite guard'),
    ('std::isfinite(surfaceRoughness)', 'surface finite guard'),
    ('if (!std::isfinite(total))\n                total = 0.0f;', 'final force finite guard'),
    ('std::isfinite(configuredSlew)', 'slew finite guard'),
    ('std::isfinite(strength)', 'condition strength finite guards'),
    ('std::isfinite(magnitude)', 'periodic magnitude finite guard'),
    ('std::isfinite(frequency)', 'periodic frequency finite guard'),
    ('!std::isfinite(amplitude)', 'fallback sine finite guard'),
]: req(ffb, needle, label)
if read(ffb).count('const float safeStrength = std::isfinite(strength)') != 2:
    raise SystemExit('ROUND6 VERIFY FAILED [spring/damper finite guards]: expected exactly two')
print('ROUND6 VERIFY OK [spring/damper finite guards both present]')

# PASS 5 F11 runtime ownership language
req(setup, 'gameplay FFB follows the exact selected DirectInput GUID.', 'F11 exact GUID ownership text')

# Lightweight C/C++ delimiter scanner that ignores strings/chars/comments. This
# specifically guards against the round4 unmatched-parenthesis regression before
# MSVC, while the real Actions compile remains authoritative.
def check_delimiters(text):
    pairs={')':'(',']':'[','}':'{'}; opens=set(pairs.values()); stack=[]
    i=0; state='code'
    while i < len(text):
        c=text[i]; n=text[i+1] if i+1<len(text) else ''
        if state=='code':
            if c=='/' and n=='/': state='line'; i+=2; continue
            if c=='/' and n=='*': state='block'; i+=2; continue
            if c=='"': state='str'; i+=1; continue
            if c=="'": state='char'; i+=1; continue
            if c in opens: stack.append((c,i))
            elif c in pairs:
                if not stack or stack[-1][0] != pairs[c]:
                    raise SystemExit(f'ROUND6 VERIFY FAILED [delimiter]: bad {c} at {i}')
                stack.pop()
        elif state=='line':
            if c=='\n': state='code'
        elif state=='block':
            if c=='*' and n=='/': state='code'; i+=2; continue
        elif state in ('str','char'):
            if c=='\\': i+=2; continue
            if (state=='str' and c=='"') or (state=='char' and c=="'"): state='code'
        i+=1
    if stack:
        raise SystemExit(f'ROUND6 VERIFY FAILED [delimiter]: unclosed {stack[-1]}')

for path in (ffb, ims, setup):
    check_delimiters(read(path))
    print(f'ROUND6 VERIFY OK [delimiter balance] {path}')

print('Round-6 five-pass verification passed')
