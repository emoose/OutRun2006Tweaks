#include "input_manager.hpp"

InputManager InputManager::instance;

void InputManager_Update()
{
	if (Settings::UseNewInput)
		InputManager::instance.update();
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

	bool apply() override
	{
		SwitchOn_hook = safetyhook::create_inline(Module::exe_ptr(0x536F0), SwitchOn_dest);
		SwitchNow_hook = safetyhook::create_inline(Module::exe_ptr(0x536C0), SwitchNow_dest);
		GetVolume_hook = safetyhook::create_inline(Module::exe_ptr(0x53720), GetVolume_dest);
		GetVolumeOld_hook = safetyhook::create_inline(Module::exe_ptr(0x53750), GetVolumeOld_dest);
		VolumeSwitch_hook = safetyhook::create_inline(Module::exe_ptr(0x53780), VolumeSwitch_dest);
		WindowInit_hook = safetyhook::create_mid(Module::exe_ptr(0xEB2B), WindowInit_dest);

		// Remove code that showed old config screen
		Memory::VP::Nop(Module::exe_ptr(0xD88D7), 0x1A);
		SumoUI_ControlConfiguration_hook = safetyhook::create_mid(Module::exe_ptr(0xD88D7), SumoUI_ControlConfiguration_dest);

		return true;
	}

	static NewInputHook instance;
};
NewInputHook NewInputHook::instance;
