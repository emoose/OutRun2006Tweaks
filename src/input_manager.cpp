#include "input_manager.hpp"

namespace Settings
{
	Setting<bool> UseNewInput{ "Controls", "UseNewInput", true,
		"Enables new SDL-based input system, allowing game to see full trigger range without any shared trigger axes issues "
		"(experimental, not every menu/gamemode has been tested with it yet)." };
	Setting<bool> BypassGameSensitivity{ "Controls", "BypassGameSensitivity", false,
		"Passes steering input to the game directly instead of through its own sensitivity curve, allowing for more "
		"sensitive controls. Only used when UseNewInput is enabled." };
}

InputManager InputManager::instance;

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
	case ModAction::MusicNext:     return "X";
	case ModAction::MusicPrevious: return "Z";
	default:                       return "(unbound)";
	}
}

void InputManager_SetVibration(WORD left, WORD right)
{
	InputManager::instance.setVibration(left, right);
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
		Settings::UseNewInput.hidden(true);
	}

	bool apply() override
	{
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
