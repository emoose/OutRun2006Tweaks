#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

// Legacy DirectInput controller configuration helper.
//
// OutRun 2006 ships generic joystick defaults that pre-assign several analogue
// channels.  Modern wheels can therefore start with a pedal on the combined
// Accel/Brake channel and another one-sided axis on Menu Up/Down.  The original
// controller screen already supports a true Unassigned state, so keep its own
// calibration UI and only change the defaults it starts from.
//
// Important: never clear a user's saved custom profile on every launch.  When
// the controller screen opens we only clear a device if all of its mappings
// still match one of the game's untouched factory/default layouts.  Pressing
// the screen's DEFAULT row intentionally clears the selected legacy joystick
// again, which gives the user a convenient manual-reset path.
namespace Settings
{
    Setting<bool> WheelLegacyBlankDefaults{
        "Controls", "WheelLegacyBlankDefaults", true,
        "Starts untouched legacy wheel/controller profiles with all axes/buttons Unassigned; the DEFAULT row also clears all bindings for manual setup."
    };
}

namespace
{
    class WheelLegacyBlankDefaults : public Hook
    {
        // Absolute game globals 0x8606D4 / 0x95AEC4 converted to module offsets.
        inline static constexpr uintptr_t DeviceSlotsOffset = 0x4606D4;
        inline static constexpr uintptr_t DeviceCountOffset = 0x55AEC4;

        // Original controller DEFAULT helper: OR2006C2C.exe + 0x4050.
        inline static constexpr uintptr_t DefaultResetOffset = 0x4050;

        // Point in the legacy controller-screen setup after the selected-device
        // list has been built, immediately before the rows are refreshed.
        inline static constexpr uintptr_t ControllerScreenReadyOffset = 0xD7F93;

        inline static constexpr size_t AxisMapOffset = 0x54;
        inline static constexpr size_t ButtonMapOffset = 0x70;

        // Generic DirectInput defaults created by the original wheel object.
        inline static constexpr std::array<int32_t, 7> FactoryAxes = {
            8, 8, 2, 0, 4, 1, 3
        };
        inline static constexpr std::array<int32_t, 16> FactoryButtons = {
            0, 1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15, 7, 6, 8, 9
        };

        // Values used internally by the original game for a truly unassigned
        // legacy mapping.  Axis setter 0x402610 accepts 8 as Unassigned, while
        // button setter 0x402550 receives -1 when a row is cleared.
        inline static constexpr int32_t AxisUnassigned = 8;
        inline static constexpr int32_t ButtonUnassigned = -1;

        inline static SafetyHookInline DefaultResetHook = {};
        inline static SafetyHookMid ControllerScreenReadyHook = {};
        inline static bool loggedAutoBlank = false;
        inline static bool loggedDefaultBlank = false;

        static bool active()
        {
            return Settings::WheelInputCompatibility &&
                !Settings::UseNewInput &&
                Settings::WheelLegacyBlankDefaults;
        }

        static int deviceCount()
        {
            auto* count = Module::exe_ptr<int>(DeviceCountOffset);
            if (!count)
                return 0;
            return std::clamp(*count, 0, 4);
        }

        static uint8_t* deviceAt(int index)
        {
            if (index < 0 || index >= deviceCount())
                return nullptr;

            auto* slots = Module::exe_ptr<uintptr_t>(DeviceSlotsOffset);
            if (!slots)
                return nullptr;
            return reinterpret_cast<uint8_t*>(slots[index]);
        }

        static bool allButtonsZero(const int32_t* buttons)
        {
            for (size_t i = 0; i < FactoryButtons.size(); ++i)
                if (buttons[i] != 0)
                    return false;
            return true;
        }

        static bool looksLikeUntouchedDefault(uint8_t* device)
        {
            if (!device)
                return false;

            const auto* axes = reinterpret_cast<const int32_t*>(device + AxisMapOffset);
            const auto* buttons = reinterpret_cast<const int32_t*>(device + ButtonMapOffset);

            const bool factoryAxes = std::equal(FactoryAxes.begin(), FactoryAxes.end(), axes);
            if (!factoryAxes)
                return false;

            // The constructor uses FactoryButtons.  The game's DEFAULT helper
            // uses an all-zero 16-entry button table.  Treat either exact state
            // as untouched/default, but preserve anything else as user data.
            const bool constructorButtons =
                std::equal(FactoryButtons.begin(), FactoryButtons.end(), buttons);
            return constructorButtons || allButtonsZero(buttons);
        }

        static void blankDevice(uint8_t* device)
        {
            if (!device)
                return;

            auto* axes = reinterpret_cast<int32_t*>(device + AxisMapOffset);
            auto* buttons = reinterpret_cast<int32_t*>(device + ButtonMapOffset);

            std::fill_n(axes, 7, AxisUnassigned);
            std::fill_n(buttons, 16, ButtonUnassigned);
        }

        static void blankUntouchedDevices()
        {
            if (!active())
                return;

            const int count = deviceCount();
            // The final entry is treated specially by the original game (its
            // keyboard/special controller path), so only touch regular legacy
            // DirectInput joystick entries here.
            for (int i = 0; i < count - 1; ++i)
            {
                auto* device = deviceAt(i);
                if (!looksLikeUntouchedDefault(device))
                    continue;

                blankDevice(device);
                if (!loggedAutoBlank)
                {
                    loggedAutoBlank = true;
                    spdlog::info(
                        "WheelLegacyBlankDefaults: untouched legacy controller mappings changed to Unassigned for manual setup");
                }
            }
        }

        static int DefaultReset_dest(int deviceIndex)
        {
            if (!active())
                return DefaultResetHook.ccall<int>(deviceIndex);

            const int count = deviceCount();
            if (deviceIndex < 0 || deviceIndex >= count - 1)
                return DefaultResetHook.ccall<int>(deviceIndex);

            auto* device = deviceAt(deviceIndex);
            if (!device)
                return DefaultResetHook.ccall<int>(deviceIndex);

            blankDevice(device);
            if (!loggedDefaultBlank)
            {
                loggedDefaultBlank = true;
                spdlog::info(
                    "WheelLegacyBlankDefaults: DEFAULT now clears legacy wheel axes/buttons to Unassigned");
            }
            return 1;
        }

        static void ControllerScreenReady_dest(SafetyHookContext&)
        {
            blankUntouchedDevices();
        }

    public:
        std::string_view description() override
        {
            return "WheelLegacyBlankDefaults";
        }

        bool validate() override
        {
            return Settings::WheelInputCompatibility;
        }

        void declare_settings() override
        {
            Settings::WheelLegacyBlankDefaults.needs_restart();
        }

        bool apply() override
        {
            DefaultResetHook = safetyhook::create_inline(
                Module::exe_ptr(DefaultResetOffset), DefaultReset_dest);
            ControllerScreenReadyHook = safetyhook::create_mid(
                Module::exe_ptr(ControllerScreenReadyOffset), ControllerScreenReady_dest);

            const bool ok = !!DefaultResetHook && !!ControllerScreenReadyHook;
            if (ok)
                spdlog::info("WheelLegacyBlankDefaults: enabled");
            return ok;
        }

        static WheelLegacyBlankDefaults instance;
    };

    WheelLegacyBlankDefaults WheelLegacyBlankDefaults::instance;
}
