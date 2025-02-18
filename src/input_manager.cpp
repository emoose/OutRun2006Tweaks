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
#include <fstream>

// fixups for SDL3 sillyness
#define SDL_GAMEPAD_BUTTON_A SDL_GAMEPAD_BUTTON_SOUTH
#define SDL_GAMEPAD_BUTTON_B SDL_GAMEPAD_BUTTON_EAST
#define SDL_GAMEPAD_BUTTON_X SDL_GAMEPAD_BUTTON_WEST
#define SDL_GAMEPAD_BUTTON_Y SDL_GAMEPAD_BUTTON_NORTH

enum class ListenState
{
	False = 0,
	WaitForButtonRelease = 1,
	Listening = 2,
	WaitForBindButtonRelease = 3
};

ListenState isListeningForInput = ListenState::False;

enum class InputSourceType
{
	GamePad,
	Keyboard
};

struct InputState
{
	float currentValue = 0.0f;
	float previousValue = 0.0f;
	bool isAxis = false;
	InputSourceType lastSourceType;

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
protected:
	InputSourceType type_ = InputSourceType::GamePad;

public:
	virtual ~InputSource() = default;
	virtual float readValue(SDL_Gamepad* gamepad) = 0;
	virtual bool isAxis() const = 0;
	virtual bool isNegated() const = 0;
	virtual std::string getBindingName(SDL_GamepadType padType = SDL_GAMEPAD_TYPE_UNKNOWN, bool isSteerAction = false) const = 0;
	virtual std::string getBindingIniDefinition() const = 0;

