from pathlib import Path

p = Path('src/overlay/wheel_setup_ui.cpp')
text = p.read_text(encoding='utf-8')

def rep(old,new,label):
    global text
    c=text.count(old)
    if c!=1:
        raise SystemExit(f'{label}: expected 1 got {c}')
    text=text.replace(old,new,1)
    print('patched',label)

# Pass 9: limit switch interception to the universal menu actions we synthesize.
rep('''        inline static uint32_t menuHeld_ = 0;\n        inline static uint32_t menuPressed_ = 0;\n        inline static uint32_t previousMenuHeld_ = 0;\n        inline static DWORD lastApplyLog_ = 0;\n''','''        inline static uint32_t menuHeld_ = 0;\n        inline static uint32_t menuPressed_ = 0;\n        inline static uint32_t previousMenuHeld_ = 0;\n        inline static DWORD lastApplyLog_ = 0;\n\n        inline static constexpr uint32_t UniversalMenuSwitchMask =\n            (1u << int(SwitchId::Start)) |\n            (1u << int(SwitchId::Back)) |\n            (1u << int(SwitchId::A)) |\n            (1u << int(SwitchId::B)) |\n            (1u << int(SwitchId::GearDown)) |\n            (1u << int(SwitchId::GearUp)) |\n            (1u << int(SwitchId::SelectionUp)) |\n            (1u << int(SwitchId::SelectionDown)) |\n            (1u << int(SwitchId::SelectionLeft)) |\n            (1u << int(SwitchId::SelectionRight));\n\n        static bool pure_universal_menu_query(uint32_t switches)\n        {\n            return switches != 0 &&\n                (switches & UniversalMenuSwitchMask) != 0 &&\n                (switches & ~UniversalMenuSwitchMask) == 0;\n        }\n''','universal menu query mask')

rep('''        static int SwitchNow_dest(uint32_t switches)\n        {\n            const int result = SwitchNowHook.ccall<int>(switches);\n            if (result || !active())\n                return result;\n            return (menuHeld_ & switches) != 0 ? 1 : 0;\n        }\n\n        static int SwitchOn_dest(uint32_t switches)\n        {\n            const int result = SwitchOnHook.ccall<int>(switches);\n            if (result || !active())\n                return result;\n            return (menuPressed_ & switches) != 0 ? 1 : 0;\n        }\n''','''        static int SwitchNow_dest(uint32_t switches)\n        {\n            const int result = SwitchNowHook.ccall<int>(switches);\n            if (result || !active() || !pure_universal_menu_query(switches))\n                return result;\n            return (menuHeld_ & switches) == switches ? 1 : 0;\n        }\n\n        static int SwitchOn_dest(uint32_t switches)\n        {\n            const int result = SwitchOnHook.ccall<int>(switches);\n            if (result || !active() || !pure_universal_menu_query(switches))\n                return result;\n            return (menuPressed_ & switches) == switches ? 1 : 0;\n        }\n''','exact menu query semantics')

# Pass 10: clamp the saved legacy slot against the live regular-game devices.
rep('''            const int slot = std::clamp(int(Settings::WheelUniversalLegacyDeviceIndex), 0, 2);\n            auto* device = device_at(slot);\n            if (!device)\n                return;\n''','''            const int regularCount = std::max(device_count() - 1, 0);\n            if (regularCount <= 0)\n                return;\n            const int maxSlot = std::min(regularCount - 1, 2);\n            const int slot = std::clamp(\n                int(Settings::WheelUniversalLegacyDeviceIndex), 0, maxSlot);\n            if (slot != int(Settings::WheelUniversalLegacyDeviceIndex))\n                Settings::WheelUniversalLegacyDeviceIndex = slot;\n\n            auto* device = device_at(slot);\n            if (!device)\n                return;\n''','apply mapping slot clamp')

rep('''            const int slot = std::clamp(int(Settings::WheelUniversalLegacyDeviceIndex), 0, 2);\n            auto* device = device_at(slot);\n            if (!device)\n                return false;\n''','''            const int regularCount = std::max(device_count() - 1, 0);\n            if (regularCount <= 0)\n                return false;\n            const int maxSlot = std::min(regularCount - 1, 2);\n            const int slot = std::clamp(\n                int(Settings::WheelUniversalLegacyDeviceIndex), 0, maxSlot);\n            if (slot != int(Settings::WheelUniversalLegacyDeviceIndex))\n                Settings::WheelUniversalLegacyDeviceIndex = slot;\n\n            auto* device = device_at(slot);\n            if (!device)\n                return false;\n''','import mapping slot clamp')

rep('''                int currentSlot = std::clamp(\n                    int(Settings::WheelUniversalLegacyDeviceIndex), 0,\n                    std::min(regularSlots - 1, 2));\n''','''                int currentSlot = std::clamp(\n                    int(Settings::WheelUniversalLegacyDeviceIndex), 0,\n                    std::min(regularSlots - 1, 2));\n                if (currentSlot != int(Settings::WheelUniversalLegacyDeviceIndex))\n                    Settings::WheelUniversalLegacyDeviceIndex = currentSlot;\n''','UI slot clamp persistence')

p.write_text(text,encoding='utf-8')
print('Applied round-2 universal menu/legacy-slot hardening')
