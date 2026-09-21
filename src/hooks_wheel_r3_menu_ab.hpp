#pragma once

#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

// MOZA R3 / ES direct menu Confirm/Cancel helper for the legacy DirectInput path.
//
// OutRun's legacy menus reuse the configured Gear Up / Gear Down actions as
// Accept / Back. If those bindings are cleared or moved while configuring the
// wheel, menu navigation can become impossible. The R3 ES wheel exposes its
// physical A/B buttons as DirectInput Button 1 / Button 2 (Pit House 1-based
// numbering), so keep those two physical buttons available in menus regardless
// of the game's saved bindings:
//   A -> A + GearUp (Accept)
//   B -> B + Back + GearDown (Cancel/Back)
//
// This helper is menu-only and releases its DirectInput handle before gameplay
// so the force-feedback backend can take exclusive FFB ownership.
namespace Settings
{
    extern Setting<bool> WheelUniversalSetupEnable;

    Setting<bool> WheelMenuR3DirectAB{
        "Controls", "WheelMenuR3DirectAB", true,
        "In legacy wheel mode, keeps the MOZA R3/ES physical A/B buttons available as menu Accept/Back regardless of saved bindings."
    };

    Setting<int> WheelMenuR3AButton{
        "Controls", "WheelMenuR3AButton", 1,
        "Pit House 1-based button number for the R3/ES A button.", Range<int>{ 1, 128 }
    };

    Setting<int> WheelMenuR3BButton{
        "Controls", "WheelMenuR3BButton", 2,
        "Pit House 1-based button number for the R3/ES B button.", Range<int>{ 1, 128 }
    };

    // Declared by hooks_wheel_r3_menu_dpad.hpp, included before this file by the
    // wheel build shim.
    extern Setting<std::string> WheelMenuR3DeviceName;
}

namespace
{
    class WheelR3MenuAB : public Hook
    {
        inline static constexpr uint32_t RawAMask = 0x00000002u;
        inline static constexpr uint32_t RawBMask = 0x00000004u;
        inline static constexpr uint32_t RawBackMask = 0x00000200u;

        inline static constexpr uint32_t ASwitchMask = 1u << int(SwitchId::A);
        inline static constexpr uint32_t BSwitchMask = 1u << int(SwitchId::B);
        inline static constexpr uint32_t BackSwitchMask = 1u << int(SwitchId::Back);
        inline static constexpr uint32_t GearDownSwitchMask = 1u << int(SwitchId::GearDown);
        inline static constexpr uint32_t GearUpSwitchMask = 1u << int(SwitchId::GearUp);

        inline static SafetyHookInline ReadIOHook = {};
        inline static SafetyHookInline SwitchNowHook = {};
        inline static SafetyHookInline SwitchOnHook = {};

        inline static IDirectInput8A* directInput = nullptr;
        inline static IDirectInputDevice8A* device = nullptr;
        inline static DWORD retryAfter = 0;
        inline static DWORD lastReadErrorLog = 0;
        inline static bool loggedReady = false;
        inline static bool loggedA = false;
        inline static bool loggedB = false;

        inline static bool currentA = false;
        inline static bool previousA = false;
        inline static bool currentB = false;
        inline static bool previousB = false;
        inline static bool haveDirectState = false;

        struct EnumContext
        {
            GUID guid{};
            std::string name;
            bool found = false;
            bool ignoreName = false;
        };

        static std::string lowerCopy(const char* text)
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
                Settings::WheelMenuR3DirectAB &&
                Game::current_mode &&
                *Game::current_mode != STATE_GAME;
        }

