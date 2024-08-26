#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <hidsdi.h>

#include <queue>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

namespace Input
{
    static int HudToggleVKey = 0;

    void PadUpdate(int controllerIndex)
    {
        PadStatePrev = PadStateCur;
        PadDigitalPrev = PadDigitalCur;

        if (XInputGetState(controllerIndex, &PadStateCur) != ERROR_SUCCESS)
            PadStateCur = { 0 };

        PadDigitalCur = PadStateCur.Gamepad.wButtons;

        // Convert analog inputs to digital bitfield
        {
            if (PadStateCur.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
                PadDigitalCur |= XINPUT_DIGITAL_LEFT_TRIGGER;
            if (PadStateCur.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
                PadDigitalCur |= XINPUT_DIGITAL_RIGHT_TRIGGER;

            // Check left stick
            if (PadStateCur.Gamepad.sThumbLY > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
                PadDigitalCur |= XINPUT_DIGITAL_LS_UP;
            if (PadStateCur.Gamepad.sThumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
                PadDigitalCur |= XINPUT_DIGITAL_LS_DOWN;
            if (PadStateCur.Gamepad.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
                PadDigitalCur |= XINPUT_DIGITAL_LS_LEFT;
            if (PadStateCur.Gamepad.sThumbLX > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
                PadDigitalCur |= XINPUT_DIGITAL_LS_RIGHT;

            // Check right stick
            if (PadStateCur.Gamepad.sThumbRY > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)
                PadDigitalCur |= XINPUT_DIGITAL_RS_UP;
            if (PadStateCur.Gamepad.sThumbRY < -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)
                PadDigitalCur |= XINPUT_DIGITAL_RS_DOWN;
            if (PadStateCur.Gamepad.sThumbRX < -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)
                PadDigitalCur |= XINPUT_DIGITAL_RS_LEFT;
            if (PadStateCur.Gamepad.sThumbRX > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)
                PadDigitalCur |= XINPUT_DIGITAL_RS_RIGHT;
        }
    }

    void HudToggleUpdate()
    {
        static bool HudTogglePrevState = false;
        bool hudToggleKeyState = (GetAsyncKeyState(HudToggleVKey) & 0x8000);
        if (HudTogglePrevState != hudToggleKeyState)
        {
            HudTogglePrevState = hudToggleKeyState;
            if (!hudToggleKeyState) // if key is being released, toggle the hud flag
                *Game::navipub_disp_flg = (*Game::navipub_disp_flg == 0 ? 1 : 0);
        }
    }

    int StringToVK(std::string_view key)
    {
        // Convert key to uppercase
        std::string keyUpper(key);
        std::transform(keyUpper.begin(), keyUpper.end(), keyUpper.begin(),
            [](unsigned char c) { return std::toupper(c); });

        static const std::unordered_map<std::string_view, int> vkMap = {
            {"LBUTTON", VK_LBUTTON}, {"RBUTTON", VK_RBUTTON}, {"CANCEL", VK_CANCEL}, {"MBUTTON", VK_MBUTTON},
            {"XBUTTON1", VK_XBUTTON1}, {"XBUTTON2", VK_XBUTTON2}, {"BACK", VK_BACK}, {"TAB", VK_TAB},
            {"CLEAR", VK_CLEAR}, {"RETURN", VK_RETURN}, {"SHIFT", VK_SHIFT}, {"CONTROL", VK_CONTROL},
            {"MENU", VK_MENU}, {"PAUSE", VK_PAUSE}, {"CAPITAL", VK_CAPITAL}, {"ESCAPE", VK_ESCAPE},
            {"SPACE", VK_SPACE}, {"PRIOR", VK_PRIOR}, {"NEXT", VK_NEXT}, {"END", VK_END},
            {"HOME", VK_HOME}, {"LEFT", VK_LEFT}, {"UP", VK_UP}, {"RIGHT", VK_RIGHT},
            {"DOWN", VK_DOWN}, {"SELECT", VK_SELECT}, {"PRINT", VK_PRINT}, {"EXECUTE", VK_EXECUTE},
            {"SNAPSHOT", VK_SNAPSHOT}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HELP", VK_HELP},
            {"LWIN", VK_LWIN}, {"RWIN", VK_RWIN}, {"APPS", VK_APPS}, {"SLEEP", VK_SLEEP},
            {"NUMPAD0", VK_NUMPAD0}, {"NUMPAD1", VK_NUMPAD1}, {"NUMPAD2", VK_NUMPAD2}, {"NUMPAD3", VK_NUMPAD3},
            {"NUMPAD4", VK_NUMPAD4}, {"NUMPAD5", VK_NUMPAD5}, {"NUMPAD6", VK_NUMPAD6}, {"NUMPAD7", VK_NUMPAD7},
            {"NUMPAD8", VK_NUMPAD8}, {"NUMPAD9", VK_NUMPAD9}, {"MULTIPLY", VK_MULTIPLY}, {"ADD", VK_ADD},
            {"SEPARATOR", VK_SEPARATOR}, {"SUBTRACT", VK_SUBTRACT}, {"DECIMAL", VK_DECIMAL}, {"DIVIDE", VK_DIVIDE},
            {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5},
            {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10},
            {"F11", VK_F11}, {"F12", VK_F12}, {"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15},
            {"F16", VK_F16}, {"F17", VK_F17}, {"F18", VK_F18}, {"F19", VK_F19}, {"F20", VK_F20},
            {"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24}, {"NUMLOCK", VK_NUMLOCK},
            {"SCROLL", VK_SCROLL}
            // TODO: are there any others worth adding here?
        };

        // Check if key is a single character
        if (keyUpper.size() == 1)
        {
            char ch = keyUpper[0];
            if (std::isalnum(ch))
                return std::toupper(ch);
        }

        // Search in the vkMap
        auto it = vkMap.find(keyUpper);
        if (it != vkMap.end())
            return it->second;

        // If not found, return -1 or some invalid value
        return 0;
    }

    void Update()
    {
        static bool inited = false;
        if (!inited)
        {
            HudToggleVKey = StringToVK(Settings::HudToggleKey);
            inited = true;
        }

        // Update gamepad for main controller id
        PadUpdate(Settings::VibrationControllerId);

        if (HudToggleVKey)
            HudToggleUpdate();
    }
};

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

class ControllerHotPlug : public Hook
{
    const static int DInputInit_CallbackPtr_Addr = 0x3E10;

public:
    inline static std::mutex mtx;
    inline static std::unique_ptr<std::thread> DeviceEnumerationThreadHandle;
    inline static std::vector<GUID> KnownDevices;
    inline static std::queue<DIDEVICEINSTANCE> NewDevices;

    static BOOL __stdcall DInput_EnumJoysticksCallback(const DIDEVICEINSTANCE* pdidInstance, VOID* pContext)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (std::find(KnownDevices.begin(), KnownDevices.end(), pdidInstance->guidInstance) == KnownDevices.end())
        {
            // GUID not found, add to the vector
            KnownDevices.push_back(pdidInstance->guidInstance);

            // Add the new device instance to the queue
            NewDevices.push(*pdidInstance);
        }

        return DIENUM_CONTINUE;
    }

    static void DeviceEnumerationThread()
    {
#ifdef _DEBUG
        SetThreadDescription(GetCurrentThread(), L"DeviceEnumerationThread");
#endif

        while (true)
        {
            if (Game::DirectInput8())
                Game::DirectInput8()->EnumDevices(DI8DEVCLASS_GAMECTRL, DInput_EnumJoysticksCallback, nullptr, DIEDFL_ATTACHEDONLY);

            std::this_thread::sleep_for(std::chrono::seconds(2)); // Poll every 2 seconds
        }
    }

    std::string_view description() override
    {
        return "ControllerHotPlug";
    }

    bool validate() override
    {
        return Settings::ControllerHotPlug;
    }

    bool apply() override
    {
        // Patch games controller init code to go through our DInput_EnumJoysticksCallback func, so we can learn GUID of any already connected pads
        Memory::VP::Patch(Module::exe_ptr(DInputInit_CallbackPtr_Addr + 1), DInput_EnumJoysticksCallback);

        return true;
    }

    static ControllerHotPlug instance;
};
ControllerHotPlug ControllerHotPlug::instance;

void DInput_RegisterNewDevices()
{
    if (!ControllerHotPlug::DeviceEnumerationThreadHandle)
    {
        ControllerHotPlug::DeviceEnumerationThreadHandle = std::make_unique<std::thread>(ControllerHotPlug::DeviceEnumerationThread);
        ControllerHotPlug::DeviceEnumerationThreadHandle->detach();
    }

    std::lock_guard<std::mutex> lock(ControllerHotPlug::mtx);
    while (!ControllerHotPlug::NewDevices.empty())
    {
        DIDEVICEINSTANCE deviceInstance = ControllerHotPlug::NewDevices.front();
        ControllerHotPlug::NewDevices.pop();

        // Tell game about it
        Game::DInput_EnumJoysticksCallback(&deviceInstance, 0);

        // Sets some kind of active-device-number to let it work
        *Module::exe_ptr<int>(0x3398D4) = 0;
    }
}

class SteeringDeadZone : public Hook
{
public:
    std::string_view description() override
    {
        return "SteeringDeadZone";
    }

    bool validate() override
    {
        return Settings::SteeringDeadZone != 0.2f;
    }

    bool apply() override
    {
        constexpr int ReadVolume_UnkCheck_Addr = 0x538FC;
        constexpr int ReadVolume_LoadDeadZone_Addr = 0x538FE + 4;

        // Remove weird check which would override deadzone value to 0.0078125
        // (maybe that value is meant to be used for wheels, but game doesn't end up using it?)
        Memory::VP::Nop(Module::exe_ptr(ReadVolume_UnkCheck_Addr), 2);

        // Patch game code with Settings::SteeringDeadZone pointer
        Memory::VP::Patch(Module::exe_ptr(ReadVolume_LoadDeadZone_Addr), &Settings::SteeringDeadZone);

        return true;
    }

    static SteeringDeadZone instance;
};
SteeringDeadZone SteeringDeadZone::instance;
