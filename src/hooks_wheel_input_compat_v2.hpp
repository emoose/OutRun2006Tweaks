#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

// Second-stage compatibility fixes for legacy DirectInput steering wheels.
//
// The original game's legacy input path can expose a one-sided pedal axis as a
// centred/shared accelerator+brake axis. In that case the first half of a MOZA
// throttle is seen as acceleration, while the second half starts producing a
// brake channel too. f365 corrected the accelerator direction, but did so after
// the game had already split that raw axis. Keep that direction correction and
// suppress only the synthetic brake half while acceleration is active.
//
// Menu filtering is deliberately done after ReadIO as well as at SwitchNow /
// SwitchOn. DInputUpdate is too early: ReadIO can rebuild the raw state after a
// filter placed there. The logical switch filter also covers menus that do not
// consult SumoDInputState directly.
namespace Settings
{
    extern Setting<bool> WheelMenuDirectionFilter; // hooks_input.cpp
    extern Setting<bool> UseNewInput;
    extern Setting<bool> WheelInputCompatibility;
    extern Setting<bool> WheelUniversalSetupEnable;

    Setting<bool> WheelPedalSplitFix{
        "Controls", "WheelPedalSplitFix", false,
        "Suppresses the synthetic brake half produced when a legacy DirectInput wheel exposes one pedal as a centred/shared axis."
    };

    Setting<int> WheelPedalSplitThreshold{
        "Controls", "WheelPedalSplitThreshold", 32,
        "Acceleration amount (0-255) required before WheelPedalSplitFix suppresses simultaneous brake cross-talk.",
        Range<int>{ 1, 254 }
    };

    Setting<bool> WheelMenuBackAlias{
        "Controls", "WheelMenuBackAlias", true,
        "In menus, treats the legacy Back action and B/Return action as aliases so either configured wheel button can leave a screen."
    };
}

namespace
{
    class WheelInputCompatibilityV2 : public Hook
    {
        struct Direction
        {
            uint32_t rawMask;
            uint32_t switchMask;
            int virtualKey;
            const char* name;
        };

        inline static constexpr std::array<Direction, 4> Directions = {{
            { 0x00000040u, 1u << int(SwitchId::SelectionUp),    VK_UP,    "up" },
            { 0x00000020u, 1u << int(SwitchId::SelectionDown),  VK_DOWN,  "down" },
            { 0x00000100u, 1u << int(SwitchId::SelectionLeft),  VK_LEFT,  "left" },
            { 0x00000080u, 1u << int(SwitchId::SelectionRight), VK_RIGHT, "right" },
        }};

        inline static constexpr uint32_t DirectionRawMask = 0x000001E0u;
        inline static constexpr uint32_t BackSwitchMask = 1u << int(SwitchId::Back);
        inline static constexpr uint32_t BSwitchMask = 1u << int(SwitchId::B);
        inline static constexpr auto LearnDelay = std::chrono::milliseconds(250);

        inline static SafetyHookInline GetVolumeHook = {};
        inline static SafetyHookInline GetVolumeOldHook = {};
        inline static SafetyHookInline ReadIOHook = {};
        inline static SafetyHookInline SwitchNowHook = {};
        inline static SafetyHookInline SwitchOnHook = {};

        inline static std::array<std::chrono::steady_clock::time_point, Directions.size()> rawHeldSince = {};
        inline static std::array<std::chrono::steady_clock::time_point, Directions.size()> logicalHeldSince = {};
        inline static uint32_t learnedDirectionRawMask = 0;
        inline static uint32_t previousFilteredDirections = 0;
        inline static DWORD lastPedalSplitLogTick = 0;
        inline static bool backAliasLogged = false;

        static bool active()
        {
            return Settings::WheelInputCompatibility &&
                !Settings::UseNewInput &&
                !Settings::WheelUniversalSetupEnable;
        }

        static bool mayLearnMenuDirection()
        {
            return active() && Game::current_mode && *Game::current_mode != STATE_GAME;
        }

        static bool keyboardHeld(const Direction& direction)
        {
            return (GetAsyncKeyState(direction.virtualKey) & 0x8000) != 0;
        }

