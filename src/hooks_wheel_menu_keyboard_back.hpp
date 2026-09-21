#pragma once

#include <Windows.h>
#include <cstdint>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

// Legacy-wheel menu escape helper.
// The original game reuses the button bound to Gear Down as Back in many menus,
// including the legacy controller configuration screen. Some screens also query
// Back/B directly. Inject ESC at both the raw-state and logical-switch layers so
// a fully-unassigned wheel profile can always leave the configuration screen.
namespace Settings
{
    Setting<bool> WheelMenuKeyboardEscapeBack{
        "Controls", "WheelMenuKeyboardEscapeBack", true,
        "In legacy wheel mode, maps keyboard Escape to the game's Back/B/GearDown menu actions."
    };
}

namespace
{
    class WheelMenuKeyboardBack : public Hook
    {
        inline static constexpr uint32_t RawBackMask = 0x00000200u;
        inline static constexpr uint32_t RawBMask = 0x00000004u;
        inline static constexpr uint32_t RawEscapeMask = RawBackMask | RawBMask;
        inline static constexpr uint32_t BackSwitchMask = 1u << int(SwitchId::Back);
        inline static constexpr uint32_t BSwitchMask = 1u << int(SwitchId::B);
        inline static constexpr uint32_t GearDownSwitchMask = 1u << int(SwitchId::GearDown);

        inline static SafetyHookInline ReadIOHook = {};
        inline static SafetyHookInline SwitchNowHook = {};
        inline static SafetyHookInline SwitchOnHook = {};

        inline static bool escapeCurrent = false;
        inline static bool escapePrevious = false;
        inline static bool logged = false;

        static bool activeInMenu()
        {
            return Settings::WheelInputCompatibility &&
                !Settings::UseNewInput &&
                Settings::WheelMenuKeyboardEscapeBack &&
                Game::current_mode &&
                *Game::current_mode != STATE_GAME;
        }

        static bool escapeHeldNow()
        {
            return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        }

        static bool isBackQuery(uint32_t switches)
        {
            return switches == BackSwitchMask ||
                switches == BSwitchMask ||
                switches == GearDownSwitchMask;
        }

        static int ReadIO_dest()
        {
            const int result = ReadIOHook.ccall<int>();

            escapePrevious = escapeCurrent;
            escapeCurrent = activeInMenu() && escapeHeldNow();

            if (!activeInMenu() || !Game::dinput_state)
                return result;

            auto* state = Game::dinput_state;
            if (escapeCurrent)
                state->buttons_4 |= RawEscapeMask;

            if (escapeCurrent && !escapePrevious)
                state->pressed_8 |= RawEscapeMask;
            else if (!escapeCurrent && escapePrevious)
                state->released_C |= RawEscapeMask;

            return result;
        }

        static int SwitchNow_dest(uint32_t switches)
        {
            const int result = SwitchNowHook.ccall<int>(switches);
            if (result)
                return result;

            if (activeInMenu() && isBackQuery(switches) && escapeHeldNow())
            {
                if (!logged)
                {
                    logged = true;
                    spdlog::info("WheelMenuKeyboardEscapeBack: ESC mapped to legacy Back/B/GearDown menu action");
                }
                return 1;
            }
            return 0;
        }

        static int SwitchOn_dest(uint32_t switches)
        {
            const int result = SwitchOnHook.ccall<int>(switches);
            if (result)
                return result;

            if (activeInMenu() && isBackQuery(switches) && escapeCurrent && !escapePrevious)
                return 1;
            return 0;
        }

    public:
        std::string_view description() override
        {
            return "WheelMenuKeyboardEscapeBack";
        }

        bool validate() override
        {
            return Settings::WheelInputCompatibility;
        }

        void declare_settings() override
        {
            Settings::WheelMenuKeyboardEscapeBack.needs_restart();
        }

        bool apply() override
        {
            ReadIOHook = safetyhook::create_inline(Module::exe_ptr(0x53BB0), ReadIO_dest);
            SwitchNowHook = safetyhook::create_inline(Module::exe_ptr(0x536C0), SwitchNow_dest);
            SwitchOnHook = safetyhook::create_inline(Module::exe_ptr(0x536F0), SwitchOn_dest);

            const bool ok = !!ReadIOHook && !!SwitchNowHook && !!SwitchOnHook;
            if (ok)
                spdlog::info("WheelMenuKeyboardEscapeBack: enabled (Back/B/GearDown)");
            return ok;
        }

        static WheelMenuKeyboardBack instance;
    };

    WheelMenuKeyboardBack WheelMenuKeyboardBack::instance;
}
