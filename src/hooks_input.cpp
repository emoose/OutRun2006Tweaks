#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <hidsdi.h>

#include <queue>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "input_manager.hpp"

namespace Settings
{
	Setting<float> SteeringDeadZone{ "Controls", "SteeringDeadZone", 0.2f,
		"Allows overriding the steering deadzone. Game default is 0.2 / 20%.", Range<float>{ 0.f, 1.f } };
	Setting<bool> ControllerHotPlug{ "Controls", "ControllerHotPlug", false,
		"Allows game to detect newly plugged in devices, rather than needing a restart. May have issues with some "
		"controllers/wheels, and is ignored when using UseNewInput as hot-plug is supported by it by default." };
	Setting<int> ImpulseVibrationMode{ "Controls", "ImpulseVibrationMode", 2,
		"Enable/disable Xbox Series impulse trigger vibration, or customise it. VibrationMode must be enabled for this to work.",
		{ "Disable", "Enable impulse triggers", "L/R motors swapped (recommended for impulse triggers)",
		  "L/R motors merged together" } };
	Setting<float> ImpulseVibrationLeftMultiplier{ "Controls", "ImpulseVibrationLeftMultiplier", 0.20f,
		"Multiplier of the trigger vibration, the normal controller motor vibration is multiplied by this to set trigger value.",
		Range<float>{ 0.f, 1.f } };
	Setting<float> ImpulseVibrationRightMultiplier{ "Controls", "ImpulseVibrationRightMultiplier", 0.20f,
		"Multiplier of the trigger vibration, the normal controller motor vibration is multiplied by this to set trigger value.",
		Range<float>{ 0.f, 1.f } };
}

namespace Input
{
    // Legacy input only - the new system binds ModAction::HudToggle instead.
    static constexpr int HudToggleVKey = VK_F10;

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

        // Legacy input has no binding UI, so it uses a fixed key.
        bool hudToggleKeyState = Settings::UseNewInput
            ? InputManager_ModActionHeld(ModAction::HudToggle)
            : (GetAsyncKeyState(HudToggleVKey) & 0x8000) != 0;

        if (HudTogglePrevState != hudToggleKeyState)
        {
            HudTogglePrevState = hudToggleKeyState;
            if (!hudToggleKeyState) // if key is being released, toggle the hud flag
                *Game::navipub_disp_flg = (*Game::navipub_disp_flg == 0 ? 1 : 0);
        }
    }

    void Update()
    {
        // Update gamepad for main controller id
        PadUpdate(Settings::VibrationControllerId);

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

        // TODO: This should disable when UseNewInput is on, since SDL3 is meant to support trigger rumble
        // But some reason I couldn't get it to work through SDL3 with any backend? (GameInput/RawInput/WGI/DirectInput)
        // Fortunately keeping this hook active seems to work with it at least.
        if (!Settings::ImpulseVibrationMode || !Settings::VibrationMode)
            return ret;

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

    void declare_settings() override
    {
        Settings::ControllerHotPlug.needs_restart();
        Settings::ControllerHotPlug.hidden(true);
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

    bool apply() override
    {
        constexpr int ReadVolume_UnkCheck_Addr = 0x538FC;
        constexpr int ReadVolume_LoadDeadZone_Addr = 0x538FE + 4;

        // Remove weird check which would override deadzone value to 0.0078125
        // (maybe that value is meant to be used for wheels, but game doesn't end up using it?)
        Memory::VP::Nop(Module::exe_ptr(ReadVolume_UnkCheck_Addr), 2);

        // Patch game code with Settings::SteeringDeadZone pointer
        Memory::VP::Patch(Module::exe_ptr(ReadVolume_LoadDeadZone_Addr), Settings::SteeringDeadZone.ptr());

        return true;
    }

    static SteeringDeadZone instance;
};
SteeringDeadZone SteeringDeadZone::instance;
