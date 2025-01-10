#include <SDL3/SDL.h>
#include <unordered_map>
#include <vector>
#include <memory>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "overlay/overlay.hpp"

#include <array>

#include "imgui.h"
#include <format>
#include <string>

// fixups for SDL3 sillyness
#define SDL_GAMEPAD_BUTTON_A SDL_GAMEPAD_BUTTON_SOUTH
#define SDL_GAMEPAD_BUTTON_B SDL_GAMEPAD_BUTTON_EAST
#define SDL_GAMEPAD_BUTTON_X SDL_GAMEPAD_BUTTON_WEST
#define SDL_GAMEPAD_BUTTON_Y SDL_GAMEPAD_BUTTON_NORTH

constexpr float BindScreenTimeout = 5.f;

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
	virtual ~InputSource() = default;
	virtual float readValue(SDL_Gamepad* gamepad) = 0;
	virtual bool isAxis() const = 0;
	virtual bool isNegated() const = 0;
	virtual std::string getBindingName(bool isSteerAction = false) const = 0;
};

class GamepadSource : public InputSource
{
	static constexpr float StickRange = 32768.f;
	static constexpr float RStickDeadzone = 0.7f; // needs pretty high deadzone otherwise flicks will bounce

	union
	{
		SDL_GamepadAxis axis_;
		SDL_GamepadButton button_;
	};
	bool is_axis_ = false;
	bool negate_ = false;
public:
	GamepadSource(SDL_GamepadAxis ax, bool negate = false)
		: axis_(ax), is_axis_(true), negate_(negate)
	{
	}
	GamepadSource(SDL_GamepadButton btn, bool negate = false)
		: button_(btn), is_axis_(false), negate_(negate)
	{
	}

	float readValue(SDL_Gamepad* gamepad) override
	{
		if (!gamepad)
			return 0.0f;

		float value = 0.0f;
		if (!is_axis_)
			value = (float)SDL_GetGamepadButton(gamepad, button_);
		else
		{
			Sint16 raw = SDL_GetGamepadAxis(gamepad, axis_);

			int deadzone = 0;
			if (axis_ == SDL_GAMEPAD_AXIS_LEFTX || axis_ == SDL_GAMEPAD_AXIS_LEFTY)
				deadzone = (StickRange * Settings::SteeringDeadZone);
			else if (axis_ == SDL_GAMEPAD_AXIS_RIGHTX || axis_ == SDL_GAMEPAD_AXIS_RIGHTY)
				deadzone = (StickRange * RStickDeadzone);

			if (abs(raw) < deadzone)
				raw = 0;

			value = raw / StickRange;
		}

		return negate_ ? -value : value;
	}

	bool isAxis() const override { return is_axis_; }
	bool isNegated() const override { return negate_; }

	std::string getBindingName(bool isSteerAction) const override
	{
		if (isAxis())
		{
			std::string direction = isSteerAction ? "" : (isNegated() ? "+" : "-");
			switch (axis_)
			{
			case SDL_GAMEPAD_AXIS_LEFTX: return "Left Stick X" + direction;
			case SDL_GAMEPAD_AXIS_LEFTY: return "Left Stick Y" + direction;
			case SDL_GAMEPAD_AXIS_RIGHTX: return "Right Stick X" + direction;
			case SDL_GAMEPAD_AXIS_RIGHTY: return "Right Stick Y" + direction;
			case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "Left Trigger";
			case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "Right Trigger";
			default: return "Unknown Axis" + direction;
			}
		}

		switch (button_)
		{
		case SDL_GAMEPAD_BUTTON_A: return "A Button";
		case SDL_GAMEPAD_BUTTON_B: return "B Button";
		case SDL_GAMEPAD_BUTTON_X: return "X Button";
		case SDL_GAMEPAD_BUTTON_Y: return "Y Button";
		case SDL_GAMEPAD_BUTTON_BACK: return "Back";
		case SDL_GAMEPAD_BUTTON_START: return "Start";
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-Pad Up";
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-Pad Down";
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-Pad Left";
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-Pad Right";
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "L1/LB";
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "R1/RB";
		default: return "Unknown Button";
		}
	}

