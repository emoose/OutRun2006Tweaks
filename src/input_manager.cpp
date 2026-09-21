#include "input_manager.hpp"
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <Windows.h>
#include <dinput.h>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace Settings
{
	Setting<int> InputBackend{ "Controls", "InputBackend", 0,
		"Backend to use for the SDL3 input system. Automatic uses DirectInput when a force-feedback wheel is attached "
		"and Windows.Gaming.Input otherwise. If a controller fails to respond, choose another backend and relaunch.",
		{ "Automatic", "RawInput", "DirectInput", "XInput" } };

	Setting<bool> WheelInputCompatibility{ "Controls", "WheelInputCompatibility", false,
		"Advanced fallback for the original-game DirectInput wheel path. It only activates when UseNewInput is false at launch; both mode changes require a restart." };

	Setting<bool> UseNewInput{ "Controls", "UseNewInput", true,
		"Enables new SDL-based input system, allowing game to see full trigger range without any shared trigger axes issues "
		"(experimental, not every menu/gamemode has been tested with it yet)." };
	Setting<bool> BypassGameSensitivity{ "Controls", "BypassGameSensitivity", false,
		"Passes steering input to the game directly instead of through its own sensitivity curve, allowing for more "
		"sensitive controls. Only used when UseNewInput is enabled." };
}

namespace
{
    BOOL CALLBACK detect_ffb_device(LPCDIDEVICEINSTANCEA, LPVOID context)
    {
        *static_cast<bool*>(context) = true;
        return DIENUM_STOP;
    }

    bool has_attached_ffb_wheel()
    {
        IDirectInput8A* di = nullptr;
        const HRESULT createHr = DirectInput8Create(
            GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
            reinterpret_cast<void**>(&di), nullptr);
        if (FAILED(createHr) || !di)
            return false;

        bool found = false;
        const HRESULT enumHr = di->EnumDevices(
            DI8DEVCLASS_GAMECTRL, detect_ffb_device, &found,
            DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
        di->Release();
        return SUCCEEDED(enumHr) && found;
    }
}

InputManager& InputManager::instance = *new InputManager;

// TODO: Move most of input_manager.hpp to this .cpp, not sure why so much was left in there..
void InputManager::init(HWND hwnd)
{
	int activeBackend = Settings::InputBackend;
	if (activeBackend == 0 && has_attached_ffb_wheel())
	{
		activeBackend = 2;
		spdlog::info(__FUNCTION__ ": Automatic backend selected DirectInput for an attached force-feedback wheel");
	}
	else if (activeBackend == 0)
		spdlog::info(__FUNCTION__ ": Automatic backend selected Windows.Gaming.Input");

	SDL_SetHint(SDL_HINT_JOYSTICK_WGI, activeBackend == 0 ? "1" : "0");
	SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, activeBackend == 1 ? "1" : "0");
	SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, activeBackend == 2 ? "1" : "0");
	SDL_SetHint(SDL_HINT_XINPUT_ENABLED, activeBackend == 3 ? "1" : "0");

	if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_VIDEO))
	{
		spdlog::error(__FUNCTION__ ": SDL input initialization failed: {}", SDL_GetError());
		return;
	}

	// Discover hardware that was connected before the game launched. This also
	// includes wheels, pedals and shifters which are not in SDL's gamepad mapping
	// database and were therefore invisible to the previous implementation.
	int joystickCount = 0;
	SDL_JoystickID* joystickIds = SDL_GetJoysticks(&joystickCount);
	if (!joystickIds)
		spdlog::error(__FUNCTION__ ": SDL device enumeration failed: {}", SDL_GetError());
	else
	{
		for (int i = 0; i < joystickCount; ++i)
		{
			onJoystickAdded(joystickIds[i]);
			if (SDL_IsGamepad(joystickIds[i]))
				onControllerAdded(joystickIds[i]);
		}
		SDL_free(joystickIds);
	}
	spdlog::info(__FUNCTION__ ": detected {} input devices ({} gamepads)", devices.size(), controllers.size());

	// Need to setup SDL_Window for SDL to see keyboard events
	SDL_PropertiesID props = SDL_CreateProperties();
	if (props)
	{
		SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "OutRun2006Tweaks");
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 1280);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 720);
		SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, hwnd);

		window = SDL_CreateWindowWithProperties(props);
	}
	else
		spdlog::error(__FUNCTION__ ": failed to create properties ({}), keyboard might not work with UseNewInput properly!", SDL_GetError());

	if (!readBindingIni(Module::BindingsIniPath))
		setupDefaultBindings();

	ensureOverlayBindable();
}

