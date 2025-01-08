#include <SDL.h>
#include <unordered_map>
#include <vector>
#include <memory>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <array>

struct InputState
{
	float currentValue = 0.0f;
	float previousValue = 0.0f;
	bool isAxis = false;

	void update(float newValue)
	{
		previousValue = currentValue;
		currentValue = newValue;
	}

	bool isNewlyPressed(float threshold = 0.5f) const
	{
		return currentValue >= threshold && previousValue < threshold;
	}

	bool isPressed(float threshold = 0.5f) const
	{
		return currentValue >= threshold;
	}
};

class InputSource
{
public:
	virtual float readValue() = 0;
	virtual bool isAxis() const = 0;
	virtual bool isController(SDL_GameController* controller) const = 0;
	virtual ~InputSource() = default;
};

class GamepadSource : public InputSource
{
	static constexpr float StickRange = 32768.f;
	static constexpr float RStickDeadzone = 0.7f; // needs pretty high deadzone otherwise flicks will bounce

	SDL_GameController* controller;
	union
	{
		SDL_GameControllerAxis axis;
		SDL_GameControllerButton button;
	};
	bool is_axis = false;
	bool negate = false;
public:
	GamepadSource(SDL_GameController* ctrl, SDL_GameControllerAxis ax, bool negate = false)
		: controller(ctrl), axis(ax), is_axis(true), negate(negate)
	{
	}
	GamepadSource(SDL_GameController* ctrl, SDL_GameControllerButton btn, bool negate = false)
		: controller(ctrl), button(btn), is_axis(false), negate(negate)
	{
	}

	float readValue() override
	{
		float value = 0.0f;
		if (!is_axis)
			value = (float)SDL_GameControllerGetButton(controller, button);
		else
		{
			Sint16 raw = SDL_GameControllerGetAxis(controller, axis);

			int deadzone = 0;
			if (axis == SDL_CONTROLLER_AXIS_LEFTX || axis == SDL_CONTROLLER_AXIS_LEFTY)
				deadzone = (StickRange * Settings::SteeringDeadZone);
			else if (axis == SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_CONTROLLER_AXIS_RIGHTY)
				deadzone = (StickRange * RStickDeadzone);

			if (abs(raw) < deadzone)
				raw = 0;

			value = raw / StickRange;
		}

		return negate ? -value : value;
	}

	bool isAxis() const override { return true; }
	bool isController(SDL_GameController* controller) const override { return controller == this->controller; }
};

class KeyboardSource : public InputSource
{
	SDL_Scancode key;
	bool negate;
public:
	KeyboardSource(SDL_Scancode k, bool negate = false) : key(k), negate(negate) {}

	float readValue() override
	{
		const Uint8* state_array = SDL_GetKeyboardState(nullptr);
		return negate ? -float(state_array[key]) : state_array[key];
	}

	bool isAxis() const override { return false; }
	bool isController(SDL_GameController* controller) const override { return false; }
};

template<typename EnumType>
class InputAction
{
	std::vector<std::unique_ptr<InputSource>> sources;
	InputState state;

public:
	const InputState& update()
	{
		float maxValue = 0.0f;
		bool isAxisInput = false;

		// Read all sources and take the highest absolute value
		for (auto& source : sources)
		{
			float currentValue = source->readValue();
			if (std::abs(currentValue) > std::abs(maxValue))
			{
				maxValue = currentValue;
				isAxisInput = source->isAxis();
			}
		}

		state.isAxis = isAxisInput;
		state.update(maxValue);
		return state;
	}

	template <typename T, typename... Args>
	void addSource(Args&&... args)
	{
		sources.push_back(std::make_unique<T>(std::forward<Args>(args)...));
	}

	void removeSourceController(SDL_GameController* controller)
	{
		sources.erase(
			std::remove_if(
				sources.begin(),
				sources.end(),
				[controller](const std::unique_ptr<InputSource>& source) {
					return source->isController(controller);
				}
			),
			sources.end()
		);
	}

	const InputState& getState() const { return state; }
};

class InputManager
{
	std::array<InputAction<ADChannel>, size_t(ADChannel::Count)> volumeBindings;
	std::array<InputAction<SwitchId>, size_t(SwitchId::Count)> switchBindings;

