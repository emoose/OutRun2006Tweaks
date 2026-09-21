#pragma once

#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

// MOZA R3 / ES-family menu navigation helper for the legacy DirectInput path.
//
// The original game can auto-bind an analogue wheel/pedal axis to menu
// Up/Down. On modern wheels that axis may not be centred the way OutRun 2006
// expects, so the menu scrolls continuously. Rather than trying to guess which
// legacy axis should be neutral, this helper replaces menu direction input with
// the R3 wheel's physical D-pad buttons while outside STATE_GAME.
//
// MOZA Pit House numbers buttons from 1, while DIJOYSTATE2::rgbButtons is
// zero-based. Public R3+ES profiles consistently map the D-pad as:
//   Up=5, Right=6, Down=7, Left=8  (Pit House numbering)
// The values remain configurable in case a firmware/wheel revision differs.
namespace Settings
{
    extern Setting<bool> WheelUniversalSetupEnable;

    Setting<bool> WheelMenuR3DirectDPad{
        "Controls", "WheelMenuR3DirectDPad", true,
        "In legacy wheel mode, ignores analogue menu-direction bindings and uses the MOZA R3/ES physical D-pad instead."
    };

    Setting<std::string> WheelMenuR3DeviceName{
        "Controls", "WheelMenuR3DeviceName", "MOZA",
        "Case-insensitive DirectInput device-name substring used for the menu D-pad reader."
    };

    Setting<int> WheelMenuR3UpButton{
        "Controls", "WheelMenuR3UpButton", 5,
        "Pit House 1-based button number for D-pad Up.", Range<int>{ 1, 128 }
    };

    Setting<int> WheelMenuR3RightButton{
        "Controls", "WheelMenuR3RightButton", 6,
        "Pit House 1-based button number for D-pad Right.", Range<int>{ 1, 128 }
    };

    Setting<int> WheelMenuR3DownButton{
        "Controls", "WheelMenuR3DownButton", 7,
        "Pit House 1-based button number for D-pad Down.", Range<int>{ 1, 128 }
    };

    Setting<int> WheelMenuR3LeftButton{
        "Controls", "WheelMenuR3LeftButton", 8,
        "Pit House 1-based button number for D-pad Left.", Range<int>{ 1, 128 }
    };
}

namespace
{
    class WheelR3MenuDPad : public Hook
    {
        struct Direction
        {
            uint32_t rawMask;
            uint32_t switchMask;
            int virtualKey;
            Settings::Setting<int>* buttonSetting;
            const char* name;
        };

        inline static const std::array<Direction, 4> Directions = {{
            { 0x00000040u, 1u << int(SwitchId::SelectionUp),    VK_UP,    &Settings::WheelMenuR3UpButton,    "up" },
            { 0x00000080u, 1u << int(SwitchId::SelectionRight), VK_RIGHT, &Settings::WheelMenuR3RightButton, "right" },
            { 0x00000020u, 1u << int(SwitchId::SelectionDown),  VK_DOWN,  &Settings::WheelMenuR3DownButton,  "down" },
            { 0x00000100u, 1u << int(SwitchId::SelectionLeft),  VK_LEFT,  &Settings::WheelMenuR3LeftButton,  "left" },
        }};

        inline static constexpr uint32_t DirectionRawMask = 0x000001E0u;
        inline static constexpr uint32_t DirectionSwitchMask =
            (1u << int(SwitchId::SelectionUp)) |
            (1u << int(SwitchId::SelectionDown)) |
            (1u << int(SwitchId::SelectionLeft)) |
            (1u << int(SwitchId::SelectionRight));

        inline static SafetyHookInline ReadIOHook = {};
        inline static SafetyHookInline SwitchNowHook = {};
        inline static SafetyHookInline SwitchOnHook = {};

        inline static IDirectInput8A* directInput = nullptr;
        inline static IDirectInputDevice8A* device = nullptr;
        inline static GUID selectedGuid{};
        inline static std::string selectedName;
        inline static DWORD retryAfter = 0;
        inline static DWORD lastReadErrorLog = 0;
        inline static bool loggedReady = false;