void InputManager_Update()
{
	if (Settings::UseNewInput)
		InputManager::instance.update();
}

// Only meaningful with the new input system; callers fall back to their own
// hardcoded keys when it is off.
bool InputManager_ModActionHeld(ModAction action)
{
	return Settings::UseNewInput && InputManager::instance.modActionHeld(action);
}

// The bound name with the new input system, or the fixed key the legacy paths
// use when it is off.
std::string InputManager_ModActionDisplayName(ModAction action)
{
	if (Settings::UseNewInput)
		return InputManager::instance.modActionDisplayName(action);

	switch (action)
	{
		case ModAction::OverlayToggle: return "F11";
		case ModAction::HudToggle:     return "F10";
		case ModAction::OpenChat:      return "Y";
		case ModAction::MusicNext:     return "X";
		case ModAction::MusicPrevious: return "Z";
		case ModAction::VRRecenter:    return "F12";
		default:                       return "(unbound)";
	}
}

void InputManager_SetVibration(WORD left, WORD right)
{
	InputManager::instance.setVibration(left, right);
}

void InputManager_StopVibration()
{
    InputManager::instance.stopVibration();
}

void InputManager_Shutdown()
{
	InputManager::instance.shutdown();
}

float InputManager_SteeringValue()
{
	return float(InputManager::instance.GetVolume(ADChannel::Steering)) / 127.0f;
}

class NewInputHook : public Hook
{
	inline static SafetyHookInline SwitchOn_hook = {};
	static int SwitchOn_dest(uint32_t switches)
	{
		// HACK: keyboard has ESC bound to both start & B/return, only let game see Start press when in-game
		if (InputManager::instance.lastInputSource() == InputSourceType::Keyboard)
			if (switches == StartSwitchMask && *Game::current_mode != STATE_GAME)
				return 0;

		return InputManager::instance.SwitchOn(switches);
	}


	inline static SafetyHookInline SwitchNow_hook = {};
	static int SwitchNow_dest(uint32_t switches)
	{
		// HACK: keyboard has ESC bound to both start & B/return, only let game see Start press when in-game
		if (InputManager::instance.lastInputSource() == InputSourceType::Keyboard)
			if (switches == StartSwitchMask && *Game::current_mode != STATE_GAME)
				return 0;

		return InputManager::instance.SwitchNow(switches);
	}

	inline static SafetyHookInline GetVolume_hook = {};
	static int GetVolume_dest(ADChannel volumeId)
	{
		int result = InputManager::instance.GetVolume(volumeId);
		if (Settings::FixFullPedalChecks) // TODO: might not be needed now that we ceil the result?
		{
			if (volumeId != ADChannel::Acceleration && volumeId != ADChannel::Brake)
				return result;
			if (result >= 254)
				result = 255;
		}
		return result;
	}
	inline static SafetyHookInline GetVolumeOld_hook = {};
	static int GetVolumeOld_dest(ADChannel volumeId)
	{
		int result = InputManager::instance.GetVolumeOld(volumeId);
		if (Settings::FixFullPedalChecks) // TODO: might not be needed now that we ceil the result?
		{
			if (volumeId != ADChannel::Acceleration && volumeId != ADChannel::Brake)
				return result;
			if (result >= 254)
				result = 255;
		}
		return result;
	}
	inline static SafetyHookInline VolumeSwitch_hook = {};
	static int VolumeSwitch_dest(ADChannel volumeId)
	{
		return VolumeSwitch_hook.ccall<int>(volumeId);
	}

