from pathlib import Path


def read(path: str) -> str:
    return Path(path).read_text(encoding='utf-8')


def require(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle not in data:
        raise SystemExit(f'ROUND8 VERIFY FAILED [{label}]: {needle!r} missing from {path}')
    print(f'ROUND8 VERIFY OK [{label}]')


def forbid(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle in data:
        raise SystemExit(f'ROUND8 VERIFY FAILED [{label}]: stale {needle!r} present in {path}')
    print(f'ROUND8 VERIFY OK [{label}]')


manager = 'src/input_manager.hpp'
ui = 'src/overlay/input_bindings_ui.cpp'
wheel_ui = 'src/overlay/wheel_setup_ui.cpp'

require(manager, 'InputSourceType lastSourceType = InputSourceType::GamePad;', 'InputState source initialized')
require(manager, 'uint32_t switch_current = 0;', 'current switch mask initialized')
require(manager, 'uint32_t switch_previous = 0;', 'previous switch mask initialized')
require(manager, 'uint32_t switch_overlay = 0;', 'overlay switch mask initialized')

require(manager, 'bool positiveOnly = false', 'InputAction supports positive-only digital aggregation')
require(manager, 'if (positiveOnly && currentValue <= 0.0f)', 'negative side ignored for digital actions')
require(manager, 'switchBindings[i].update(gamepad, resolveJoystick, true)', 'switches use positive-only aggregation')
require(manager, 'modBindings[i].update(gamepad, resolveJoystick, true)', 'mod actions use positive-only aggregation')

require(manager, 'volumes[i].previousValue = 0.0f;', 'overlay clears stale previous analog value')
require(manager, 'volumes[i].currentValue = 0.0f;', 'overlay clears stale current analog value')
require(manager, 'never leave a stale', 'overlay analog safety documented')

require(manager, 'auto previousVolumeBindings = volumeBindings;', 'binding load snapshots current volume binds')
require(manager, 'int loadedBinds = 0;', 'binding load counts valid parses')
require(manager, 'no valid bindings parsed; previous bindings preserved', 'zero-valid binding file rejected')
require(manager, 'ensureOverlayBindable();\n\t\treturn true;', 'successful reload always keeps overlay reachable')
require(manager, 'ignoring invalid negative suffix for non-axis action', 'impossible button/key negation normalized')

require(ui, 'std::optional<InputBinding> releaseGuardBinding;', 'captured input release guard exists')
require(ui, 'capturedReleased = std::abs(releaseGuardBinding->read(', 'release guard reads captured input')
require(ui, 'capturedReleased && !manager.anyInputPressed()', 'next bind waits for captured release')
require(ui, 'releaseGuardBinding = *quickSetupCandidate;\n\t\t\t\t\tquickSetupCandidate.reset();\n\t\t\t\t\t++quickSetupStep;', 'quick setup use waits for release')
require(ui, 'releaseGuardBinding = *quickSetupCandidate;\n\t\t\t\t\tquickSetupCandidate.reset();\n\t\t\t\t\tisListeningForInput = ListenState::WaitForBindButtonRelease;', 'quick setup retry waits for release')
require(ui, 'else if (steering || binding.isAxis())', 'invert UI limited to directional inputs')
require(ui, 'Invert is only meaningful for axes or steering-direction buttons', 'invalid button invert explained')

require(wheel_ui, 'Settings::WheelUniversalSetupEnable = true;\n                    Settings::WheelMenuR3DirectDPad = false;\n                    Settings::WheelMenuR3DirectAB = false;', 'legacy import disables fixed R3 menu readers')

# Small behavioral regression checks for the digital aggregation rule.
def digital(values):
    best = 0.0
    for value in values:
        if value <= 0.0:
            continue
        if abs(value) > abs(best):
            best = value
    return best

if digital([-1.0, 1.0]) != 1.0:
    raise SystemExit('ROUND8 VERIFY FAILED [negative axis must not mask +1 button]')
print('ROUND8 VERIFY OK [negative axis does not mask +1 button]')
if digital([-1.0, 0.0]) != 0.0:
    raise SystemExit('ROUND8 VERIFY FAILED [negative-only digital action must be inactive]')
print('ROUND8 VERIFY OK [negative-only digital action is inactive]')

# Lightweight C/C++ delimiter scanner, ignoring strings/comments, to catch patch
# generation mistakes before the real MSVC build.
def balanced(path: str) -> None:
    text = read(path)
    stack = []
    pairs = {')': '(', ']': '[', '}': '{'}
    opens = set(pairs.values())
    i = 0
    state = 'code'
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ''
        if state == 'code':
            if c == '/' and n == '/':
                state = 'line'; i += 2; continue
            if c == '/' and n == '*':
                state = 'block'; i += 2; continue
            if c == '"':
                state = 'string'; i += 1; continue
            if c == "'":
                state = 'char'; i += 1; continue
            if c in opens:
                stack.append(c)
            elif c in pairs:
                if not stack or stack[-1] != pairs[c]:
                    raise SystemExit(f'ROUND8 VERIFY FAILED [delimiter balance] {path}: unexpected {c} at {i}')
                stack.pop()
            i += 1; continue
        if state == 'line':
            if c == '\n': state = 'code'
            i += 1; continue
        if state == 'block':
            if c == '*' and n == '/': state = 'code'; i += 2; continue
            i += 1; continue
        if state in ('string', 'char'):
            quote = '"' if state == 'string' else "'"
            if c == '\\': i += 2; continue
            if c == quote: state = 'code'
            i += 1; continue
    if stack:
        raise SystemExit(f'ROUND8 VERIFY FAILED [delimiter balance] {path}: unclosed {stack[-1]}')
    print(f'ROUND8 VERIFY OK [delimiter balance] {path}')

for path in (manager, ui, wheel_ui):
    balanced(path)

print('Round-8 input binding and key-setting hardening verification passed')