        static BOOL CALLBACK enumDevicesCallback(
            LPCDIDEVICEINSTANCEA instance, LPVOID context)
        {
            auto* ctx = static_cast<EnumContext*>(context);
            const std::string wanted = lowerCopy(Settings::WheelMenuR3DeviceName.get().c_str());
            const std::string instanceName = lowerCopy(instance->tszInstanceName);
            const std::string productName = lowerCopy(instance->tszProductName);

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
                lowerCopy(Settings::WheelMenuR3DeviceName.get().c_str()) == "moza")
            {
                ctx.ignoreName = true;
                spdlog::warn(
                    "WheelMenuR3DirectAB: no literal MOZA name; trying first attached FFB wheel");
                hr = directInput->EnumDevices(
                    DI8DEVCLASS_GAMECTRL,
                    enumDevicesCallback,
                    &ctx,
                    DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
            }
            if (FAILED(hr) || !ctx.found)
            {
                retryAfter = now + 2000;
                releaseDevice();
                return false;
            }

            hr = directInput->CreateDevice(ctx.guid, &device, nullptr);
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

            hr = device->SetCooperativeLevel(
                hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
            if (FAILED(hr))
            {
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

            if (!loggedReady)
            {
                loggedReady = true;
                spdlog::info(
                    "WheelMenuR3DirectAB: ready on '{}' (PitHouse A/B={}/{})",
                    ctx.name,
                    int(Settings::WheelMenuR3AButton),
                    int(Settings::WheelMenuR3BButton));
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

        static bool pollButtons(bool& a, bool& b)
        {
            a = false;
            b = false;

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
                        "WheelMenuR3DirectAB: GetDeviceState failed (0x{:08X})",
                        static_cast<unsigned>(hr));
                }
                haveDirectState = false;
                releaseDevice();
                retryAfter = now + 500;
                return false;
            }

            a = buttonHeld(state, int(Settings::WheelMenuR3AButton));
            b = buttonHeld(state, int(Settings::WheelMenuR3BButton));
            return true;
        }

        static void updateRawState(SumoDInputState* state)
        {
            if (!state)
                return;

            if (!activeInMenu())
            {
                if (device || directInput)
                    releaseDevice();
                previousA = currentA = false;
                previousB = currentB = false;
                return;
            }

            bool a = false;
            bool b = false;
            if (!pollButtons(a, b))
                return;

            previousA = currentA;
            previousB = currentB;
            currentA = a;
            currentB = b;
            haveDirectState = true;

            if (currentA)
                state->buttons_4 |= RawAMask;
            if (currentB)
                state->buttons_4 |= RawBMask | RawBackMask;

            if (currentA && !previousA)
                state->pressed_8 |= RawAMask;
            else if (!currentA && previousA)
                state->released_C |= RawAMask;

            if (currentB && !previousB)
                state->pressed_8 |= RawBMask | RawBackMask;
            else if (!currentB && previousB)
                state->released_C |= RawBMask | RawBackMask;
        }

        static int ReadIO_dest()
        {
            const int result = ReadIOHook.ccall<int>();
            updateRawState(Game::dinput_state);
            return result;
        }

        static bool isAQuery(uint32_t switches)
        {
            return switches == ASwitchMask || switches == GearUpSwitchMask;
        }

        static bool isBQuery(uint32_t switches)
        {
            return switches == BSwitchMask ||
                switches == BackSwitchMask ||
                switches == GearDownSwitchMask;
        }

        static int SwitchNow_dest(uint32_t switches)
        {
            const int result = SwitchNowHook.ccall<int>(switches);
            if (result)
                return result;

            if (!activeInMenu() || !haveDirectState)
                return 0;

            if (currentA && isAQuery(switches))
            {
                if (!loggedA)
                {
                    loggedA = true;
                    spdlog::info("WheelMenuR3DirectAB: physical A -> A/GearUp menu Accept");
                }
                return 1;
            }

            if (currentB && isBQuery(switches))
            {
                if (!loggedB)
                {
                    loggedB = true;
                    spdlog::info("WheelMenuR3DirectAB: physical B -> B/Back/GearDown menu Cancel");
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

            if (!activeInMenu() || !haveDirectState)
                return 0;

            if (currentA && !previousA && isAQuery(switches))
                return 1;
            if (currentB && !previousB && isBQuery(switches))
                return 1;
            return 0;
        }

    public:
        std::string_view description() override
        {
            return "WheelR3MenuAB";
        }

        bool validate() override
        {
            return Settings::WheelInputCompatibility &&
                !Settings::UseNewInput &&
                Settings::WheelMenuR3DirectAB;
        }

        void declare_settings() override
        {
            Settings::WheelMenuR3DirectAB.needs_restart();
            Settings::WheelMenuR3AButton.needs_restart();
            Settings::WheelMenuR3BButton.needs_restart();
            Settings::WheelMenuR3DirectAB.hidden(Settings::UseNewInput);
            Settings::WheelMenuR3AButton.hidden(Settings::UseNewInput);
            Settings::WheelMenuR3BButton.hidden(Settings::UseNewInput);
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
                    "WheelR3MenuAB: enabled (PitHouse A/B={}/{})",
                    int(Settings::WheelMenuR3AButton),
                    int(Settings::WheelMenuR3BButton));
            }
            return ok;
        }

        static WheelR3MenuAB instance;
    };

    WheelR3MenuAB WheelR3MenuAB::instance;
}