        static void learnRawDirections(SumoDInputState* state)
        {
            if (!state || !mayLearnMenuDirection())
                return;

            const auto now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < Directions.size(); ++i)
            {
                const auto& direction = Directions[i];
                if (learnedDirectionRawMask & direction.rawMask)
                    continue;

                const bool held = (state->buttons_4 & direction.rawMask) != 0;
                if (held && !keyboardHeld(direction))
                {
                    if (rawHeldSince[i] == std::chrono::steady_clock::time_point{})
                        rawHeldSince[i] = now;
                    else if (now - rawHeldSince[i] >= LearnDelay)
                    {
                        learnedDirectionRawMask |= direction.rawMask;
                        spdlog::info(
                            "WheelMenuDirectionFilterV2: suppressing continuously-held '{}' raw input (mask 0x{:X}) after ReadIO",
                            direction.name, direction.rawMask);
                    }
                }
                else
                {
                    rawHeldSince[i] = {};
                }
            }
        }

        static void filterRawDirections(SumoDInputState* state)
        {
            if (!state || !active() || !Settings::WheelMenuDirectionFilter)
                return;

            learnRawDirections(state);

            uint32_t keyboardDirections = 0;
            for (const auto& direction : Directions)
                if (keyboardHeld(direction))
                    keyboardDirections |= direction.rawMask;

            const uint32_t rawDirections = state->buttons_4 & DirectionRawMask;
            const uint32_t outputDirections =
                (rawDirections & ~learnedDirectionRawMask) | keyboardDirections;

            state->buttons_4 = (state->buttons_4 & ~DirectionRawMask) | outputDirections;
            state->pressed_8 = (state->pressed_8 & ~DirectionRawMask) |
                (outputDirections & ~previousFilteredDirections);
            state->released_C = (state->released_C & ~DirectionRawMask) |
                (previousFilteredDirections & ~outputDirections);
            previousFilteredDirections = outputDirections;
        }

