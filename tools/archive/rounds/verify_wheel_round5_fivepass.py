from pathlib import Path


def read(path: str) -> str:
    return Path(path).read_text(encoding='utf-8')


def require(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle not in data:
        raise SystemExit(f'ROUND5 VERIFY FAILED [{label}]: {needle!r} missing from {path}')
    print(f'ROUND5 VERIFY OK [{label}]')


def forbid(path: str, needle: str, label: str) -> None:
    data = read(path)
    if needle in data:
        raise SystemExit(f'ROUND5 VERIFY FAILED [{label}]: stale {needle!r} remains in {path}')
    print(f'ROUND5 VERIFY OK [{label}]')


def verify_cpp_delimiters(path: str) -> None:
    """Balance (), {}, [] while ignoring C/C++ comments and quoted literals."""
    s = read(path)
    stack = []
    pairs = {')': '(', '}': '{', ']': '['}
    opens = set(pairs.values())
    i = 0
    state = 'code'
    while i < len(s):
        ch = s[i]
        nxt = s[i + 1] if i + 1 < len(s) else ''
        if state == 'code':
            if ch == '/' and nxt == '/':
                state = 'line_comment'; i += 2; continue
            if ch == '/' and nxt == '*':
                state = 'block_comment'; i += 2; continue
            if ch == '"':
                state = 'string'; i += 1; continue
            if ch == "'":
                state = 'char'; i += 1; continue
            if ch in opens:
                stack.append((ch, i))
            elif ch in pairs:
                if not stack or stack[-1][0] != pairs[ch]:
                    raise SystemExit(
                        f'ROUND5 VERIFY FAILED [delimiter balance]: unexpected {ch!r} at offset {i} in {path}')
                stack.pop()
        elif state == 'line_comment':
            if ch == '\n':
                state = 'code'
        elif state == 'block_comment':
            if ch == '*' and nxt == '/':
                state = 'code'; i += 2; continue
        elif state in ('string', 'char'):
            quote = '"' if state == 'string' else "'"
            if ch == '\\':
                i += 2; continue
            if ch == quote:
                state = 'code'
        i += 1

    if state == 'block_comment':
        raise SystemExit(f'ROUND5 VERIFY FAILED [delimiter balance]: unterminated block comment in {path}')
    if stack:
        ch, pos = stack[-1]
        raise SystemExit(
            f'ROUND5 VERIFY FAILED [delimiter balance]: unclosed {ch!r} from offset {pos} in {path}')
    print(f'ROUND5 VERIFY OK [C++ delimiter balance: {path}]')


ffb = 'src/hooks_wheel_ffb.cpp'
ims = 'src/input_manager.hpp'

# Regression for the build-blocker caught by the previous real MSVC compile.
for deadline, label in [
    ('recreateHoldoffUntil_', 'periodic deadline outer parenthesis'),
    ('springRecreateHoldoffUntil_', 'spring deadline outer parenthesis'),
    ('damperRecreateHoldoffUntil_', 'damper deadline outer parenthesis'),
]:
    require(ffb, f'tick_reached(GetTickCount(), {deadline}))', label)
    forbid(ffb, f'tick_reached(GetTickCount(), {deadline})\n            {{', f'no broken {label}')
verify_cpp_delimiters(ffb)
verify_cpp_delimiters(ims)

# PASS 1: post-selection interface failures are actionable and capabilities are
# checked with their actual HRESULT.
require(ffb, 'hr = device_->GetCapabilities(&caps);', 'GetCapabilities HRESULT captured')
require(ffb, 'mark_selected_interface_failed("GetCapabilities", hr);', 'GetCapabilities sibling fallback')
require(ffb, 'mark_selected_interface_failed("CreateDevice", failHr);', 'CreateDevice sibling fallback')
require(ffb, 'mark_selected_interface_failed("SetDataFormat", hr);', 'SetDataFormat sibling fallback')
require(ffb, 'mark_selected_interface_failed("SetCooperativeLevel", hr);', 'cooperative-level sibling fallback')
require(ffb, 'mark_selected_interface_failed("Acquire", hr);', 'Acquire sibling fallback')
require(ffb, 'mark_selected_interface_failed("initial SETACTUATORSON", actuatorOnHr);', 'actuator sibling fallback')

# PASS 2: driver autocenter ownership is explicit and restored even on partial
# initialization / reinitialize release paths.
require(ffb, 'bool driverAutocenterDisabled_ = false;', 'autocenter ownership state')
require(ffb, 'driverAutocenterDisabled_ = true;', 'autocenter disable ownership acquired')
require(ffb, 'void restore_driver_autocenter(const char* where)', 'shared autocenter restore helper')
require(ffb, 'restore_driver_autocenter("device reinitialize");', 'reinitialize restores autocenter')
require(ffb, 'restore_driver_autocenter("device release");', 'all device release restores autocenter')
require(ffb, 'driverAutocenterDisabled_ = false;\n            spdlog::info("WheelFFB: PanicStop autocenter restore', 'PanicStop clears autocenter ownership')

# PASS 3: BACKGROUND cooperative mode still has explicit focus safety around
# every input-loss reacquisition.
require(ffb, 'if (!appActive_ || !gameHwnd_ || GetForegroundWindow() != gameHwnd_)', 'pre-Acquire foreground gate')
require(ffb, 'if (!appActive_ || GetForegroundWindow() != gameHwnd_)\n                {\n                    device_->Unacquire();', 'post-Acquire focus race gate')

# PASS 4: COM root/device state is invalidated before risky Release calls.
require(ffb, 'IDirectInput8A* staleDirectInput = directInput_;\n            directInput_ = nullptr;', 'DirectInput root state-first release')
require(ffb, 'WheelFFB: exception releasing DirectInput root object', 'DirectInput root release exception guard')
require(ffb, 'IDirectInputDevice8A* staleDevice = device_;\n            device_ = nullptr;', 'device state-first release')

# PASS 5: raw identical-device keys cannot collide after hotplug, and removing a
# controller before the primary keeps the same primary controller selected.
require(ims, '#include <algorithm>', 'explicit algorithm dependency')
require(ims, 'while (std::any_of(devices.begin(), devices.end(), [&guid, occurrence]', 'lowest-unused occurrence allocation')
require(ims, 'device.guid == guid && device.occurrence == occurrence;', 'occurrence collision test')
forbid(ims, 'const int occurrence = int(std::count_if(devices.begin(), devices.end()', 'no count-based duplicate occurrence allocator')
require(ims, 'const int removedIndex = int(std::distance(controllers.begin(), it));', 'removed controller index captured')
require(ims, 'else if (primaryControllerIndex > removedIndex)\n\t\t\t{\n\t\t\t\t--primaryControllerIndex;', 'primary index shifted after earlier removal')

print('Round-5 five-pass source hardening verification passed')