	std::mutex mtx;
	std::vector<SDL_GameController*> controllers;
	int primaryControllerIndex = -1;

	SDL_Window* window = nullptr;

	// cached values as of last update call
	std::array<InputState, size_t(ADChannel::Count)> volumes;
	uint32_t switch_current;
	uint32_t switch_previous;

public:
	~InputManager()
	{
		for (auto controller : controllers)
			SDL_GameControllerClose(controller);

		if (window)
		{
			SDL_DestroyWindow(window);
			window = nullptr;
		}

		SDL_Quit();
	}

	void init(HWND hwnd)
	{
		SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
		SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
		SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "1");

		SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_VIDEO);

		window = SDL_CreateWindowFrom(hwnd); // Needed for SDL keyboard events

		setupKeyboardBindings();
	}

	void setupKeyboardBindings()
	{
		addVolumeBinding<KeyboardSource>(ADChannel::Steering, SDL_SCANCODE_LEFT, true);
		addVolumeBinding<KeyboardSource>(ADChannel::Steering, SDL_SCANCODE_RIGHT);
		addVolumeBinding<KeyboardSource>(ADChannel::Acceleration, SDL_SCANCODE_UP);
		addVolumeBinding<KeyboardSource>(ADChannel::Brake, SDL_SCANCODE_DOWN);

		addSwitchBinding<KeyboardSource>(SwitchId::GearUp, SDL_SCANCODE_W);
		addSwitchBinding<KeyboardSource>(SwitchId::GearDown, SDL_SCANCODE_D);

		addSwitchBinding<KeyboardSource>(SwitchId::Start, SDL_SCANCODE_ESCAPE);
		addSwitchBinding<KeyboardSource>(SwitchId::Back, SDL_SCANCODE_ESCAPE);
		addSwitchBinding<KeyboardSource>(SwitchId::A, SDL_SCANCODE_RETURN);
		addSwitchBinding<KeyboardSource>(SwitchId::B, SDL_SCANCODE_ESCAPE);
		addSwitchBinding<KeyboardSource>(SwitchId::X, SDL_SCANCODE_E);
		addSwitchBinding<KeyboardSource>(SwitchId::Y, SDL_SCANCODE_F);

		addSwitchBinding<KeyboardSource>(SwitchId::ChangeView, SDL_SCANCODE_F);

		addSwitchBinding<KeyboardSource>(SwitchId::SelectionUp, SDL_SCANCODE_UP);
		addSwitchBinding<KeyboardSource>(SwitchId::SelectionDown, SDL_SCANCODE_DOWN);
		addSwitchBinding<KeyboardSource>(SwitchId::SelectionLeft, SDL_SCANCODE_LEFT);
		addSwitchBinding<KeyboardSource>(SwitchId::SelectionRight, SDL_SCANCODE_RIGHT);

		addSwitchBinding<KeyboardSource>(SwitchId::SignIn, SDL_SCANCODE_F1);
		addSwitchBinding<KeyboardSource>(SwitchId::License, SDL_SCANCODE_F2);
		addSwitchBinding<KeyboardSource>(SwitchId::X, SDL_SCANCODE_F1);
		addSwitchBinding<KeyboardSource>(SwitchId::Y, SDL_SCANCODE_F2);
	}

	void setupGamepadBindings(SDL_GameController* controller)
	{
		Game::PadType = Game::GamepadType::Xbox;
		auto type = SDL_GameControllerGetType(controller);
		switch (type)
		{
		case SDL_CONTROLLER_TYPE_PS3:
		case SDL_CONTROLLER_TYPE_PS4:
		case SDL_CONTROLLER_TYPE_PS5:
			Game::PadType = Game::GamepadType::PS;
			break;
		case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
		case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
		case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
		case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
			Game::PadType = Game::GamepadType::Switch;
			break;
		};

		addVolumeBinding<GamepadSource>(ADChannel::Steering, controller, SDL_CONTROLLER_AXIS_LEFTX);
		addVolumeBinding<GamepadSource>(ADChannel::Steering, controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT, true);
		addVolumeBinding<GamepadSource>(ADChannel::Steering, controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
		addVolumeBinding<GamepadSource>(ADChannel::Acceleration, controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
		addVolumeBinding<GamepadSource>(ADChannel::Brake, controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);

		addSwitchBinding<GamepadSource>(SwitchId::GearUp, controller, SDL_CONTROLLER_AXIS_RIGHTY);
		addSwitchBinding<GamepadSource>(SwitchId::GearDown, controller, SDL_CONTROLLER_AXIS_RIGHTY, true);
		addSwitchBinding<GamepadSource>(SwitchId::GearUp, controller, SDL_CONTROLLER_BUTTON_A);
		addSwitchBinding<GamepadSource>(SwitchId::GearDown, controller, SDL_CONTROLLER_BUTTON_B);

		addSwitchBinding<GamepadSource>(SwitchId::Start, controller, SDL_CONTROLLER_BUTTON_START);
		addSwitchBinding<GamepadSource>(SwitchId::Back, controller, SDL_CONTROLLER_BUTTON_BACK);
		addSwitchBinding<GamepadSource>(SwitchId::A, controller, SDL_CONTROLLER_BUTTON_A);
		addSwitchBinding<GamepadSource>(SwitchId::B, controller, SDL_CONTROLLER_BUTTON_B);
		addSwitchBinding<GamepadSource>(SwitchId::X, controller, SDL_CONTROLLER_BUTTON_X);
		addSwitchBinding<GamepadSource>(SwitchId::Y, controller, SDL_CONTROLLER_BUTTON_Y);

		addSwitchBinding<GamepadSource>(SwitchId::ChangeView, controller, SDL_CONTROLLER_BUTTON_Y);

		addSwitchBinding<GamepadSource>(SwitchId::SelectionUp, controller, SDL_CONTROLLER_AXIS_LEFTY, true);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionDown, controller, SDL_CONTROLLER_AXIS_LEFTY, false);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionLeft, controller, SDL_CONTROLLER_AXIS_LEFTX, true);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionRight, controller, SDL_CONTROLLER_AXIS_LEFTX, false);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionUp, controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionDown, controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionLeft, controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionRight, controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

		// Some reason signin/license need both X/Y and SignIn/License bound, odd
		addSwitchBinding<GamepadSource>(SwitchId::SignIn, controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
		addSwitchBinding<GamepadSource>(SwitchId::License, controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
		addSwitchBinding<GamepadSource>(SwitchId::X, controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
		addSwitchBinding<GamepadSource>(SwitchId::Y, controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
	}

	void onControllerAdded(int deviceIndex)
	{
		spdlog::debug(__FUNCTION__ "({})", deviceIndex);

		SDL_GameController* controller = SDL_GameControllerOpen(deviceIndex);
		if (!controller)
		{
			spdlog::error(__FUNCTION__ "({}): !controller", deviceIndex);
			return;
		}

		std::lock_guard<std::mutex> lock(mtx);

		// check if we've already seen this controller, SDL sometimes sends two controller added events some reason
		if (std::find(controllers.begin(), controllers.end(), controller) != controllers.end())
		{
			spdlog::warn(__FUNCTION__ "({}): dupe, ignored", deviceIndex);
			SDL_GameControllerClose(controller);
			return;
		}

		controllers.push_back(controller);

		if (primaryControllerIndex == -1)
		{
			primaryControllerIndex = controllers.size() - 1;
			spdlog::debug("InputManager::primaryControllerIndex = {}", primaryControllerIndex);
			setupGamepadBindings(controller); // Bind inputs to the new primary controller
		}
	}

	void onControllerRemoved(int instanceId)
	{
		spdlog::debug(__FUNCTION__ "(instance {})", instanceId);

		std::lock_guard<std::mutex> lock(mtx);

		auto it = std::find_if(controllers.begin(), controllers.end(), [instanceId](SDL_GameController* controller)
			{
				return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == instanceId;
			});

		if (it != controllers.end())
		{
			Game::PadType = Game::GamepadType::PC;

			for (auto& binding : volumeBindings)
				binding.removeSourceController(*it);
			for (auto& binding : switchBindings)
				binding.removeSourceController(*it);

			SDL_GameControllerClose(*it);
			controllers.erase(it);

			spdlog::debug(__FUNCTION__ "(instance {}): removed instance", instanceId);

			if (primaryControllerIndex >= controllers.size())
			{
				if (controllers.empty())
					primaryControllerIndex = -1;
				else
				{
					primaryControllerIndex = 0;
					setupGamepadBindings(controllers[primaryControllerIndex]); // Rebind inputs to next available controller
				}
				spdlog::debug("InputManager::primaryControllerIndex = {}", primaryControllerIndex);
			}
		}
	}

	void update()
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
			switch (event.type)
			{
			case SDL_CONTROLLERDEVICEADDED:
				onControllerAdded(event.cdevice.which);
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				onControllerRemoved(event.cdevice.which);
				break;
			}

		// update & cache our bindings
		switch_previous = switch_current;
		switch_current = 0;

		for (size_t i = 0; i < volumeBindings.size(); ++i)
			volumes[i] = volumeBindings[i].update();

		for (size_t i = 0; i < switchBindings.size(); ++i)
			if (switchBindings[i].update().isPressed())
				switch_current |= (1 << i);
	}

	void set_vibration(WORD left, WORD right)
	{
		if (primaryControllerIndex != -1)
		{
			std::lock_guard<std::mutex> lock(mtx);
			SDL_GameControllerRumble(controllers[primaryControllerIndex], left, right, 1000);
			SDL_GameControllerRumbleTriggers(controllers[primaryControllerIndex], left, right, 1000);
		}
	}

	// Add sources to bindings
	template <typename T, typename... Args>
	void addVolumeBinding(ADChannel id, Args&&... args)
	{
		volumeBindings[int(id)].addSource<T>(std::forward<Args>(args)...);
	}

	template <typename T, typename... Args>
	void addSwitchBinding(SwitchId id, Args&&... args)
	{
		switchBindings[int(id)].addSource<T>(std::forward<Args>(args)...);
	}

	// Handlers for games original input functions
	int GetVolume(ADChannel volumeId)
	{
		const auto& state = volumes[int(volumeId)];
		if (volumeId == ADChannel::Steering)
			return int(ceil(state.currentValue * 127.0f));

		return int(ceil(state.currentValue * 255.0f));
	}

	int GetVolumeOld(ADChannel volumeId)
	{
		const auto& state = volumes[int(volumeId)];
		if (volumeId == ADChannel::Steering)
			return int(ceil(state.previousValue * 127.0f));

		return int(ceil(state.previousValue * 255.0f));
	}

	bool SwitchOn(uint32_t switches)
	{
		return (switch_previous & switches) != switches && (switch_current & switches) == switches;
	}

	bool SwitchNow(uint32_t switches)
	{
		return (switch_current & switches) == switches;
	}

	static InputManager instance;
};
InputManager InputManager::instance;

void InputManager_Init(HWND window)
{
	if (Settings::UseNewInput)
		InputManager::instance.init(window);
}

void InputManager_Update()
{
	if (Settings::UseNewInput)
		InputManager::instance.update();
}

void InputManager_SetVibration(WORD left, WORD right)
{
	InputManager::instance.set_vibration(left, right);
}

class NewInputHook : public Hook
{
	inline static SafetyHookInline SwitchOn_hook = {};
	static int SwitchOn_dest(uint32_t switches)
	{
		// HACK: keyboard has ESC bound to both start & B/return, don't let menus see start button presses otherwise returning won't work
		if (switches == (1 << int(SwitchId::Start)) && *Game::current_mode == STATE_SUMO_FE)
			return 0;

		return InputManager::instance.SwitchOn(switches);
	}

	inline static SafetyHookInline SwitchNow_hook = {};
	static int SwitchNow_dest(uint32_t switches)
	{
		// HACK: keyboard has ESC bound to both start & B/return, don't let menus see start button presses otherwise returning won't work
		if (switches == (1 << int(SwitchId::Start)) && *Game::current_mode == STATE_SUMO_FE)
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

		return true;
	}

	static NewInputHook instance;
};
NewInputHook NewInputHook::instance;