	InputSourceType type() { return type_; }
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
		type_ = InputSourceType::GamePad;
	}
	GamepadSource(SDL_GamepadButton btn, bool negate = false)
		: button_(btn), is_axis_(false), negate_(negate)
	{
		type_ = InputSourceType::GamePad;
	}

	void set(SDL_GamepadAxis ax, bool negate = false)
	{
		axis_ = ax;
		is_axis_ = true;
		negate_ = negate;
	}

	void set(SDL_GamepadButton btn, bool negate = false)
	{
		button_ = btn;
		is_axis_ = false;
		negate_ = negate;
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
			else
				deadzone = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

			if (abs(raw) < deadzone)
				raw = 0;

			value = raw / StickRange;
		}

		return negate_ ? -value : value;
	}

	bool isAxis() const override { return is_axis_; }
	bool isNegated() const override { return negate_; }

	std::string getBindingName_Xbox(bool isSteerAction) const
	{
		if (isAxis())
		{
			std::string direction = isSteerAction ? "" : (negate_ ? "+" : "-");
			switch (axis_)
			{
				case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "Left Trigger";
				case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "Right Trigger";
				default: return "Unknown Axis" + direction;
			}
		}

		switch (button_)
		{
			case SDL_GAMEPAD_BUTTON_A: return "A";
			case SDL_GAMEPAD_BUTTON_B: return "B";
			case SDL_GAMEPAD_BUTTON_X: return "X";
			case SDL_GAMEPAD_BUTTON_Y: return "Y";
			case SDL_GAMEPAD_BUTTON_START: return "Start";
			case SDL_GAMEPAD_BUTTON_BACK: return "Back";
			case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LS";
			case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RS";
			case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "Left Bumper";
			case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "Right Bumper";
		}
		return "Unknown Button";
	}

	std::string getBindingName_PS(SDL_GamepadType padType, bool isSteerAction) const
	{
		if (isAxis())
		{
			std::string direction = isSteerAction ? "" : (negate_ ? "+" : "-");
			switch (axis_)
			{
				case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "L2";
				case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "R2";
				default: return "Unknown Axis" + direction;
			}
		}

		switch (button_)
		{
			case SDL_GAMEPAD_BUTTON_A: return "Cross";
			case SDL_GAMEPAD_BUTTON_B: return "Circle";
			case SDL_GAMEPAD_BUTTON_X: return "Square";
			case SDL_GAMEPAD_BUTTON_Y: return "Triangle";
			case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L3";
			case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R3";
			case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "L1";
			case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "L2";
		}

		// PS4/PS5 changed the start/back names, grr...
		if (padType == SDL_GAMEPAD_TYPE_PS3)
		{
			if (button_ == SDL_GAMEPAD_BUTTON_START)
				return "Start";
			else if (button_ == SDL_GAMEPAD_BUTTON_BACK)
				return "Select";
		}
		else if(padType == SDL_GAMEPAD_TYPE_PS4)
		{
			if (button_ == SDL_GAMEPAD_BUTTON_START)
				return "Options";
			else if (button_ == SDL_GAMEPAD_BUTTON_BACK)
				return "Share";
		}
		else if (padType == SDL_GAMEPAD_TYPE_PS5)
		{
			if (button_ == SDL_GAMEPAD_BUTTON_START)
				return "Options";
			else if (button_ == SDL_GAMEPAD_BUTTON_BACK)
				return "Create";
		}
		return "Unknown Button";
	}

	std::string getBindingName_Switch(bool isSteerAction) const
	{
		if (isAxis())
		{
			std::string direction = isSteerAction ? "" : (negate_ ? "+" : "-");
			switch (axis_)
			{
				case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "ZL";
				case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "ZR";
				default: return "Unknown Axis" + direction;
			}
		}

		switch (button_)
		{
			case SDL_GAMEPAD_BUTTON_A: return "B";
			case SDL_GAMEPAD_BUTTON_B: return "A";
			case SDL_GAMEPAD_BUTTON_X: return "Y";
			case SDL_GAMEPAD_BUTTON_Y: return "X";
			case SDL_GAMEPAD_BUTTON_START: return "Plus";
			case SDL_GAMEPAD_BUTTON_BACK: return "Minus";
			case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LS";
			case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RS";
			case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "L";
			case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "R";
		}
		return "Unknown Button";
	}

	std::string getBindingName(SDL_GamepadType padType, bool isSteerAction) const override
	{
		if (isAxis())
		{
			std::string direction = isSteerAction ? "" : (negate_ ? "+" : "-");
			switch (axis_)
			{
				case SDL_GAMEPAD_AXIS_LEFTX: return "Left Stick X" + direction;
				case SDL_GAMEPAD_AXIS_LEFTY: return "Left Stick Y" + direction;
				case SDL_GAMEPAD_AXIS_RIGHTX: return "Right Stick X" + direction;
				case SDL_GAMEPAD_AXIS_RIGHTY: return "Right Stick Y" + direction;
			}
		}
		else // !isAxis()
		{
			switch (button_)
			{
				case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-Pad Up";
				case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-Pad Down";
				case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-Pad Left";
				case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-Pad Right";
			}
		}

		switch (padType)
		{
			case SDL_GAMEPAD_TYPE_PS3:
			case SDL_GAMEPAD_TYPE_PS4:
			case SDL_GAMEPAD_TYPE_PS5:
				return getBindingName_PS(padType, isSteerAction);
			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
				return getBindingName_Switch(isSteerAction);
		}

		return getBindingName_Xbox(isSteerAction);
	}

	std::string getBindingIniDefinition() const override
	{
		if (isAxis())
		{
			switch (axis_)
			{
			case SDL_GAMEPAD_AXIS_LEFTX: return "LS-X";
			case SDL_GAMEPAD_AXIS_LEFTY: return "LS-Y";
			case SDL_GAMEPAD_AXIS_RIGHTX: return "RS-X";
			case SDL_GAMEPAD_AXIS_RIGHTY: return "RS-Y";
			case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LT";
			case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT";
			default: return "";
			}
		}

		switch (button_)
		{
		case SDL_GAMEPAD_BUTTON_A: return "A";
		case SDL_GAMEPAD_BUTTON_B: return "B";
		case SDL_GAMEPAD_BUTTON_X: return "X";
		case SDL_GAMEPAD_BUTTON_Y: return "Y";
		case SDL_GAMEPAD_BUTTON_BACK: return "Back";
		case SDL_GAMEPAD_BUTTON_START: return "Start";
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return "DPad-Up";
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "DPad-Down";
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "DPad-Left";
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "DPad-Right";
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
		default: return "";
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
	KeyboardSource(SDL_Scancode k, bool negate = false) : key_(k), negate_(negate)
	{
		type_ = InputSourceType::Keyboard;
	}

	void set(SDL_Scancode k, bool negate = false)
	{
		key_ = k;
		negate_ = negate;
	}

	float readValue(SDL_Gamepad* gamepad) override
	{
		const bool* state_array = SDL_GetKeyboardState(nullptr);
		return negate_ ? -float(state_array[key_]) : state_array[key_];
	}

	bool isAxis() const override { return false; }
	bool isNegated() const override { return negate_; }
	std::string getBindingName(SDL_GamepadType padType, bool isSteerAction) const override
	{
		return SDL_GetScancodeName(key_);
	}
	std::string getBindingIniDefinition() const override
	{
		return SDL_GetScancodeName(key_);
	}
	SDL_Scancode key() const { return key_; }
};

class InputAction
{
	std::vector<std::unique_ptr<InputSource>> sources_;
	InputState state_;

public:
	const InputState& update(SDL_Gamepad* primary_pad)
	{
		float maxValue = 0.0f;
		bool isAxisInput = false;
		InputSourceType lastSource = state_.lastSourceType;

		// Read all sources and take the highest absolute value
		for (auto& source : sources_)
		{
			float currentValue = source->readValue(primary_pad);
			if (std::abs(currentValue) > std::abs(maxValue))
			{
				maxValue = currentValue;
				isAxisInput = source->isAxis();
				lastSource = source->type();
			}
		}

		state_.lastSourceType = lastSource;
		state_.isAxis = isAxisInput;
		state_.update(maxValue);
		return state_;
	}

	template <typename T, typename... Args>
	void addSource(Args&&... args)
	{
		sources_.push_back(std::make_unique<T>(std::forward<Args>(args)...));
	}

	void clear()
	{
		sources_.clear();
	}

	std::vector<std::unique_ptr<InputSource>>& sources() { return sources_; }

	const InputState& getState() const { return state_; }
	void setState(const InputState& state) { this->state_ = state; }
};

class InputManager
{
	std::array<InputAction, size_t(ADChannel::Count)> volumeBindings;
	std::array<InputAction, size_t(SwitchId::Count)> switchBindings;

	std::mutex mtx;
	std::vector<SDL_Gamepad*> controllers;
	int primaryControllerIndex = -1;

	SDL_Window* window = nullptr;

	// cached values as of last update call
	std::array<InputState, size_t(ADChannel::Count)> volumes;
	uint32_t switch_current;
	uint32_t switch_previous;
	uint32_t switch_overlay;
	InputSourceType lastInputSource_ = InputSourceType::GamePad;

private:
	// user settings
	bool BypassGameSensitivity = false;

	static inline const std::string volumeNames[] = {
		"Steering",
		"Acceleration",
		"Brake"
	};

	static inline const std::string switchNames[] = {
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

	static int PadButtonFromName(const char* name)
	{
		if (!stricmp(name, "LS-X")) return SDL_GAMEPAD_AXIS_LEFTX;
		if (!stricmp(name, "LS-Y")) return SDL_GAMEPAD_AXIS_LEFTY;
		if (!stricmp(name, "RS-X")) return SDL_GAMEPAD_AXIS_RIGHTX;
		if (!stricmp(name, "RS-Y")) return SDL_GAMEPAD_AXIS_RIGHTY;
		if (!stricmp(name, "LT")) return SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
		if (!stricmp(name, "RT")) return SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;

		if (!stricmp(name, "A")) return SDL_GAMEPAD_BUTTON_A;
		if (!stricmp(name, "B")) return SDL_GAMEPAD_BUTTON_B;
		if (!stricmp(name, "X")) return SDL_GAMEPAD_BUTTON_X;
		if (!stricmp(name, "Y")) return SDL_GAMEPAD_BUTTON_Y;
		if (!stricmp(name, "Back")) return SDL_GAMEPAD_BUTTON_BACK;
		if (!stricmp(name, "Start")) return SDL_GAMEPAD_BUTTON_START;
		if (!stricmp(name, "DPad-Up")) return SDL_GAMEPAD_BUTTON_DPAD_UP;
		if (!stricmp(name, "DPad-Down")) return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
		if (!stricmp(name, "DPad-Left")) return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
		if (!stricmp(name, "DPad-Right")) return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
		if (!stricmp(name, "LB")) return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
		if (!stricmp(name, "RB")) return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;

		return SDL_GAMEPAD_BUTTON_INVALID;
	}

	// Helper function to check if a button name is actually an axis
	static bool IsAxisName(const char* name)
	{
		if (!stricmp(name, "LS-X")) return true;
		if (!stricmp(name, "LS-Y")) return true;
		if (!stricmp(name, "RS-X")) return true;
		if (!stricmp(name, "RS-Y")) return true;
		if (!stricmp(name, "LT")) return true;
		if (!stricmp(name, "RT")) return true;

		return false;
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

	void setupGamepad(SDL_Gamepad* controller)
	{
		Game::CurrentPadType = Game::GamepadType::Xbox;
		auto type = SDL_GetGamepadType(controller);
		switch (type)
		{
		case SDL_GAMEPAD_TYPE_PS3:
		case SDL_GAMEPAD_TYPE_PS4:
		case SDL_GAMEPAD_TYPE_PS5:
			Game::CurrentPadType = Game::GamepadType::PS;
			break;
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
			Game::CurrentPadType = Game::GamepadType::Switch;
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

		// If we don't have primary already, set it as this
		if (primaryControllerIndex == -1)
			setPrimaryGamepad(controllers.size() - 1);
	}

	void onControllerRemoved(SDL_JoystickID instanceId)
	{
		spdlog::debug(__FUNCTION__ "(instance {})", instanceId);

		std::lock_guard<std::mutex> lock(mtx);

		auto it = std::find_if(controllers.begin(), controllers.end(), [instanceId](SDL_Gamepad* controller)
			{
				return SDL_GetGamepadID(controller) == instanceId;
			});

		if (it != controllers.end())
		{
			Game::CurrentPadType = Game::GamepadType::PC;

			SDL_CloseGamepad(*it);
			controllers.erase(it);

			spdlog::debug(__FUNCTION__ "(instance {}): removed instance", instanceId);

			if (primaryControllerIndex >= controllers.size())
				setPrimaryGamepad(controllers.empty() ? -1 : 0);
		}
	}

public:
	~InputManager()
	{
		for (auto controller : controllers)
			SDL_CloseGamepad(controller);
	}

	SDL_Gamepad* getPrimaryGamepad()
	{
		if (primaryControllerIndex < 0)
			return nullptr;
		if (primaryControllerIndex >= controllers.size())
			return nullptr;
		return controllers[primaryControllerIndex];
	}

	void setPrimaryGamepad(int index)
	{
		if (index < 0 || index >= controllers.size())
			primaryControllerIndex = -1;
		else
			primaryControllerIndex = index;

		spdlog::debug("InputManager::primaryControllerIndex = {}", primaryControllerIndex);

		if (auto* pad = getPrimaryGamepad())
			setupGamepad(pad);
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

		if (!readBindingIni(Module::BindingsIniPath))
			setupDefaultBindings();
	}

	void setupDefaultBindings()
	{
		// Remove any previous bindings
		for (auto& binding : volumeBindings)
			binding.clear();
		for (auto& binding : switchBindings)
			binding.clear();

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

	bool readBindingIni(const std::filesystem::path& iniPath)
	{
		if (!std::filesystem::exists(iniPath))
			return false;

		spdlog::info(__FUNCTION__ " - reading INI from {}", iniPath.string());

		std::vector<std::pair<std::string, std::string>> pad_binds;
		std::vector<std::pair<std::string, std::string>> key_binds;

		std::ifstream file(iniPath);
		if (!file || !file.is_open())
		{
			spdlog::error(__FUNCTION__ " - failed to read INI, using defaults");
			return false;
		}

		std::string line;
		bool inBindingSection = false;
		bool inKeyboardSection = false;

		while (std::getline(file, line))
		{
			if (inBindingSection)
			{
				if (line.empty())
					continue;
				line = Util::trim(line);
				if (line.front() != '[' && line.back() != ']') // Haven't reached a new section
				{
					if (line.front() == '#' || line.front() == ';')
						continue;

					auto delimiterPos = line.find('=');
					if (delimiterPos != std::string::npos)
					{
						std::string action = Util::trim(line.substr(0, delimiterPos));
						std::string bind = Util::trim(line.substr(delimiterPos + 1));

						if (inKeyboardSection)
							key_binds.emplace_back(action, bind);
						else
							pad_binds.emplace_back(action, bind);
					}
					continue;
				}
			}
			if (line == "[Gamepad]")
			{
				inBindingSection = true;
				inKeyboardSection = false;
			}
			else if (line == "[Keyboard]")
			{
				inBindingSection = true;
				inKeyboardSection = true;
			}
			else
			{
				// unknown section
				inBindingSection = false;
				inKeyboardSection = false;
			}
		}

		file.close();

		if (key_binds.size() <= 0 && pad_binds.size() <= 0)
		{
			spdlog::error(__FUNCTION__ " - failed to read binds from INI, using defaults");
			return false;
		}

		spdlog::info(__FUNCTION__ " - {} key binds, {} pad binds", key_binds.size(), pad_binds.size());

		// we have binds, reset any of our defaults

		for (auto& binding : volumeBindings)
			binding.clear();
		for (auto& binding : switchBindings)
			binding.clear();

		for (auto& [action, binding] : key_binds)
		{
			auto actionName = action;
			bool negate = false;
			if (actionName.substr(actionName.length() - 1, 1) == "-")
			{
				negate = true;
				actionName = actionName.substr(0, actionName.length() - 1);
			}

			SDL_Scancode scancode = SDL_GetScancodeFromName(binding.c_str());
			if (scancode == SDL_SCANCODE_UNKNOWN)
			{
				spdlog::error(__FUNCTION__ ": failed to parse binding scancode for {} = {}", action, binding);
				continue;
			}

			ADChannel volumeId = ADChannel::Count;
			SwitchId switchId = SwitchId::Count;

			// is this a volume?
			for (int i = 0; i < 3; i++)
			{
				if (!stricmp(actionName.c_str(), volumeNames[i].c_str()))
				{
					volumeId = ADChannel(i);
					break;
				}
			}
			if (volumeId == ADChannel::Count)
			{
				// is this a switch?
				for (int i = 0; i < int(SwitchId::Count); i++)
				{
					if (!stricmp(actionName.c_str(), switchNames[i].c_str()))
					{
						switchId = SwitchId(i);
						break;
					}
				}
			}

			if (volumeId != ADChannel::Count)
				addVolumeBinding<KeyboardSource>(volumeId, scancode, negate);
			else if (switchId != SwitchId::Count)
				addSwitchBinding<KeyboardSource>(switchId, scancode, negate);
			else
			{
				spdlog::error(__FUNCTION__ ": failed to parse binding action for {} = {}", action, binding);
				continue;
			}
		}

		for (auto& [action, binding] : pad_binds)
		{
			auto actionName = action;
			bool negate = false;
			if (actionName.substr(actionName.length() - 1, 1) == "-")
			{
				negate = true;
				actionName = actionName.substr(0, actionName.length() - 1);
			}

			int buttonOrAxis = PadButtonFromName(binding.c_str());
			if (buttonOrAxis == SDL_GAMEPAD_BUTTON_INVALID)
			{
				spdlog::error(__FUNCTION__ ": failed to parse binding scancode for {} = {}", action, binding);
				continue;
			}

			ADChannel volumeId = ADChannel::Count;
			SwitchId switchId = SwitchId::Count;

			// Check if it's a volume action
			for (int i = 0; i < 3; i++)
			{
				if (!stricmp(actionName.c_str(), volumeNames[i].c_str()))
				{
					volumeId = ADChannel(i);
					break;
				}
			}
			if (volumeId == ADChannel::Count)
			{
				// Check if it's a switch action
				for (int i = 0; i < int(SwitchId::Count); i++)
				{
					if (!stricmp(actionName.c_str(), switchNames[i].c_str()))
					{
						switchId = SwitchId(i);
						break;
					}
				}
			}

			if (volumeId != ADChannel::Count)
			{
				if (IsAxisName(binding.c_str()))
					addVolumeBinding<GamepadSource>(volumeId, static_cast<SDL_GamepadAxis>(buttonOrAxis), negate);
				else
					addVolumeBinding<GamepadSource>(volumeId, static_cast<SDL_GamepadButton>(buttonOrAxis), negate);
			}
			else if (switchId != SwitchId::Count)
			{
				if (IsAxisName(binding.c_str()))
					addSwitchBinding<GamepadSource>(switchId, static_cast<SDL_GamepadAxis>(buttonOrAxis), negate);
				else
					addSwitchBinding<GamepadSource>(switchId, static_cast<SDL_GamepadButton>(buttonOrAxis), negate);
			}
			else
			{
				spdlog::error(__FUNCTION__ ": failed to parse binding action for {} = {}", action, binding);
				continue;
			}
		}

		return true;
	}

	bool saveBindingIni(const std::filesystem::path& iniPath)
	{
		std::ofstream file(iniPath, std::ios::out | std::ios::trunc);
		if (!file.is_open())
		{
			spdlog::error(__FUNCTION__ ": failed to open file for writing: {}", iniPath.string());
			return false;
		}

		file << "# These bindings are used when UseNewInput is enabled inside OutRun2006Tweaks.ini\n";
		file << "# With that enabled, you can use in-game Controls > Configuration dialog to change these during gameplay\n";
		file << "# (editing this file manually can allow more advanced config, such as binding multiple inputs to a single action)\n";
		file << "# If this file doesn't exist or is empty, bindings will be reset to default.\n";
		file << "#\n";
		file << "# Actions with a negative symbol after them ('Steering-') either treat the input as a negative value, or only trigger the action on negative inputs\n";
		file << "# Analog actions bound to digital inputs, eg. 'Steering- = DPad-Left', will make DPad-Left send a negative Steering value, making it move to the left\n";
		file << "# Digital actions bound to analog inputs, eg. 'Gear Down- = RS-Y', will only trigger the action when RS-Y is negative\n";
		file << "# Analog -> analog actions can also be inverted by adding a negative to them\n\n";
		file << "[Gamepad]\n";
		for (int i = 0; i < 3; ++i)
		{
			auto& binding = volumeBindings[i];
			for (const auto& source : binding.sources())
				if (auto* gamepadSource = dynamic_cast<const GamepadSource*>(source.get()))
				{
					std::string direction = gamepadSource->isNegated() ? "-" : "";
					file << volumeNames[i] << direction << " = " << gamepadSource->getBindingIniDefinition() << "\n";
				}
		}

		for (int i = 0; i < int(SwitchId::Count); ++i)
		{
			auto& binding = switchBindings[i];
			for (const auto& source : binding.sources())
				if (auto* gamepadSource = dynamic_cast<const GamepadSource*>(source.get()))
				{
					std::string direction = gamepadSource->isNegated() ? "-" : "";
					file << switchNames[i] << direction << " = " << gamepadSource->getBindingIniDefinition() << "\n";
				}
		}

		file << "\n[Keyboard]\n";
		for (int i = 0; i < 3; ++i)
		{
			auto& binding = volumeBindings[i];
			for (const auto& source : binding.sources())
				if (auto* keyboardSource = dynamic_cast<const KeyboardSource*>(source.get()))
				{
					std::string direction = keyboardSource->isNegated() ? "-" : "";
					file << volumeNames[i] << direction << " = " << keyboardSource->getBindingIniDefinition() << "\n";
				}
		}

		for (int i = 0; i < int(SwitchId::Count); ++i)
		{
			auto& binding = switchBindings[i];
			for (const auto& source : binding.sources())
				if (auto* keyboardSource = dynamic_cast<const KeyboardSource*>(source.get()))
				{
					std::string direction = keyboardSource->isNegated() ? "-" : "";
					file << switchNames[i] << direction << " = " << keyboardSource->getBindingIniDefinition() << "\n";
				}
		}

		file.close();
		spdlog::info(__FUNCTION__": saved to INI file: {}", iniPath.string());

		return true;
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
			case SDL_EVENT_QUIT:
				PostQuitMessage(0);
				break;
			}

		auto* gamepad = getPrimaryGamepad();

		// update & cache our bindings
		switch_previous = switch_current;
		switch_current = 0;

		// hacks to hide inputs until all buttons are released
		static bool disableOverlayInputs = false;
		static bool disableGameInputs = false;

		if (isListeningForInput == ListenState::Listening)
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
		{
			auto& switchState = switchBindings[i].update(gamepad);
			if (switchState.isPressed())
			{
				switch_current |= (1 << i);
				lastInputSource_ = switchState.lastSourceType;
			}
		}

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

	void setVibration(WORD left, WORD right)
	{
		auto* controller = getPrimaryGamepad();
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

	bool anyInputPressed()
	{
		int key_count = 0;
		const bool* key_state = SDL_GetKeyboardState(&key_count);
		for (int i = 0; i < key_count; i++)
			if (key_state[i])
				return true;

		auto* controller = getPrimaryGamepad();
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

	InputSourceType lastInputSource() { return lastInputSource_; }

	//
	// Handlers for games original input functions
	//
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

	friend class InputBindingsUI;

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
	InputManager::instance.setVibration(left, right);
}

class InputBindingsUI : public OverlayWindow
{
	static constexpr float BindScreenTimeout = 5.f;

	std::string currentlyBinding;
	ADChannel currentVolumeChannel;
	SwitchId currentSwitchId;
	bool currentIsNegate = false;
	bool isBindingVolume = false;
	bool isBindingKeyboard = false;
	float bindScreenDisplayTime = 0.f;
	InputSource* currentSourceReplacing = nullptr;

public:
	void init() override {}

	bool HandleNewBinding()
	{
		auto& manager = InputManager::instance;

		auto* controller = manager.getPrimaryGamepad();

		InputAction* action = nullptr;
		if (isBindingVolume)
			action = &manager.volumeBindings[static_cast<int>(currentVolumeChannel)];
		else
			action = &manager.switchBindings[static_cast<int>(currentSwitchId)];

		// Handle keyboard binding
		if (isBindingKeyboard)
		{
			// Update timeout
			bindScreenDisplayTime -= ImGui::GetIO().DeltaTime;
			if (bindScreenDisplayTime <= 0.0f)
			{
				isListeningForInput = ListenState::False;
				ImGui::CloseCurrentPopup();
				return false;
			}

			// Check all possible keys
			int key_count = 0;
			const bool* key_state = SDL_GetKeyboardState(&key_count);
			for (int i = 0; i < key_count; i++)
			{
				if (key_state[i])
				{
					SDL_Scancode key = static_cast<SDL_Scancode>(i);

					if (currentSourceReplacing != nullptr)
					{
						auto* kbd_source = dynamic_cast<KeyboardSource*>(currentSourceReplacing);
						kbd_source->set(key, kbd_source->isNegated());
					}
					else
					{
						// remove all existing keyboard bindings

						action->sources().erase(
							std::remove_if(
								action->sources().begin(),
								action->sources().end(),
								[this](const std::unique_ptr<InputSource>& source)
								{
									return dynamic_cast<const KeyboardSource*>(source.get()) != nullptr && source->isNegated() == currentIsNegate;
								}
							),
							action->sources().end()
						);

						action->addSource<KeyboardSource>(key, currentIsNegate);
					}

					isListeningForInput = ListenState::WaitForBindButtonRelease;
					ImGui::CloseCurrentPopup();
					return true;
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
					if (currentSourceReplacing != nullptr)
					{
						auto* pad_source = dynamic_cast<GamepadSource*>(currentSourceReplacing);
						pad_source->set(static_cast<SDL_GamepadButton>(i), pad_source->isNegated());
					}
					else
					{
						// remove all existing bindings
						action->sources().erase(
							std::remove_if(
								action->sources().begin(),
								action->sources().end(),
								[this](const std::unique_ptr<InputSource>& source)
								{
									return dynamic_cast<const GamepadSource*>(source.get()) != nullptr && source->isNegated() == currentIsNegate;
								}
							),
							action->sources().end()
						);

						action->addSource<GamepadSource>(static_cast<SDL_GamepadButton>(i), currentIsNegate);
					}

					isListeningForInput = ListenState::WaitForBindButtonRelease;
					ImGui::CloseCurrentPopup();
					return true;
				}
			}

			// Check all possible axes
			for (int i = SDL_GAMEPAD_AXIS_LEFTX; i < SDL_GAMEPAD_AXIS_COUNT; i++)
			{
				float value = SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(i)) / 32768.0f;
				bool negate = value < 0;
				if (isBindingVolume && currentVolumeChannel == ADChannel::Steering)
					negate = false;

				if (std::abs(value) > 0.5f)
				{
					if (currentSourceReplacing != nullptr)
					{
						auto* pad_source = dynamic_cast<GamepadSource*>(currentSourceReplacing);
						pad_source->set(static_cast<SDL_GamepadAxis>(i), pad_source->isNegated());
					}
					else
					{
						action->sources().erase(
							std::remove_if(
								action->sources().begin(),
								action->sources().end(),
								[](const std::unique_ptr<InputSource>& source)
								{
									return dynamic_cast<const GamepadSource*>(source.get()) != nullptr;
								}
							),
							action->sources().end()
						);
						action->addSource<GamepadSource>(static_cast<SDL_GamepadAxis>(i), negate);
					}

					isListeningForInput = ListenState::WaitForBindButtonRelease;
					ImGui::CloseCurrentPopup();
					return true;
				}
			}
		}
		return false;
	}

	void showPromptVolume(ADChannel channel, bool isNegate, bool isKeyboard, InputSource* replacing)
	{
		isListeningForInput = ListenState::WaitForButtonRelease;
		isBindingVolume = true;
		currentVolumeChannel = channel;
		currentIsNegate = isNegate;
		isBindingKeyboard = isKeyboard;
		currentlyBinding = InputManager::volumeNames[int(channel)];
		currentSourceReplacing = replacing;
		bindScreenDisplayTime = BindScreenTimeout;
	}

	void showPromptSwitch(SwitchId switchId, bool isNegate, bool isKeyboard, InputSource* replacing)
	{
		isListeningForInput = ListenState::WaitForButtonRelease;
		isBindingVolume = false;
		currentSwitchId = switchId;
		currentIsNegate = isNegate;
		isBindingKeyboard = isKeyboard;
		currentlyBinding = InputManager::switchNames[int(switchId)];
		currentSourceReplacing = replacing;
		bindScreenDisplayTime = BindScreenTimeout;
	}

	void render(bool overlayEnabled) override
	{
		if (!Overlay::IsBindingDialogActive)
			return;

		auto& manager = InputManager::instance;

		auto padType = SDL_GAMEPAD_TYPE_XBOX360;
		if (auto* primary = manager.getPrimaryGamepad())
			padType = SDL_GetGamepadType(primary);

		bool dialogOpen = true;

		static bool unsavedChanges = false;

		ImGui::OpenPopup("Input Bindings");
		if (ImGui::BeginPopupModal("Input Bindings", &dialogOpen, ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_AlwaysAutoResize))
		{
			if (ImGui::Button(unsavedChanges ? "Save bindings*##save" : "Save bindings##save"))
				if (manager.saveBindingIni(Module::BindingsIniPath))
					unsavedChanges = false;

			ImGui::SameLine();

			if (ImGui::Button("Load bindings"))
				if (manager.readBindingIni(Module::BindingsIniPath))
					unsavedChanges = false;

			if (ImGui::BeginTable("Controllers", 2, ImGuiTableFlags_Borders))
			{
				ImGui::TableSetupColumn("Detected Controllers");
				ImGui::TableSetupColumn("Type");
				ImGui::TableHeadersRow();

				if (manager.controllers.empty())
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Text("No controllers detected");
					ImGui::TableNextColumn();
				}
				else
				{
					for (size_t i = 0; i < manager.controllers.size(); i++)
					{
						ImGui::TableNextRow();
						ImGui::TableNextColumn();

						auto* controller = manager.controllers[i];
						std::string name = SDL_GetGamepadName(controller);

						if (ImGui::Button(name.c_str()))
							manager.setPrimaryGamepad(i);

						ImGui::TableNextColumn();

						if (int(i) == manager.primaryControllerIndex)
							ImGui::Text("Active/Primary");
						else
							ImGui::Text("Inactive");
					}
				}
				ImGui::EndTable();
			}

			ImGui::Separator();

			if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (ImGui::BeginTable("VolumeBindings", 3, ImGuiTableFlags_Borders))
				{
					ImGui::TableSetupColumn("Action");
					ImGui::TableSetupColumn("Controller Binding");
					ImGui::TableSetupColumn("Keyboard Binding");
					ImGui::TableHeadersRow();

					for (int i = 0; i < 3; i++)
					{
						ADChannel volChannel = ADChannel(i);
						auto& volAction = manager.volumeBindings[i];

						ImGui::TableNextRow();

						// Action name
						ImGui::TableNextColumn();
						ImGui::Text("%s", manager.volumeNames[i].c_str());

						// Controller binding
						{
							ImGui::TableNextColumn();

							// Show all bindings for this action
							int sourceIdx = 0;
							bool hasBinds = false;
							for (const auto& source : volAction.sources())
							{
								if (dynamic_cast<const GamepadSource*>(source.get()))
								{
									hasBinds = true;

									if (ImGui::Button(std::format("{}##vol_ctrl_{}_{}", source->getBindingName(padType, i == 0), i, sourceIdx).c_str()))
										showPromptVolume(ADChannel(i), source->isNegated(), false, source.get());
								}
								sourceIdx++;
							}

							// If no binds were displayed, show a single None button to allow user to bind it
							if (!hasBinds)
							{
								if (ImGui::Button(std::format("None##vol_ctrl_{}", i).c_str()))
									showPromptVolume(ADChannel(i), false, false, nullptr);
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
											kbd_negative = source->getBindingName(padType, true);
										else
											kbd_positive = source->getBindingName(padType, true);
									}
								}
							}
							if (ImGui::Button(std::format("{}##vol_kb_neg_{}", kbd_negative, i).c_str()))
								showPromptVolume(ADChannel(i), volChannel == ADChannel::Steering ? true : false, true, nullptr);

							if (volChannel == ADChannel::Steering)
							{
								ImGui::SameLine();
								if (ImGui::Button(std::format("{}##vol_kb_pos_{}", kbd_positive, i).c_str()))
									showPromptVolume(ADChannel(i), false, true, nullptr);
							}
						}
					}

					for (int i = 0; i < int(SwitchId::Count); i++)
					{
						SwitchId switchId = SwitchId(i);

						// TODO: scan through games SwitchOn/SwitchNow calls and see if these are ever used
						if (switchId == SwitchId::Unknown0x100 || switchId == SwitchId::Unknown0x200 ||
							switchId == SwitchId::Unknown0x10000 || switchId == SwitchId::Unknown0x20000)
						{
							continue;
						}

						auto& switchAction = manager.switchBindings[i];

						ImGui::TableNextRow();

						// Action name
						ImGui::TableNextColumn();
						ImGui::Text("%s", manager.switchNames[i].c_str());

						// Controller binding
						{
							ImGui::TableNextColumn();

							// Show all bindings for this action
							int sourceIdx = 0;
							bool hasBinds = false;
							for (const auto& source : switchAction.sources())
							{
								if (dynamic_cast<const GamepadSource*>(source.get()))
								{
									hasBinds = true;

									if (ImGui::Button(std::format("{}##switch_ctrl_{}_{}", source->getBindingName(), i, sourceIdx).c_str()))
										showPromptSwitch(SwitchId(i), source->isNegated(), false, source.get());
								}
								sourceIdx++;
							}

							if(!hasBinds)
								if (ImGui::Button(std::format("None##switch_ctrl_{}", i).c_str()))
									showPromptSwitch(SwitchId(i), false, false, nullptr);
						}

						// Keyboard binding
						{
							ImGui::TableNextColumn();
							std::string keyboardBind = "None";
							for (const auto& source : switchAction.sources())
							{
								if (dynamic_cast<const KeyboardSource*>(source.get()))
								{
									keyboardBind = source->getBindingName(padType, false);
									break;
								}
							}
							if (ImGui::Button(std::format("{}##switch_kb_{}", keyboardBind, i).c_str()))
								showPromptSwitch(SwitchId(i), false, true, nullptr);
						}
					}
					ImGui::EndTable();
				}
			}

			const char* vibrationModes[] = { "Disabled", "Enabled", "Swap L/R", "Merge L/R" };
			ImGui::Combo("Vibration Mode", &Settings::VibrationMode, vibrationModes, IM_ARRAYSIZE(vibrationModes));
			ImGui::SliderInt("Vibration Strength", &Settings::VibrationStrength, 0, 10);
			ImGui::Combo("Impulse Vibration", &Settings::ImpulseVibrationMode, vibrationModes, IM_ARRAYSIZE(vibrationModes));

			int deadzonePercent = Settings::SteeringDeadZone * 100.f;
			if (ImGui::SliderInt("Steering Deadzone", &deadzonePercent, 5, 20, "%d%%"))
				Settings::SteeringDeadZone = float(deadzonePercent) / 100.f;

			ImGui::Checkbox("Bypass Sensitivity", &manager.BypassGameSensitivity);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Passes steering input to the game directly, allows for more sensitive controls");

			if (unsavedChanges)
				ImGui::Text("Note: you have unsaved changes!");

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
					unsavedChanges = true;
					Settings::SteeringDeadZone = 0.2f;
					manager.BypassGameSensitivity = false;
					manager.setupDefaultBindings();
					if (auto* controller = manager.getPrimaryGamepad())
						manager.setupGamepad(controller);
					showReallyPrompt = false;
				}
			}

			// Input listening overlay
			if (isListeningForInput == ListenState::False)
			{
				// Close bind dialog if user pressed back/B
				if ((manager.switch_overlay & (1 << int(SwitchId::Back) | 1 << int(SwitchId::B))) != 0)
					dialogOpen = false;
			}
			else
			{
				if (isListeningForInput == ListenState::WaitForButtonRelease)
				{
					// wait for user to release all buttons before we start listening
					if (!manager.anyInputPressed())
					{
						isListeningForInput = ListenState::Listening;
					}
				}
				else if (isListeningForInput == ListenState::WaitForBindButtonRelease)
				{
					if (!manager.anyInputPressed())
					{
						isListeningForInput = ListenState::False;
					}
				}
				else if (isListeningForInput == ListenState::Listening)
				{
					ImGui::OpenPopup("Listening for Input");

					if (ImGui::BeginPopupModal("Listening for Input", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
					{
						ImGui::Text("Press %s input to bind to %s",
							isBindingKeyboard ? "keyboard" : "controller",
							currentlyBinding.c_str());

						if (isBindingKeyboard)
							ImGui::Text("Aborting in %.1f seconds", bindScreenDisplayTime);
						else
							ImGui::Text("Press ESC to abort");

						// Check for binding presses
						if (HandleNewBinding())
							unsavedChanges = true;

						if (!isBindingKeyboard && ImGui::IsKeyPressed(ImGuiKey_Escape))
						{
							isListeningForInput = ListenState::False;
							ImGui::CloseCurrentPopup();
						}

						ImGui::EndPopup();
					}
				}
			}
			ImGui::EndPopup();
		}

		if (!dialogOpen)
		{
			Overlay::IsBindingDialogActive = false;
			Overlay::RequestMouseHide = true;
		}
	}
	static InputBindingsUI instance;
};
InputBindingsUI InputBindingsUI::instance;

constexpr uint32_t StartSwitchMask = 1 << int(SwitchId::Start);

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
