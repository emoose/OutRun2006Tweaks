#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <hidsdi.h>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

// Hooks into XINPUT1_4's DeviceIoControl via undocumented DriverHook(0xBAAD0001) call
// Allows us to detect SET_GAMEPAD_STATE ioctl and send the GIP HID command for trigger impulses
// Hopefully will work for most series controller connection types...

// Defs from OpenXInput, GIP command code from X1nput
constexpr DWORD IOCTL_XINPUT_BASE = 0x8000;
static DWORD IOCTL_XINPUT_SET_GAMEPAD_STATE = CTL_CODE(IOCTL_XINPUT_BASE, 0x804, METHOD_BUFFERED, FILE_WRITE_ACCESS);                    // 0x8000A010
struct InSetState_t
{
    BYTE deviceIndex;
    BYTE ledState;
    BYTE leftMotorSpeed;
    BYTE rightMotorSpeed;
    BYTE flags;
};

#define XUSB_SET_STATE_FLAG_VIBRATION   ((BYTE)0x02)

class ImpulseVibration : public Hook
{
#define MAX_STR 255
    inline static wchar_t wstr[MAX_STR];

    static BOOL WINAPI DetourDeviceIoControl(
        HANDLE hDevice,
        DWORD dwIoControlCode,
        LPVOID lpInBuffer,
        DWORD nInBufferSize,
        LPVOID lpOutBuffer,
        DWORD nOutBufferSize,
        LPDWORD lpBytesReturned,
        LPOVERLAPPED lpOverlapped
    )
    {
        auto ret = DeviceIoControl(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, lpBytesReturned, lpOverlapped);
        if (dwIoControlCode != IOCTL_XINPUT_SET_GAMEPAD_STATE)
            return ret;

        // Don't send GIP command to x360, may cause issues with some bad third-party ones?
        if (HidD_GetProductString(hDevice, wstr, MAX_STR) && wcsstr(wstr, L"360"))
            return ret;

        if (!Settings::ImpulseVibrationMode)
            return ret; // how did we get here?

        InSetState_t* inData = (InSetState_t*)lpInBuffer;
        if ((inData->flags & XUSB_SET_STATE_FLAG_VIBRATION) != 0)
        {
            float leftTriggerInput = float(inData->leftMotorSpeed);
            float rightTriggerInput = float(inData->rightMotorSpeed);

            if (Settings::ImpulseVibrationMode == 2) // Swap L/R
            {
                leftTriggerInput = float(inData->rightMotorSpeed);
                rightTriggerInput = float(inData->leftMotorSpeed);
            }
            else if (Settings::ImpulseVibrationMode == 3) // Merge L/R by using whichever is highest
            {
                leftTriggerInput = rightTriggerInput = max(leftTriggerInput, rightTriggerInput);
            }

            uint8_t buf[9] = { 0 };
            buf[0] = 0x03; // HID report ID (3 for bluetooth, any for USB)
            buf[1] = 0x0F; // Motor flag mask(?)
            buf[2] = uint8_t(leftTriggerInput * Settings::ImpulseVibrationLeftMultiplier); // Left trigger impulse
            buf[3] = uint8_t(rightTriggerInput * Settings::ImpulseVibrationRightMultiplier); // Right trigger impulse
            buf[4] = inData->leftMotorSpeed; // Left rumble
            buf[5] = inData->rightMotorSpeed; // Right rumble
            // "Pulse"
            buf[6] = 0xFF; // On time
            buf[7] = 0x00; // Off time 
            buf[8] = 0xFF; // Number of repeats
            WriteFile(hDevice, buf, 9, lpBytesReturned, lpOverlapped);
        }

        return ret;
    }

public:
    std::string_view description() override
    {
        return "ImpulseVibration";
    }

    bool validate() override
    {
        return Settings::ImpulseVibrationMode != 0 && Settings::VibrationMode != 0;
    }

    bool apply() override
    {
        auto xinput = LoadLibraryA("xinput1_4.dll");
        if (!xinput)
            return false;

        typedef BOOL(__stdcall* DllMain_fn)(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved);
        auto dllmain = (DllMain_fn)GetProcAddress(xinput, "DllMain");
        if (!dllmain)
            return false;

        dllmain(nullptr, 0xBAAD0001, DetourDeviceIoControl);

        return true;
    }

    static ImpulseVibration instance;
};
ImpulseVibration ImpulseVibration::instance;