	// ReadIO updates the dinput_state structs values with data from dinput.
	// Hook that so we can overwrite them afterward.
	// (needed due to some Sumo UI code peeking the dinput data directly instead of using SwitchOn/GetVolume/etc)
	inline static SafetyHookInline ReadIO_hook = {};
	static int ReadIO_dest()
	{
		int result = ReadIO_hook.ccall<int>();
		InputManager::instance.applyRawDInputState();
		return result;
	}

	inline static SafetyHookMid WindowInit_hook = {};
	static void WindowInit_dest(SafetyHookContext& ctx)
	{
		InputManager::instance.init((HWND)ctx.ebp);
	}

	inline static SafetyHookMid SumoUI_ControlConfiguration_hook = {};
	static void SumoUI_ControlConfiguration_dest(SafetyHookContext& ctx)
	{
		Overlay::RequestBindingDialog = true;
	}

public:
	std::string_view description() override
	{
		return "NewInputHook";
	}

	bool validate() override
	{
		return Settings::UseNewInput;
	}

	void declare_settings() override
	{
		Settings::UseNewInput.needs_restart();
		// Keep this hidden while the modern path is active: changing it live
		// would stop SDL updates before the legacy hooks exist. It becomes visible
		// after a user explicitly boots with UseNewInput=false.
		Settings::UseNewInput.hidden(Settings::UseNewInput);
		Settings::WheelInputCompatibility.needs_restart();
		Settings::InputBackend.needs_restart();
	}

	bool apply() override
	{
		// OR2 arcade menus check for "ChangeView" action for their Y/F2 button checks
		// All the C2C menus check for the "Y" action instead though (and the C2C X/F1 check uses "X" action)
		// Rather than needing to overload ChangeView with more binds, let's just patch OR2 to match C2C and check for "Y"
		constexpr uintptr_t Sel_ModeSel_Ctrl_ChangeViewMask = 0xC3F4C;
		constexpr uintptr_t Sel_Bgm_Ctrl_ChangeViewMask = 0xC49D6;

		Memory::VP::Patch(Module::exe_ptr(Sel_ModeSel_Ctrl_ChangeViewMask), uint32_t(1 << int(SwitchId::Y)));
		Memory::VP::Patch(Module::exe_ptr(Sel_Bgm_Ctrl_ChangeViewMask), uint32_t(1 << int(SwitchId::Y)));

		SwitchOn_hook = safetyhook::create_inline(Module::exe_ptr(0x536F0), SwitchOn_dest);
		SwitchNow_hook = safetyhook::create_inline(Module::exe_ptr(0x536C0), SwitchNow_dest);
		GetVolume_hook = safetyhook::create_inline(Module::exe_ptr(0x53720), GetVolume_dest);
		GetVolumeOld_hook = safetyhook::create_inline(Module::exe_ptr(0x53750), GetVolumeOld_dest);
		VolumeSwitch_hook = safetyhook::create_inline(Module::exe_ptr(0x53780), VolumeSwitch_dest);
		ReadIO_hook = safetyhook::create_inline(Module::exe_ptr(0x53BB0), ReadIO_dest);
		WindowInit_hook = safetyhook::create_mid(Module::exe_ptr(0xEB2B), WindowInit_dest);

		// Remove code that showed old config screen
		Memory::VP::Nop(Module::exe_ptr(0xD88D7), 0x1A);
		SumoUI_ControlConfiguration_hook = safetyhook::create_mid(Module::exe_ptr(0xD88D7), SumoUI_ControlConfiguration_dest);

		return true;
	}

	static NewInputHook instance;
};
NewInputHook NewInputHook::instance;