        inline static uint32_t currentDirectionsRaw = 0;
        inline static uint32_t previousDirectionsRaw = 0;
        inline static bool haveDirectState = false;

        struct EnumContext
        {
            GUID guid{};
            std::string name;
            bool found = false;
            bool ignoreName = false;
        };

        static std::string r3_lower_copy(const char* text)
        {
            std::string result = text ? text : "";
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        static bool activeInMenu()
        {
            return Settings::WheelInputCompatibility &&
                !Settings::UseNewInput &&
                !Settings::WheelUniversalSetupEnable &&
                Settings::WheelMenuR3DirectDPad &&
                Game::current_mode &&
                *Game::current_mode != STATE_GAME;
        }

        static BOOL CALLBACK enumDevicesCallback(
            LPCDIDEVICEINSTANCEA instance, LPVOID context)
        {
            auto* ctx = static_cast<EnumContext*>(context);
            const std::string wanted = r3_lower_copy(Settings::WheelMenuR3DeviceName.get().c_str());
            const std::string instanceName = r3_lower_copy(instance->tszInstanceName);
            const std::string productName = r3_lower_copy(instance->tszProductName);

            if (!ctx->ignoreName && !wanted.empty() &&
                instanceName.find(wanted) == std::string::npos &&
                productName.find(wanted) == std::string::npos)
            {
                return DIENUM_CONTINUE;
            }

            ctx->guid = instance->guidInstance;
            ctx->name = instance->tszProductName;
            ctx->found = true;
            return DIENUM_STOP;
        }

        static void releaseDevice()
        {
            if (device)
            {
                device->Unacquire();
                device->Release();
                device = nullptr;
            }
            if (directInput)
            {
                directInput->Release();
                directInput = nullptr;
            }
            selectedName.clear();
            haveDirectState = false;
        }

        static bool ensureDevice()
        {
            if (device)
                return true;

            const DWORD now = GetTickCount();
            if (retryAfter != 0 && static_cast<LONG>(now - retryAfter) < 0)
                return false;

            releaseDevice();

            HRESULT hr = DirectInput8Create(
                GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8A,
                reinterpret_cast<void**>(&directInput),
                nullptr);
            if (FAILED(hr) || !directInput)
            {
                retryAfter = now + 2000;
                return false;
            }

            EnumContext ctx{};
            hr = directInput->EnumDevices(
                DI8DEVCLASS_GAMECTRL,
                enumDevicesCallback,
                &ctx,
                DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
            if (SUCCEEDED(hr) && !ctx.found &&
                r3_lower_copy(Settings::WheelMenuR3DeviceName.get().c_str()) == "moza")
            {
                ctx.ignoreName = true;
                spdlog::warn(
                    "WheelMenuR3DirectDPad: no literal MOZA name; trying first attached FFB wheel");
                hr = directInput->EnumDevices(
                    DI8DEVCLASS_GAMECTRL,
                    enumDevicesCallback,
                    &ctx,
                    DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
            }
            if (FAILED(hr) || !ctx.found)
            {
                spdlog::warn(
                    "WheelMenuR3DirectDPad: no DirectInput FFB wheel matched '{}'",
                    Settings::WheelMenuR3DeviceName.get());
                retryAfter = now + 2000;
                releaseDevice();
                return false;
            }

            selectedGuid = ctx.guid;
            selectedName = ctx.name;

            hr = directInput->CreateDevice(selectedGuid, &device, nullptr);
            if (FAILED(hr) || !device)
            {
                retryAfter = now + 2000;
                releaseDevice();
                return false;
            }

            hr = device->SetDataFormat(&c_dfDIJoystick2);
            if (FAILED(hr))
            {
                retryAfter = now + 2000;
                releaseDevice();
                return false;
            }

            HWND hwnd = Game::GameHwnd();
            if (!hwnd)
            {
                retryAfter = now + 1000;
                releaseDevice();
                return false;
            }

            // Non-exclusive input reader: leave the original game DirectInput
            // device alone while navigating menus.
            hr = device->SetCooperativeLevel(
                hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
            if (FAILED(hr))
            {
                spdlog::warn(
                    "WheelMenuR3DirectDPad: SetCooperativeLevel failed (0x{:08X})",
                    static_cast<unsigned>(hr));
                retryAfter = now + 2000;
                releaseDevice();
                return false;
            }

            hr = device->Acquire();
            if (FAILED(hr) && hr != S_FALSE)
            {
                retryAfter = now + 2000;
                releaseDevice();
                return false;
            }

            DIDEVCAPS caps{};
            caps.dwSize = sizeof(caps);
            device->GetCapabilities(&caps);

            if (!loggedReady)
            {
                loggedReady = true;
                spdlog::info(
                    "WheelMenuR3DirectDPad: ready on '{}' (buttons={}, PitHouse Up/Right/Down/Left={}/{}/{}/{})",
                    selectedName,
                    caps.dwButtons,
                    int(Settings::WheelMenuR3UpButton),
                    int(Settings::WheelMenuR3RightButton),
                    int(Settings::WheelMenuR3DownButton),
                    int(Settings::WheelMenuR3LeftButton));
            }

            return true;
        }

        static bool buttonHeld(const DIJOYSTATE2& state, int oneBasedButton)
        {
            const int index = oneBasedButton - 1;
            if (index < 0 || index >= 128)
                return false;
            return (state.rgbButtons[index] & 0x80) != 0;
        }

        static uint32_t keyboardDirections()
        {
            uint32_t result = 0;
            for (const auto& direction : Directions)
            {
                if ((GetAsyncKeyState(direction.virtualKey) & 0x8000) != 0)
                    result |= direction.rawMask;
            }
            return result;
        }

        static bool pollDirectDirections(uint32_t& result)
        {
            result = 0;
            if (!ensureDevice())
                return false;

            HRESULT hr = device->Poll();
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
            {
                device->Acquire();
                hr = device->Poll();
            }

            DIJOYSTATE2 state{};
            hr = device->GetDeviceState(sizeof(state), &state);
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
            {
                device->Acquire();
                device->Poll();
                hr = device->GetDeviceState(sizeof(state), &state);
            }

            if (FAILED(hr))
            {
                const DWORD now = GetTickCount();
                if (now - lastReadErrorLog >= 5000)
                {
                    lastReadErrorLog = now;
                    spdlog::warn(
                        "WheelMenuR3DirectDPad: GetDeviceState failed (0x{:08X}); using legacy menu filter fallback",
                        static_cast<unsigned>(hr));
                }
                haveDirectState = false;
                releaseDevice();
                retryAfter = now + 500;
                return false;
            }

            for (const auto& direction : Directions)
            {
                if (buttonHeld(state, int(direction.buttonSetting->get())))
                    result |= direction.rawMask;
            }
            return true;
        }

        static uint32_t rawToSwitch(uint32_t raw)
        {
            uint32_t result = 0;
            for (const auto& direction : Directions)
                if (raw & direction.rawMask)
                    result |= direction.switchMask;
            return result;
        }

        static void replaceMenuDirections(SumoDInputState* state)
        {
            if (!state)
                return;

            if (!activeInMenu())
            {
                // The FFB engine intentionally acquires the wheel exclusively.
                // Release this non-exclusive menu reader before gameplay so it
                // can never block FFB acquisition on drivers that enforce it.
                if (device || directInput)
                    releaseDevice();
                previousDirectionsRaw = 0;
                currentDirectionsRaw = 0;
                return;
            }

            uint32_t wheelDirections = 0;
            if (!pollDirectDirections(wheelDirections))
            {
                // The V2 continuously-held-direction filter remains underneath
                // this hook as a fallback when a second non-exclusive read is
                // not possible on a specific driver.
                haveDirectState = false;
                return;
            }

            previousDirectionsRaw = currentDirectionsRaw;
            currentDirectionsRaw = wheelDirections | keyboardDirections();
            haveDirectState = true;

            // Replace, don't merge: this intentionally discards the original
            // game's auto-bound analogue menu axis, which is the source of the
            // permanent Up/Down input on the R3.
            state->buttons_4 =
                (state->buttons_4 & ~DirectionRawMask) | currentDirectionsRaw;
            state->pressed_8 =
                (state->pressed_8 & ~DirectionRawMask) |
                (currentDirectionsRaw & ~previousDirectionsRaw);
            state->released_C =
                (state->released_C & ~DirectionRawMask) |
                (previousDirectionsRaw & ~currentDirectionsRaw);
        }

        static int ReadIO_dest()
        {
            const int result = ReadIOHook.ccall<int>();
            replaceMenuDirections(Game::dinput_state);
            return result;
        }

        static bool pureDirectionQuery(uint32_t switches)
        {
            return (switches & DirectionSwitchMask) != 0 &&
                (switches & ~DirectionSwitchMask) == 0;
        }

        static int SwitchNow_dest(uint32_t switches)
        {
            if (activeInMenu() && haveDirectState && pureDirectionQuery(switches))
            {
                const uint32_t currentSwitches = rawToSwitch(currentDirectionsRaw);
                return (currentSwitches & switches) == switches;
            }
            return SwitchNowHook.ccall<int>(switches);
        }

        static int SwitchOn_dest(uint32_t switches)
        {
            if (activeInMenu() && haveDirectState && pureDirectionQuery(switches))
            {
                const uint32_t currentSwitches = rawToSwitch(currentDirectionsRaw);
                const uint32_t previousSwitches = rawToSwitch(previousDirectionsRaw);
                return (previousSwitches & switches) != switches &&
                    (currentSwitches & switches) == switches;
            }
            return SwitchOnHook.ccall<int>(switches);
        }

    public:
        std::string_view description() override
        {
            return "WheelR3MenuDPad";
        }

        bool validate() override
        {
            return Settings::WheelInputCompatibility &&
                !Settings::UseNewInput &&
                Settings::WheelMenuR3DirectDPad;
        }

        void declare_settings() override
        {
            Settings::WheelMenuR3DirectDPad.needs_restart();
            Settings::WheelMenuR3DeviceName.needs_restart();
            Settings::WheelMenuR3UpButton.needs_restart();
            Settings::WheelMenuR3RightButton.needs_restart();
            Settings::WheelMenuR3DownButton.needs_restart();
            Settings::WheelMenuR3LeftButton.needs_restart();

            Settings::WheelMenuR3DirectDPad.hidden(Settings::UseNewInput);
            Settings::WheelMenuR3DeviceName.hidden(Settings::UseNewInput);
            Settings::WheelMenuR3UpButton.hidden(Settings::UseNewInput);
            Settings::WheelMenuR3RightButton.hidden(Settings::UseNewInput);
            Settings::WheelMenuR3DownButton.hidden(Settings::UseNewInput);
            Settings::WheelMenuR3LeftButton.hidden(Settings::UseNewInput);
        }

        bool apply() override
        {
            ReadIOHook = safetyhook::create_inline(Module::exe_ptr(0x53BB0), ReadIO_dest);
            SwitchNowHook = safetyhook::create_inline(Module::exe_ptr(0x536C0), SwitchNow_dest);
            SwitchOnHook = safetyhook::create_inline(Module::exe_ptr(0x536F0), SwitchOn_dest);

            const bool ok = !!ReadIOHook && !!SwitchNowHook && !!SwitchOnHook;
            if (ok)
            {
                spdlog::info(
                    "WheelR3MenuDPad: enabled (PitHouse Up/Right/Down/Left={}/{}/{}/{})",
                    int(Settings::WheelMenuR3UpButton),
                    int(Settings::WheelMenuR3RightButton),
                    int(Settings::WheelMenuR3DownButton),
                    int(Settings::WheelMenuR3LeftButton));
            }
            return ok;
        }

        static WheelR3MenuDPad instance;
    };

    WheelR3MenuDPad WheelR3MenuDPad::instance;
}
