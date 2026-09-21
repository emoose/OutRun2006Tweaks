from pathlib import Path

path = Path("src/hooks_wheel_ffb.cpp")
text = path.read_text(encoding="utf-8")


def replace_once(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    text = text.replace(old, new, 1)


replace_once(
'''            if (!inGameplay)\n            {\n                zero_all_forces();\n                reset_signal_state();\n                return;\n            }\n\n            float steer = read_game_steering();\n''',
'''            if (!inGameplay)\n            {\n                zero_all_forces();\n                // Let the legacy/menu DirectInput reader reacquire the same\n                // physical wheel while menus or F11 setup are active. Effects\n                // remain created and are reused when gameplay resumes.\n                if (device_ && deviceAcquired_)\n                {\n                    device_->Unacquire();\n                    deviceAcquired_ = false;\n                }\n                reset_signal_state();\n                return;\n            }\n\n            if (device_ && !deviceAcquired_)\n            {\n                const HRESULT acquireHr = device_->Acquire();\n                if (FAILED(acquireHr))\n                    return;\n                deviceAcquired_ = true;\n                device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n            }\n\n            float steer = read_game_steering();\n''',
"menu/gameplay device handoff"
)

replace_once(
'''            hr = device_->Acquire();\n            if (FAILED(hr))\n            {\n                spdlog::error("WheelFFB: Acquire('{}') failed (0x{:08X})", selectedName_, (unsigned)hr);\n                release_device();\n                release_directinput();\n                return false;\n            }\n\n            device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n''',
'''            hr = device_->Acquire();\n            if (FAILED(hr))\n            {\n                spdlog::error("WheelFFB: Acquire('{}') failed (0x{:08X})", selectedName_, (unsigned)hr);\n                release_device();\n                release_directinput();\n                return false;\n            }\n            deviceAcquired_ = true;\n\n            device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);\n''',
"initialize acquired flag"
)

text = text.replace(
'''                device_->Acquire();\n                hr = springEffect_->SetParameters(''',
'''                if (SUCCEEDED(device_->Acquire()))\n                    deviceAcquired_ = true;\n                hr = springEffect_->SetParameters(''')
text = text.replace(
'''                device_->Acquire();\n                hr = damperEffect_->SetParameters(''',
'''                if (SUCCEEDED(device_->Acquire()))\n                    deviceAcquired_ = true;\n                hr = damperEffect_->SetParameters(''')
text = text.replace(
'''                device_->Acquire();\n                if (constantEffect_)''',
'''                if (SUCCEEDED(device_->Acquire()))\n                    deviceAcquired_ = true;\n                if (constantEffect_)''')

replace_once(
'''            hr = device_->Unacquire();\n            spdlog::info("WheelFFB: PanicStop Unacquire => 0x{:08X}", (unsigned)hr);\n''',
'''            hr = device_->Unacquire();\n            deviceAcquired_ = false;\n            spdlog::info("WheelFFB: PanicStop Unacquire => 0x{:08X}", (unsigned)hr);\n''',
"panic acquired flag"
)

replace_once(
'''            __try\n            {\n                device_->Unacquire();\n                device_->Release();\n''',
'''            __try\n            {\n                device_->Unacquire();\n                deviceAcquired_ = false;\n                device_->Release();\n''',
"release acquired flag"
)

replace_once(
'''        bool initialized_ = false;\n        bool panicStopped_ = false;\n        bool periodicsActive_ = false;\n''',
'''        bool initialized_ = false;\n        bool panicStopped_ = false;\n        bool deviceAcquired_ = false;\n        bool periodicsActive_ = false;\n''',
"acquired member"
)

path.write_text(text, encoding="utf-8")
print("Applied menu/gameplay DirectInput handoff patch")