	SDL_GamepadButton button() const { return button_; }
	SDL_GamepadAxis axis() const { return axis_; }
};

class KeyboardSource : public InputSource
{
	SDL_Scancode key_;
	bool negate_;
public:
	KeyboardSource(SDL_Scancode k, bool negate = false) : key_(k), negate_(negate) {}

	float readValue(SDL_Gamepad* gamepad) override
	{
		const bool* state_array = SDL_GetKeyboardState(nullptr);
		return negate_ ? -float(state_array[key_]) : state_array[key_];
	}

	bool isAxis() const override { return false; }
	bool isNegated() const override { return negate_; }
	std::string getBindingName(bool isSteerAction) const override
	{
		return SDL_GetScancodeName(key_);
	}
	SDL_Scancode key() const { return key_; }
};

template<typename EnumType>
class InputAction
{
	std::vector<std::unique_ptr<InputSource>> sources_;
	InputState state;

public:
	const InputState& update(SDL_Gamepad* primary_pad)
	{
		float maxValue = 0.0f;
		bool isAxisInput = false;

		// Read all sources and take the highest absolute value
		for (auto& source : sources_)
		{
			float currentValue = source->readValue(primary_pad);
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
		sources_.push_back(std::make_unique<T>(std::forward<Args>(args)...));
	}

	void removeSourcesByType(bool keyboard)
	{
		sources_.erase(
			std::remove_if(
				sources_.begin(),
				sources_.end(),
				[keyboard](const std::unique_ptr<InputSource>& source) {
					if(keyboard)
						return dynamic_cast<const KeyboardSource*>(source.get()) != nullptr;
					return dynamic_cast<const GamepadSource*>(source.get()) != nullptr;
				}
			),
			sources_.end()
		);
	}

	std::vector<std::unique_ptr<InputSource>>& sources() { return sources_; }

	const InputState& getState() const { return state; }
	void setState(const InputState& state) { this->state = state; }

};

class InputManager
{
	std::array<InputAction<ADChannel>, size_t(ADChannel::Count)> volumeBindings;
	std::array<InputAction<SwitchId>, size_t(SwitchId::Count)> switchBindings;

	std::mutex mtx;
	std::vector<SDL_Gamepad*> controllers;
	int primaryControllerIndex = -1;

	SDL_Window* window = nullptr;

	// cached values as of last update call
	std::array<InputState, size_t(ADChannel::Count)> volumes;
	uint32_t switch_current;
	uint32_t switch_previous;
	uint32_t switch_overlay;

	// user settings
	bool BypassGameSensitivity = false;

public:
	~InputManager()
	{
		for (auto controller : controllers)
			SDL_CloseGamepad(controller);

		if (window)
		{
			SDL_DestroyWindow(window);
			window = nullptr;
		}

		SDL_Quit();
	}

	SDL_Gamepad* primary_gamepad()
	{
		if (primaryControllerIndex < 0)
			return nullptr;
		if (primaryControllerIndex >= controllers.size())
			return nullptr;
		return controllers[primaryControllerIndex];
	}

	void init(HWND hwnd)
	{
		SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
		SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
		SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "1");

		SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_VIDEO);

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