        static void learnLogicalDirection(uint32_t switches, bool held)
        {
            if (!mayLearnMenuDirection() || !Settings::WheelMenuDirectionFilter)
                return;

            const auto now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < Directions.size(); ++i)
            {
                const auto& direction = Directions[i];
                if ((switches & direction.switchMask) == 0 ||
                    (learnedDirectionRawMask & direction.rawMask))
                    continue;

                if (held && !keyboardHeld(direction))
                {
                    if (logicalHeldSince[i] == std::chrono::steady_clock::time_point{})
                        logicalHeldSince[i] = now;
                    else if (now - logicalHeldSince[i] >= LearnDelay)
                    {
                        learnedDirectionRawMask |= direction.rawMask;
                        spdlog::info(
                            "WheelMenuDirectionFilterV2: suppressing continuously-held '{}' logical switch (mask 0x{:X})",
                            direction.name, direction.switchMask);
                    }
                }
                else
                {
                    logicalHeldSince[i] = {};
                }
            }
        }

        static bool shouldSuppressLogicalDirection(uint32_t switches)
        {
            if (!active() || !Settings::WheelMenuDirectionFilter)
                return false;

            for (const auto& direction : Directions)
            {
                if ((learnedDirectionRawMask & direction.rawMask) &&
                    (switches & direction.switchMask) &&
                    !keyboardHeld(direction))
                {
                    return true;
                }
            }
            return false;
        }

        static int filterPedal(int channel, int result, bool oldValue)
        {
            if (!active() || !Settings::WheelPedalSplitFix || channel != int(ADChannel::Brake) || result <= 0)
                return result;

            // Call our trampoline for the acceleration channel. The trampoline
            // still passes through the earlier f365 direction correction, so
            // this value is already 0 at rest and rises with pedal travel.
            const int acceleration = oldValue
                ? GetVolumeOldHook.call<int>(int(ADChannel::Acceleration))
                : GetVolumeHook.call<int>(int(ADChannel::Acceleration));

            const int threshold = std::clamp(int(Settings::WheelPedalSplitThreshold), 1, 254);
            if (acceleration < threshold)
                return result;

            const DWORD now = GetTickCount();
            if (now - lastPedalSplitLogTick >= 2000)
            {
                lastPedalSplitLogTick = now;
                spdlog::info(
                    "WheelPedalSplitFix: suppressing synthetic brake while accelerator is active (accel={}, brake={})",
                    acceleration, result);
            }
            return 0;
        }

        static int GetVolume_dest(int channel)
        {
            const int result = GetVolumeHook.call<int>(channel);
            return filterPedal(channel, result, false);
        }

        static int GetVolumeOld_dest(int channel)
        {
            const int result = GetVolumeOldHook.call<int>(channel);
            return filterPedal(channel, result, true);
        }

        static int ReadIO_dest()
        {
            const int result = ReadIOHook.ccall<int>();
            if (active() && Settings::WheelMenuDirectionFilter)
                filterRawDirections(Game::dinput_state);
            return result;
        }

        static int SwitchNow_dest(uint32_t switches)
        {
            int result = SwitchNowHook.ccall<int>(switches);

            if (!active())
                return result;

            if (Settings::WheelMenuDirectionFilter)
            {
                learnLogicalDirection(switches, result != 0);
                if (result && shouldSuppressLogicalDirection(switches))
                    result = 0;
            }

            if (!result && Settings::WheelMenuBackAlias &&
                Game::current_mode && *Game::current_mode != STATE_GAME)
            {
                if (switches == BSwitchMask)
                    result = SwitchNowHook.ccall<int>(BackSwitchMask);
                else if (switches == BackSwitchMask)
                    result = SwitchNowHook.ccall<int>(BSwitchMask);

                if (result && !backAliasLogged)
                {
                    backAliasLogged = true;
                    spdlog::info("WheelMenuBackAlias: treating legacy Back and B/Return as equivalent in menus");
                }
            }

            return result;
        }

        static int SwitchOn_dest(uint32_t switches)
        {
            int result = SwitchOnHook.ccall<int>(switches);

            if (!active())
                return result;

            if (result && shouldSuppressLogicalDirection(switches))
                result = 0;

            if (!result && Settings::WheelMenuBackAlias &&
                Game::current_mode && *Game::current_mode != STATE_GAME)
            {
                if (switches == BSwitchMask)
                    result = SwitchOnHook.ccall<int>(BackSwitchMask);
                else if (switches == BackSwitchMask)
                    result = SwitchOnHook.ccall<int>(BSwitchMask);

                if (result && !backAliasLogged)
                {
                    backAliasLogged = true;
                    spdlog::info("WheelMenuBackAlias: treating legacy Back and B/Return as equivalent in menus");
                }
            }

            return result;
        }

    public:
        std::string_view description() override
        {
            return "WheelInputCompatibilityV2";
        }

        bool validate() override
        {
            // Install with the legacy stack so F11 can switch the universal
            // profile live. active() remains the runtime ownership gate.
            return Settings::WheelInputCompatibility && !Settings::UseNewInput;
        }

        void declare_settings() override
        {
            Settings::WheelPedalSplitFix.needs_restart();
            Settings::WheelMenuBackAlias.needs_restart();
            Settings::WheelPedalSplitFix.hidden(Settings::UseNewInput);
            Settings::WheelPedalSplitThreshold.hidden(Settings::UseNewInput);
            Settings::WheelMenuBackAlias.hidden(Settings::UseNewInput);
        }

        bool apply() override
        {
            // These addresses are the same legacy entry points already used by
            // FixFullPedalChecks/NewInputHook. SafetyHook chains inline hooks;
            // in compatibility mode NewInputHook is disabled, while the pedal
            // fix intentionally sits after FixFullPedalChecks.
            GetVolumeHook = safetyhook::create_inline(Module::exe_ptr(0x53720), GetVolume_dest);
            GetVolumeOldHook = safetyhook::create_inline(Module::exe_ptr(0x53750), GetVolumeOld_dest);
            ReadIOHook = safetyhook::create_inline(Module::exe_ptr(0x53BB0), ReadIO_dest);
            SwitchNowHook = safetyhook::create_inline(Module::exe_ptr(0x536C0), SwitchNow_dest);
            SwitchOnHook = safetyhook::create_inline(Module::exe_ptr(0x536F0), SwitchOn_dest);

            const bool ok = !!GetVolumeHook && !!GetVolumeOldHook && !!ReadIOHook && !!SwitchNowHook && !!SwitchOnHook;
            if (ok)
            {
                spdlog::info(
                    "WheelInputCompatibilityV2: pedalSplitFix={}, threshold={}, postReadIO/menuSwitchFilter={}, backAlias={}",
                    bool(Settings::WheelPedalSplitFix),
                    int(Settings::WheelPedalSplitThreshold),
                    bool(Settings::WheelMenuDirectionFilter),
                    bool(Settings::WheelMenuBackAlias));
            }
            return ok;
        }

        static WheelInputCompatibilityV2 instance;
    };

    WheelInputCompatibilityV2 WheelInputCompatibilityV2::instance;
}