		setupDefaultBindings();
	}

	void setupDefaultBindings()
	{
		// Remove any previous bindings
		for (auto& binding : volumeBindings)
		{
			binding.removeSourcesByType(true);
			binding.removeSourcesByType(false);
		}
		for (auto& binding : switchBindings)
		{
			binding.removeSourcesByType(true);
			binding.removeSourcesByType(false);
		}

		// Keyboard
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

		// Gamepad
		addVolumeBinding<GamepadSource>(ADChannel::Steering, SDL_GAMEPAD_AXIS_LEFTX);
		addVolumeBinding<GamepadSource>(ADChannel::Steering, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
		addVolumeBinding<GamepadSource>(ADChannel::Steering, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
		addVolumeBinding<GamepadSource>(ADChannel::Acceleration, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
		addVolumeBinding<GamepadSource>(ADChannel::Brake, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);

		addSwitchBinding<GamepadSource>(SwitchId::GearUp, SDL_GAMEPAD_AXIS_RIGHTY);
		addSwitchBinding<GamepadSource>(SwitchId::GearDown, SDL_GAMEPAD_AXIS_RIGHTY, true);
		addSwitchBinding<GamepadSource>(SwitchId::GearUp, SDL_GAMEPAD_BUTTON_A);
		addSwitchBinding<GamepadSource>(SwitchId::GearDown, SDL_GAMEPAD_BUTTON_B);

		addSwitchBinding<GamepadSource>(SwitchId::Start, SDL_GAMEPAD_BUTTON_START);
		addSwitchBinding<GamepadSource>(SwitchId::Back, SDL_GAMEPAD_BUTTON_BACK);
		addSwitchBinding<GamepadSource>(SwitchId::A, SDL_GAMEPAD_BUTTON_A);
		addSwitchBinding<GamepadSource>(SwitchId::B, SDL_GAMEPAD_BUTTON_B);
		addSwitchBinding<GamepadSource>(SwitchId::X, SDL_GAMEPAD_BUTTON_X);
		addSwitchBinding<GamepadSource>(SwitchId::Y, SDL_GAMEPAD_BUTTON_Y);

		addSwitchBinding<GamepadSource>(SwitchId::ChangeView, SDL_GAMEPAD_BUTTON_Y);

		addSwitchBinding<GamepadSource>(SwitchId::SelectionUp, SDL_GAMEPAD_AXIS_LEFTY, true);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionDown, SDL_GAMEPAD_AXIS_LEFTY, false);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionLeft, SDL_GAMEPAD_AXIS_LEFTX, true);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionRight, SDL_GAMEPAD_AXIS_LEFTX, false);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionUp, SDL_GAMEPAD_BUTTON_DPAD_UP);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
		addSwitchBinding<GamepadSource>(SwitchId::SelectionRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

		// Some reason signin/license need both X/Y and SignIn/License bound, odd
		addSwitchBinding<GamepadSource>(SwitchId::SignIn, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
		addSwitchBinding<GamepadSource>(SwitchId::License, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
		addSwitchBinding<GamepadSource>(SwitchId::X, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
		addSwitchBinding<GamepadSource>(SwitchId::Y, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
	}

	void setupGamepad(SDL_Gamepad* controller)
	{
		Game::PadType = Game::GamepadType::Xbox;
		auto type = SDL_GetGamepadType(controller);
		switch (type)
		{
		case SDL_GAMEPAD_TYPE_PS3:
		case SDL_GAMEPAD_TYPE_PS4:
		case SDL_GAMEPAD_TYPE_PS5:
			Game::PadType = Game::GamepadType::PS;
			break;
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
			Game::PadType = Game::GamepadType::Switch;
			break;
		};
	}

	void onControllerAdded(SDL_JoystickID instanceId)
	{
		spdlog::debug(__FUNCTION__ "({})", instanceId);

		SDL_Gamepad* controller = SDL_OpenGamepad(instanceId);
		if (!controller)
		{
			spdlog::error(__FUNCTION__ "({}): !controller", instanceId);
			return;
		}

		std::lock_guard<std::mutex> lock(mtx);

		// check if we've already seen this controller, SDL sometimes sends two controller added events some reason
		if (std::find(controllers.begin(), controllers.end(), controller) != controllers.end())
		{
			spdlog::warn(__FUNCTION__ "({}): dupe, ignored", instanceId);
			SDL_CloseGamepad(controller);
			return;
		}

		controllers.push_back(controller);

		if (primaryControllerIndex == -1)
		{
			primaryControllerIndex = controllers.size() - 1;
			spdlog::debug("InputManager::primaryControllerIndex = {}", primaryControllerIndex);
			setupGamepad(controller); // Bind inputs to the new primary controller
		}
	}

	void onControllerRemoved(SDL_JoystickID instanceId)
	{
		spdlog::debug(__FUNCTION__ "(instance {})", instanceId);

		std::lock_guard<std::mutex> lock(mtx);

		auto it = std::find_if(controllers.begin(), controllers.end(), [instanceId](SDL_Gamepad* controller)
			{
				return SDL_GetJoystickID(SDL_GetGamepadJoystick(controller)) == instanceId;
			});

		if (it != controllers.end())
		{
			Game::PadType = Game::GamepadType::PC;

			SDL_CloseGamepad(*it);
			controllers.erase(it);

			spdlog::debug(__FUNCTION__ "(instance {}): removed instance", instanceId);

			if (primaryControllerIndex >= controllers.size())
			{
				if (controllers.empty())
					primaryControllerIndex = -1;
				else
				{
					primaryControllerIndex = 0;
					setupGamepad(controllers[primaryControllerIndex]); // Rebind inputs to next available controller
				}
				spdlog::debug("InputManager::primaryControllerIndex = {}", primaryControllerIndex);
			}
		}
	}

	static int Sumo_CalcSteerSensitivity_wrapper(int a1, int a2)
	{
		int returnValue;
		__asm {
			push ebx

			mov eax, a1
			mov ebx, a2

			call Game::Sumo_CalcSteerSensitivity

			mov returnValue, eax

			pop ebx
		}
		return returnValue;
	}

	void update()
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
			switch (event.type)
			{
			case SDL_EVENT_GAMEPAD_ADDED:
				onControllerAdded(event.gdevice.which);
				break;
			case SDL_EVENT_GAMEPAD_REMOVED:
				onControllerRemoved(event.gdevice.which);
				break;
			}

		auto* gamepad = primary_gamepad();

		// update & cache our bindings
		switch_previous = switch_current;
		switch_current = 0;

		// hacks to hide inputs until all buttons are released
		static bool disableOverlayInputs = false;
		static bool disableGameInputs = false;

		if (bindDialog.isListeningForInput == ListenState::Listening)
			disableOverlayInputs = true;
		if (Overlay::IsBindingDialogActive)
			disableGameInputs = true;

		for (size_t i = 0; i < volumeBindings.size(); ++i)
		{
			auto& vol = volumeBindings[i].update(gamepad);
			if (Overlay::IsBindingDialogActive) [[unlikely]]
				continue;

			volumes[i] = vol;
			if (i == 0 && !BypassGameSensitivity)
			{
				int cur = ceil(volumes[i].currentValue * 127.0f);
				int prev = ceil(volumes[i].previousValue * 127.0f);
				volumes[i].currentValue = Sumo_CalcSteerSensitivity_wrapper(cur, prev) / 127.0f;
				volumeBindings[i].setState(volumes[i]);
			}
		}

		for (size_t i = 0; i < switchBindings.size(); ++i)
			if (switchBindings[i].update(gamepad).isPressed())
				switch_current |= (1 << i);

		if (disableOverlayInputs && switch_current == 0) [[unlikely]]
			disableOverlayInputs = false;
		if (disableGameInputs && switch_current == 0) [[unlikely]]
			disableGameInputs = false;

		if (disableOverlayInputs) [[unlikely]]
			switch_current = 0;

		switch_overlay = switch_current;

		if (disableGameInputs || Overlay::IsBindingDialogActive) [[unlikely]]
			switch_current = 0;
	}

	void set_vibration(WORD left, WORD right)
	{
		auto* controller = primary_gamepad();
		if (!controller)
			return;

		std::lock_guard<std::mutex> lock(mtx);
		SDL_RumbleGamepad(controller, left, right, 1000);

		if (!Settings::ImpulseVibrationMode)
		{
			int impulseLeft = float(left);
			int impulseRight = float(right);

			if (Settings::ImpulseVibrationMode == 2) // Swap L/R
			{
				impulseLeft = float(right);
				impulseRight = float(left);
			}
			else if (Settings::ImpulseVibrationMode == 3) // Merge L/R by using whichever is highest
			{
				impulseLeft = impulseRight = max(left, right);
			}
			impulseLeft = impulseLeft * Settings::ImpulseVibrationLeftMultiplier;
			impulseRight = impulseRight * Settings::ImpulseVibrationRightMultiplier;
			SDL_RumbleGamepadTriggers(controller, Uint16(ceil(impulseLeft)), Uint16(ceil(impulseRight)), 1000);
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

	//
	// Input binding window
	//

	enum class ListenState
	{
		False = 0,
		WaitForButtonRelease = 1,
		Listening = 2
	};

	struct BindingDialogState
	{
		ListenState isListeningForInput = ListenState::False;
		bool issListeningForInput = false;
		std::string currentlyBinding;
		ADChannel currentVolumeChannel;
		SwitchId currentSwitchId;
		bool currentIsNegate = false;
		bool isBindingVolume = false;
		bool isBindingKeyboard = false;
		float bindScreenDisplayTime = 0.f;
	};

	BindingDialogState bindDialog;

	bool AnyInputPressed()
	{
		int key_count = 0;
		const bool* key_state = SDL_GetKeyboardState(&key_count);
		for (int i = 0; i < key_count; i++)
			if (key_state[i])
				return true;

		auto* controller = primary_gamepad();
		if (!controller)
			return false;

		// Check all possible buttons
		for (int i = SDL_GAMEPAD_BUTTON_SOUTH; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
			if (SDL_GetGamepadButton(controller, static_cast<SDL_GamepadButton>(i)))
				return true;
		
		// Check all possible axes
		for (int i = SDL_GAMEPAD_AXIS_LEFTX; i < SDL_GAMEPAD_AXIS_COUNT; i++)
		{
			float value = SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(i)) / 32768.0f;
			if (std::abs(value) > 0.5f)
				return true;
		}

		return false;
	}

	void HandleNewBinding()
	{
		auto* controller = primary_gamepad();

		// Handle keyboard binding
		if (bindDialog.isBindingKeyboard)
		{
			// Update timeout
			bindDialog.bindScreenDisplayTime -= ImGui::GetIO().DeltaTime;
			if (bindDialog.bindScreenDisplayTime <= 0.0f)
			{
				bindDialog.isListeningForInput = ListenState::False;
				ImGui::CloseCurrentPopup();
				return;
			}

			// Check all possible keys
			int key_count = 0;
			const bool* key_state = SDL_GetKeyboardState(&key_count);
			for (int i = 0; i < key_count; i++)
			{
				if (key_state[i])
				{
					SDL_Scancode key = static_cast<SDL_Scancode>(i);

					// remove existing keyboard bindings
					if (bindDialog.isBindingVolume)
					{
						auto& binding = volumeBindings[static_cast<int>(bindDialog.currentVolumeChannel)];
						binding.sources().erase(
							std::remove_if(
								binding.sources().begin(),
								binding.sources().end(),
								[this](const std::unique_ptr<InputSource>& source)
								{
									return dynamic_cast<const KeyboardSource*>(source.get()) != nullptr && source->isNegated() == bindDialog.currentIsNegate;
								}
							),
							binding.sources().end()
						);

						// If we're adding keyboard binding for steering, make sure we setup negate based on which button user clicked (left / right)
						if (bindDialog.currentVolumeChannel == ADChannel::Steering)
							binding.addSource<KeyboardSource>(key, bindDialog.currentIsNegate);
						else
							binding.addSource<KeyboardSource>(key);
					}
					else
					{
						auto& binding = switchBindings[static_cast<int>(bindDialog.currentSwitchId)];
						binding.sources().erase(
							std::remove_if(
								binding.sources().begin(),
								binding.sources().end(),
								[](const std::unique_ptr<InputSource>& source)
								{
									return dynamic_cast<const KeyboardSource*>(source.get()) != nullptr;
								}
							),
							binding.sources().end()
						);
						binding.addSource<KeyboardSource>(key);
					}

					bindDialog.isListeningForInput = ListenState::False;
					ImGui::CloseCurrentPopup();
					return;
				}
			}
		}
		// Handle controller binding
		else if (controller)
		{
			// Check all possible buttons
			for (int i = SDL_GAMEPAD_BUTTON_SOUTH; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
			{
				if (SDL_GetGamepadButton(controller, static_cast<SDL_GamepadButton>(i)))
				{
					if (bindDialog.isBindingVolume)
					{
						auto& binding = volumeBindings[static_cast<int>(bindDialog.currentVolumeChannel)];
						binding.sources().erase(
							std::remove_if(
								binding.sources().begin(),
								binding.sources().end(),
								[](const std::unique_ptr<InputSource>& source)
								{
									return dynamic_cast<const GamepadSource*>(source.get()) != nullptr;
								}
							),
							binding.sources().end()
						);
						binding.addSource<GamepadSource>(static_cast<SDL_GamepadButton>(i));
					}
					else
					{
						auto& binding = switchBindings[static_cast<int>(bindDialog.currentSwitchId)];
						binding.sources().erase(
							std::remove_if(
								binding.sources().begin(),
								binding.sources().end(),
								[](const std::unique_ptr<InputSource>& source)
								{
									return dynamic_cast<const GamepadSource*>(source.get()) != nullptr;
								}
							),
							binding.sources().end()
						);
						binding.addSource<GamepadSource>(static_cast<SDL_GamepadButton>(i));
					}

					bindDialog.isListeningForInput = ListenState::False;
					ImGui::CloseCurrentPopup();
					return;
				}
			}

			// Check all possible axes
			for (int i = SDL_GAMEPAD_AXIS_LEFTX; i < SDL_GAMEPAD_AXIS_COUNT; i++)
			{
				float value = SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(i)) / 32768.0f;
				bool negate = value < 0;
				if (bindDialog.isBindingVolume && bindDialog.currentVolumeChannel == ADChannel::Steering)
					negate = false;

				if (std::abs(value) > 0.5f)
				{
					if (bindDialog.isBindingVolume)
					{
						auto& binding = volumeBindings[static_cast<int>(bindDialog.currentVolumeChannel)];
						binding.sources().erase(
							std::remove_if(
								binding.sources().begin(),
								binding.sources().end(),
								[](const std::unique_ptr<InputSource>& source)
								{
									return dynamic_cast<const GamepadSource*>(source.get()) != nullptr;
								}
							),
							binding.sources().end()
						);
						binding.addSource<GamepadSource>(static_cast<SDL_GamepadAxis>(i), negate);
					}
					else
					{
						auto& binding = switchBindings[static_cast<int>(bindDialog.currentSwitchId)];
						binding.sources().erase(
							std::remove_if(
								binding.sources().begin(),
								binding.sources().end(),
								[](const std::unique_ptr<InputSource>& source)
								{
									return dynamic_cast<const GamepadSource*>(source.get()) != nullptr;
								}
							),
							binding.sources().end()
						);
						binding.addSource<GamepadSource>(static_cast<SDL_GamepadAxis>(i), negate);
					}

					bindDialog.isListeningForInput = ListenState::False;
					ImGui::CloseCurrentPopup();
					return;
				}
			}
		}
	}

	bool RenderBindingDialog()
	{
		bool dialogOpen = true;
		if ((switch_overlay & (1 << int(SwitchId::Back) | 1 << int(SwitchId::B))) != 0)
			dialogOpen = false;

		ImGui::OpenPopup("Input Bindings");
		if (ImGui::BeginPopupModal("Input Bindings", &dialogOpen, ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Note: settings here currently aren't saved, fix soon.");

			if (ImGui::BeginTable("Controllers", 2, ImGuiTableFlags_Borders))
			{
				ImGui::TableSetupColumn("Detected Controllers");
				ImGui::TableSetupColumn("Type");
				ImGui::TableHeadersRow();

				if (controllers.empty())
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Text("No controllers detected");
					ImGui::TableNextColumn();
				}
				else
				{
					for (size_t i = 0; i < controllers.size(); i++)
					{
						ImGui::TableNextRow();
						ImGui::TableNextColumn();

						auto* controller = controllers[i];
						std::string name = SDL_GetGamepadName(controller);

						if(ImGui::Button(name.c_str()))
						{
							primaryControllerIndex = i;
							setupGamepad(controller);
						}

						ImGui::TableNextColumn();

						if (int(i) == primaryControllerIndex)
							ImGui::Text("Active/Primary");
						else
							ImGui::Text("Inactive");
					}
				}
				ImGui::EndTable();
			}

			ImGui::Separator();

			// Analog Controls Table
			if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (ImGui::BeginTable("VolumeBindings", 3, ImGuiTableFlags_Borders))
				{
					ImGui::TableSetupColumn("Action");
					ImGui::TableSetupColumn("Controller Binding");
					ImGui::TableSetupColumn("Keyboard Binding");
					ImGui::TableHeadersRow();

					const char* volumeNames[] = {
						"Steering",
						"Acceleration",
						"Brake"
					};

					for (int i = 0; i < 3; i++)
					{
						ADChannel volChannel = ADChannel(i);
						auto& volAction = volumeBindings[i];

						ImGui::TableNextRow();

						// Action name
						ImGui::TableNextColumn();
						ImGui::Text("%s", volumeNames[i]);

						// Controller binding
						{
							ImGui::TableNextColumn();
							std::string controllerBind = "None";
							for (const auto& source : volAction.sources())
							{
								if (dynamic_cast<const GamepadSource*>(source.get()))
								{
									controllerBind = source->getBindingName(i == 0);
									break;
								}
							}
							if (ImGui::Button(std::format("{}##vol_ctrl_{}", controllerBind, i).c_str()))
							{
								bindDialog.isListeningForInput = ListenState::WaitForButtonRelease;
								bindDialog.isBindingVolume = true;
								bindDialog.isBindingKeyboard = false;
								bindDialog.currentVolumeChannel = static_cast<ADChannel>(i);
								bindDialog.currentlyBinding = volumeNames[i];
							}
						}

						// Keyboard binding
						{
							ImGui::TableNextColumn();
							std::string kbd_negative = "None";
							std::string kbd_positive = "None";
							for (const auto& source : volAction.sources())
							{
								if (dynamic_cast<const KeyboardSource*>(source.get()))
								{
									if (volChannel != ADChannel::Steering)
									{
										kbd_negative = source->getBindingName();
										break;
									}
									else
									{
										if (source->isNegated())
											kbd_negative = source->getBindingName(true);
										else
											kbd_positive = source->getBindingName(true);
									}
								}
							}
							if (ImGui::Button(std::format("{}##vol_kb_neg_{}", kbd_negative, i).c_str()))
							{
								bindDialog.isListeningForInput = ListenState::WaitForButtonRelease;
								bindDialog.isBindingVolume = true;
								bindDialog.isBindingKeyboard = true;
								bindDialog.currentVolumeChannel = static_cast<ADChannel>(i);
								bindDialog.currentlyBinding = volumeNames[i];
								bindDialog.currentIsNegate = volChannel == ADChannel::Steering ? true : false;
								bindDialog.bindScreenDisplayTime = BindScreenTimeout;
							}

							if (volChannel == ADChannel::Steering)
							{
								ImGui::SameLine();
								if (ImGui::Button(std::format("{}##vol_kb_pos_{}", kbd_positive, i).c_str()))
								{
									bindDialog.isListeningForInput = ListenState::WaitForButtonRelease;
									bindDialog.isBindingVolume = true;
									bindDialog.isBindingKeyboard = true;
									bindDialog.currentVolumeChannel = static_cast<ADChannel>(i);
									bindDialog.currentlyBinding = volumeNames[i];
									bindDialog.currentIsNegate = false;
									bindDialog.bindScreenDisplayTime = BindScreenTimeout;
								}
							}
						}
					}

					const char* switchNames[] = {
						"Start",
						"Back",
						"A",
						"B",
						"X",
						"Y",
						"Gear Down",
						"Gear Up",
						"Unk0x100",
						"Unk0x200",
						"Selection Up",
						"Selection Down",
						"Selection Left",
						"Selection Right",
						"License",
						"Sign In",
						"Unk0x10000",
						"Unk0x20000",
						"Change View"
					};

					for (int i = 0; i < int(SwitchId::Count); i++)
					{
						SwitchId switchId = SwitchId(i);

						if (switchId == SwitchId::Unknown0x100 || switchId == SwitchId::Unknown0x200 ||
							switchId == SwitchId::Unknown0x10000 || switchId == SwitchId::Unknown0x20000)
						{
							continue;
						}

						auto& switchAction = switchBindings[i];

						ImGui::TableNextRow();

						// Action name
						ImGui::TableNextColumn();
						ImGui::Text("%s", switchNames[i]);

						// Controller binding
						{
							ImGui::TableNextColumn();
							std::string controllerBind = "None";
							for (const auto& source : switchAction.sources())
							{
								if (dynamic_cast<const GamepadSource*>(source.get()))
								{
									controllerBind = source->getBindingName();
									break;
								}
							}
							if (ImGui::Button(std::format("{}##switch_ctrl_{}", controllerBind, i).c_str()))
							{
								bindDialog.isListeningForInput = ListenState::WaitForButtonRelease;
								bindDialog.isBindingVolume = false;
								bindDialog.isBindingKeyboard = false;
								bindDialog.currentSwitchId = static_cast<SwitchId>(i);
								bindDialog.currentlyBinding = switchNames[i];
							}
						}

						// Keyboard binding
						{
							ImGui::TableNextColumn();
							std::string keyboardBind = "None";
							for (const auto& source : switchAction.sources())
							{
								if (dynamic_cast<const KeyboardSource*>(source.get()))
								{
									keyboardBind = source->getBindingName(false);
									break;
								}
							}
							if (ImGui::Button(std::format("{}##switch_kb_{}", keyboardBind, i).c_str()))
							{
								bindDialog.isListeningForInput = ListenState::WaitForButtonRelease;
								bindDialog.isBindingVolume = false;
								bindDialog.isBindingKeyboard = true;
								bindDialog.currentSwitchId = static_cast<SwitchId>(i);
								bindDialog.currentlyBinding = switchNames[i];
								bindDialog.bindScreenDisplayTime = BindScreenTimeout;
							}
						}
					}
					ImGui::EndTable();
				}
			}

			int deadzonePercent = Settings::SteeringDeadZone * 100.f;
			if (ImGui::SliderInt("Steering Deadzone", &deadzonePercent, 5, 20, "%d%%"))
				Settings::SteeringDeadZone = float(deadzonePercent) / 100.f;

			ImGui::Checkbox("Bypass Sensitivity", &BypassGameSensitivity);
			if(ImGui::IsItemHovered())
				ImGui::SetTooltip("Passes steering input to the game directly, allows for more sensitive controls");

			if (ImGui::Button("Return to game"))
				dialogOpen = false;
			ImGui::SameLine();

			static bool showReallyPrompt = false;
			if (ImGui::Button(!showReallyPrompt ? "Reset to default##clear" : "Are you sure?##clear"))
			{
				if (!showReallyPrompt)
				{
					showReallyPrompt = true;
				}
				else
				{
					Settings::SteeringDeadZone = 0.2f;
					BypassGameSensitivity = false;
					setupDefaultBindings();
					if (auto* controller = primary_gamepad())
						setupGamepad(controller);
					showReallyPrompt = false;
				}
			}

			// Input listening overlay
			if (bindDialog.isListeningForInput != ListenState::False)
			{
				if (bindDialog.isListeningForInput == ListenState::WaitForButtonRelease)
				{
					// wait for user to release all buttons before we start listening
					if (!AnyInputPressed())
					{
						bindDialog.isListeningForInput = ListenState::Listening;
					}
				}
				else if (bindDialog.isListeningForInput == ListenState::Listening)
				{
					ImGui::OpenPopup("Listening for Input");

					if (ImGui::BeginPopupModal("Listening for Input", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
					{
						ImGui::Text("Press %s input to bind to %s",
							bindDialog.isBindingKeyboard ? "keyboard" : "controller",
							bindDialog.currentlyBinding.c_str());

						if(bindDialog.isBindingKeyboard)
							ImGui::Text("Aborting in %.1f seconds", bindDialog.bindScreenDisplayTime);
						else
							ImGui::Text("Press ESC to abort");

						// Check for binding presses
						HandleNewBinding();

						if (!bindDialog.isBindingKeyboard && ImGui::IsKeyPressed(ImGuiKey_Escape))
						{
							bindDialog.isListeningForInput = ListenState::False;
							ImGui::CloseCurrentPopup();
						}

						ImGui::EndPopup();
					}
				}
			}
			ImGui::EndPopup();
		}

		return dialogOpen;
	}

	static InputManager instance;
};
InputManager InputManager::instance;

void InputManager_Update()
{
	if (Settings::UseNewInput)
		InputManager::instance.update();
}

void InputManager_SetVibration(WORD left, WORD right)
{
	InputManager::instance.set_vibration(left, right);
}

bool InputManager_RenderBindingDialog()
{
	return Settings::UseNewInput ? InputManager::instance.RenderBindingDialog() : false;
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
