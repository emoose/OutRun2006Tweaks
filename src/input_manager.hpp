#pragma once

#include <SDL3/SDL.h>
#include <unordered_map>
#include <vector>
#include <memory>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "overlay/overlay.hpp"

#include <array>
#include <optional>
#include <istream>
#include "input_names.hpp"

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

inline ListenState isListeningForInput = ListenState::False;

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

//
// A single bound input: one gamepad button, one gamepad axis, or one key.
//
struct InputBinding
{
	enum class Kind : uint8_t { None, PadButton, PadAxis, Key };

	static constexpr float StickRange = 32768.f;
	static constexpr float RStickDeadzone = 0.7f; // needs a high deadzone or flicks bounce

	Kind kind = Kind::None;
	bool negate = false;
	union
	{
		SDL_GamepadButton button;
		SDL_GamepadAxis axis;
		SDL_Scancode key;
	};

	InputBinding() : button(SDL_GAMEPAD_BUTTON_INVALID) {}
	InputBinding(SDL_GamepadButton b, bool n = false) : kind(Kind::PadButton), negate(n), button(b) {}
	InputBinding(SDL_GamepadAxis a, bool n = false) : kind(Kind::PadAxis), negate(n), axis(a) {}
	InputBinding(SDL_Scancode k, bool n = false) : kind(Kind::Key), negate(n), key(k) {}

	bool isAxis() const { return kind == Kind::PadAxis; }
	bool isKeyboard() const { return kind == Kind::Key; }
	bool isGamepad() const { return kind == Kind::PadButton || kind == Kind::PadAxis; }
	bool isNegated() const { return negate; }

	InputSourceType sourceType() const
	{
		return isKeyboard() ? InputSourceType::Keyboard : InputSourceType::GamePad;
	}

	float read(SDL_Gamepad* gamepad) const
	{
		float value = 0.0f;

		switch (kind)
		{
		case Kind::Key:
		{
			const bool* state_array = SDL_GetKeyboardState(nullptr);
			value = float(state_array[key]);
			break;
		}
		case Kind::PadButton:
			if (!gamepad)
				return 0.0f;
			value = float(SDL_GetGamepadButton(gamepad, button));
			break;
		case Kind::PadAxis:
		{
			if (!gamepad)
				return 0.0f;

			Sint16 raw = SDL_GetGamepadAxis(gamepad, axis);

			int deadzone = 0;
			if (axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY)
				deadzone = int(StickRange * Settings::SteeringDeadZone);
			else if (axis == SDL_GAMEPAD_AXIS_RIGHTX || axis == SDL_GAMEPAD_AXIS_RIGHTY)
				deadzone = int(StickRange * RStickDeadzone);
			else
				deadzone = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

			if (abs(raw) < deadzone)
				raw = 0;

			value = raw / StickRange;
			break;
		}
		default:
			return 0.0f;
		}

		return negate ? -value : value;
	}

	std::string displayName(SDL_GamepadType padType = SDL_GAMEPAD_TYPE_UNKNOWN, bool isSteerAction = false) const
	{
		switch (kind)
		{
		case Kind::Key:       return SDL_GetScancodeName(key);
		case Kind::PadAxis:   return InputNames::displayNameForAxis(axis, padType, negate, isSteerAction);
		case Kind::PadButton: return InputNames::displayNameForButton(button, padType);
		default:              return "";
		}
	}

	std::string iniName() const
	{
		switch (kind)
		{
		case Kind::Key:       return SDL_GetScancodeName(key);
		case Kind::PadAxis:   return InputNames::iniNameForAxis(axis);
		case Kind::PadButton: return InputNames::iniNameForButton(button);
		default:              return "";
		}
	}
};

class InputAction
{
	std::vector<InputBinding> bindings_;
	InputState state_;

public:
	const InputState& update(SDL_Gamepad* primary_pad)
	{
		float maxValue = 0.0f;
		bool isAxisInput = false;
		InputSourceType lastSource = state_.lastSourceType;

		// Read all bindings and take the highest absolute value
		for (const auto& binding : bindings_)
		{
			float currentValue = binding.read(primary_pad);
			if (std::abs(currentValue) > std::abs(maxValue))
			{
				maxValue = currentValue;
				isAxisInput = binding.isAxis();
				lastSource = binding.sourceType();
			}
		}

		state_.lastSourceType = lastSource;
		state_.isAxis = isAxisInput;
		state_.update(maxValue);
		return state_;
	}

	void add(const InputBinding& binding) { bindings_.push_back(binding); }

	void clear() { bindings_.clear(); }

	std::vector<InputBinding>& bindings() { return bindings_; }
	const std::vector<InputBinding>& bindings() const { return bindings_; }

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

	// Switch bitmasks. switch_current/_previous are what the game sees;
	// switch_overlay is the same data before game-side suppression, so the
	// overlay can still be driven while the game is deaf.
	uint32_t switch_current;
	uint32_t switch_previous;
	uint32_t switch_overlay;

	// Latched until the user releases everything - see update().
	// Previously function-local statics inside update().
	bool suppressOverlayUntilRelease = false;
	bool suppressGameUntilRelease = false;
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
		addVolumeBinding(ADChannel::Steering, SDL_SCANCODE_LEFT, true);
		addVolumeBinding(ADChannel::Steering, SDL_SCANCODE_RIGHT);
		addVolumeBinding(ADChannel::Acceleration, SDL_SCANCODE_UP);
		addVolumeBinding(ADChannel::Brake, SDL_SCANCODE_DOWN);

		addSwitchBinding(SwitchId::GearUp, SDL_SCANCODE_W);
		addSwitchBinding(SwitchId::GearDown, SDL_SCANCODE_D);

		addSwitchBinding(SwitchId::Start, SDL_SCANCODE_ESCAPE);
		addSwitchBinding(SwitchId::Back, SDL_SCANCODE_ESCAPE);
		addSwitchBinding(SwitchId::A, SDL_SCANCODE_RETURN);
		addSwitchBinding(SwitchId::B, SDL_SCANCODE_ESCAPE);
		addSwitchBinding(SwitchId::X, SDL_SCANCODE_E);
		addSwitchBinding(SwitchId::Y, SDL_SCANCODE_F);

		addSwitchBinding(SwitchId::ChangeView, SDL_SCANCODE_F);

		addSwitchBinding(SwitchId::SelectionUp, SDL_SCANCODE_UP);
		addSwitchBinding(SwitchId::SelectionDown, SDL_SCANCODE_DOWN);
		addSwitchBinding(SwitchId::SelectionLeft, SDL_SCANCODE_LEFT);
		addSwitchBinding(SwitchId::SelectionRight, SDL_SCANCODE_RIGHT);

		addSwitchBinding(SwitchId::SignIn, SDL_SCANCODE_F1);
		addSwitchBinding(SwitchId::License, SDL_SCANCODE_F2);
		addSwitchBinding(SwitchId::X, SDL_SCANCODE_F1);
		addSwitchBinding(SwitchId::Y, SDL_SCANCODE_F2);

		// Gamepad
		addVolumeBinding(ADChannel::Steering, SDL_GAMEPAD_AXIS_LEFTX);
		addVolumeBinding(ADChannel::Steering, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
		addVolumeBinding(ADChannel::Steering, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
		addVolumeBinding(ADChannel::Acceleration, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
		addVolumeBinding(ADChannel::Brake, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);

		addSwitchBinding(SwitchId::GearUp, SDL_GAMEPAD_AXIS_RIGHTY);
		addSwitchBinding(SwitchId::GearDown, SDL_GAMEPAD_AXIS_RIGHTY, true);
		addSwitchBinding(SwitchId::GearUp, SDL_GAMEPAD_BUTTON_A);
		addSwitchBinding(SwitchId::GearDown, SDL_GAMEPAD_BUTTON_B);

		addSwitchBinding(SwitchId::Start, SDL_GAMEPAD_BUTTON_START);
		addSwitchBinding(SwitchId::Back, SDL_GAMEPAD_BUTTON_BACK);
		addSwitchBinding(SwitchId::A, SDL_GAMEPAD_BUTTON_A);
		addSwitchBinding(SwitchId::B, SDL_GAMEPAD_BUTTON_B);
		addSwitchBinding(SwitchId::X, SDL_GAMEPAD_BUTTON_X);
		addSwitchBinding(SwitchId::Y, SDL_GAMEPAD_BUTTON_Y);

		addSwitchBinding(SwitchId::ChangeView, SDL_GAMEPAD_BUTTON_Y);

		addSwitchBinding(SwitchId::SelectionUp, SDL_GAMEPAD_AXIS_LEFTY, true);
		addSwitchBinding(SwitchId::SelectionDown, SDL_GAMEPAD_AXIS_LEFTY, false);
		addSwitchBinding(SwitchId::SelectionLeft, SDL_GAMEPAD_AXIS_LEFTX, true);
		addSwitchBinding(SwitchId::SelectionRight, SDL_GAMEPAD_AXIS_LEFTX, false);
		addSwitchBinding(SwitchId::SelectionUp, SDL_GAMEPAD_BUTTON_DPAD_UP);
		addSwitchBinding(SwitchId::SelectionDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
		addSwitchBinding(SwitchId::SelectionLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
		addSwitchBinding(SwitchId::SelectionRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

		// Some reason signin/license need both X/Y and SignIn/License bound, odd
		addSwitchBinding(SwitchId::SignIn, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
		addSwitchBinding(SwitchId::License, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
		addSwitchBinding(SwitchId::X, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
		addSwitchBinding(SwitchId::Y, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
	}

	//
	// INI parsing
	//
	struct IniEntry
	{
		std::string section;
		std::string key;
		std::string value;
	};

	// text -> entries. Knows nothing about actions or bindings.
	static std::vector<IniEntry> parseIniLines(std::istream& in)
	{
		std::vector<IniEntry> entries;
		std::string section;
		std::string line;

		while (std::getline(in, line))
		{
			line = Util::trim(line);
			if (line.empty() || line.front() == '#' || line.front() == ';')
				continue;

			if (line.front() == '[' && line.back() == ']')
			{
				section = line.substr(1, line.size() - 2);
				continue;
			}

			auto delim = line.find('=');
			if (delim == std::string::npos)
				continue;

			entries.push_back({ section,
				Util::trim(line.substr(0, delim)),
				Util::trim(line.substr(delim + 1)) });
		}

		return entries;
	}

	struct ActionRef
	{
		bool isVolume = false;
		int index = -1;
		bool negate = false;
	};

	// "Steering-" -> { volume, 0, negated }
	static std::optional<ActionRef> parseActionName(std::string_view name)
	{
		ActionRef ref;

		if (!name.empty() && name.back() == '-')
		{
			ref.negate = true;
			name.remove_suffix(1);
		}

		const std::string trimmed(name);

		for (int i = 0; i < int(ADChannel::Count); i++)
			if (!stricmp(trimmed.c_str(), volumeNames[i].c_str()))
			{
				ref.isVolume = true;
				ref.index = i;
				return ref;
			}

		for (int i = 0; i < int(SwitchId::Count); i++)
			if (!stricmp(trimmed.c_str(), switchNames[i].c_str()))
			{
				ref.isVolume = false;
				ref.index = i;
				return ref;
			}

		return std::nullopt;
	}

	// "LS-X" / "Return" -> InputBinding (negate applied by the caller)
	static std::optional<InputBinding> parseBindingValue(const std::string& value, bool keyboard)
	{
		if (keyboard)
		{
			SDL_Scancode scancode = SDL_GetScancodeFromName(value.c_str());
			if (scancode == SDL_SCANCODE_UNKNOWN)
				return std::nullopt;
			return InputBinding(scancode);
		}

		if (auto axis = InputNames::axisFromIni(value))
			return InputBinding(*axis);
		if (auto button = InputNames::buttonFromIni(value))
			return InputBinding(*button);

		return std::nullopt;
	}

	void addBinding(const ActionRef& ref, const InputBinding& binding)
	{
		if (ref.isVolume)
			volumeBindings[ref.index].add(binding);
		else
			switchBindings[ref.index].add(binding);
	}

	bool readBindingIni(const std::filesystem::path& iniPath)
	{
		if (!std::filesystem::exists(iniPath))
			return false;

		spdlog::info(__FUNCTION__ " - reading INI from {}", iniPath.string());

		std::ifstream file(iniPath);
		if (!file || !file.is_open())
		{
			spdlog::error(__FUNCTION__ " - failed to read INI, using defaults");
			return false;
		}

		const auto entries = parseIniLines(file);
		file.close();

		int key_binds = 0;
		int pad_binds = 0;
		for (const auto& entry : entries)
		{
			if (!stricmp(entry.section.c_str(), "Keyboard"))
				key_binds++;
			else if (!stricmp(entry.section.c_str(), "Gamepad"))
				pad_binds++;
		}

		if (key_binds <= 0 && pad_binds <= 0)
		{
			spdlog::error(__FUNCTION__ " - failed to read binds from INI, using defaults");
			return false;
		}

		spdlog::info(__FUNCTION__ " - {} key binds, {} pad binds", key_binds, pad_binds);

		// we have binds, reset any of our defaults
		for (auto& binding : volumeBindings)
			binding.clear();
		for (auto& binding : switchBindings)
			binding.clear();

		for (const auto& entry : entries)
		{
			const bool keyboard = !stricmp(entry.section.c_str(), "Keyboard");
			if (!keyboard && stricmp(entry.section.c_str(), "Gamepad"))
				continue; // unknown section

			auto action = parseActionName(entry.key);
			if (!action)
			{
				spdlog::error(__FUNCTION__ ": failed to parse binding action for {} = {}", entry.key, entry.value);
				continue;
			}

			auto binding = parseBindingValue(entry.value, keyboard);
			if (!binding)
			{
				spdlog::error(__FUNCTION__ ": failed to parse binding for {} = {}", entry.key, entry.value);
				continue;
			}

			binding->negate = action->negate;
			addBinding(*action, *binding);
		}

		return true;
	}

	// Writes every binding of one source kind (pad or keyboard) for one section.
	void writeBindingSection(std::ostream& file, bool keyboard)
	{
		auto writeAction = [&](const std::string& name, const InputAction& action)
		{
			for (const auto& bind : action.bindings())
			{
				if (bind.isKeyboard() != keyboard)
					continue;

				const std::string direction = bind.isNegated() ? "-" : "";
				file << name << direction << " = " << bind.iniName() << "\n";
			}
		};

		for (int i = 0; i < int(ADChannel::Count); ++i)
			writeAction(volumeNames[i], volumeBindings[i]);

		for (int i = 0; i < int(SwitchId::Count); ++i)
			writeAction(switchNames[i], switchBindings[i]);
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
		writeBindingSection(file, false);

		file << "\n[Keyboard]\n";
		writeBindingSection(file, true);

		file.close();
		spdlog::info(__FUNCTION__": saved to INI file: {}", iniPath.string());

		return true;
	}

	void pumpSdlEvents()
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
	}

	// Reads every volume binding into the cache the game later queries.
	// Skipped entirely while the binding dialog is up, so a stick being waved
	// around to pick a bind doesn't steer the car.
	void updateVolumes(SDL_Gamepad* gamepad)
	{
		for (size_t i = 0; i < volumeBindings.size(); ++i)
		{
			auto& vol = volumeBindings[i].update(gamepad);
			if (Overlay::IsBindingDialogActive) [[unlikely]]
				continue;

			volumes[i] = vol;

			// Steering runs through the game's own sensitivity curve so that
			// the in-game sensitivity setting still applies.
			if (i == 0 && !BypassGameSensitivity)
			{
				int cur = ceil(volumes[i].currentValue * 127.0f);
				int prev = ceil(volumes[i].previousValue * 127.0f);
				volumes[i].currentValue = Sumo_CalcSteerSensitivity_wrapper(cur, prev) / 127.0f;
				volumeBindings[i].setState(volumes[i]);
			}
		}
	}

	// Collapses the switch bindings into a bitmask, one bit per SwitchId.
	uint32_t readSwitchMask(SDL_Gamepad* gamepad)
	{
		uint32_t mask = 0;
		for (size_t i = 0; i < switchBindings.size(); ++i)
		{
			auto& switchState = switchBindings[i].update(gamepad);
			if (switchState.isPressed())
			{
				mask |= (1 << i);
				lastInputSource_ = switchState.lastSourceType;
			}
		}
		return mask;
	}

	void update()
	{
		pumpSdlEvents();

		auto* gamepad = getPrimaryGamepad();

		switch_previous = switch_current;

		// Whatever opened the binding dialog (or started a listen) is still
		// physically held down right now. Passing that press on would instantly
		// re-trigger whatever we just opened, so latch a suppression flag and
		// swallow input until the user has released everything.
		if (isListeningForInput == ListenState::Listening)
			suppressOverlayUntilRelease = true;
		if (Overlay::IsBindingDialogActive)
			suppressGameUntilRelease = true;

		updateVolumes(gamepad);
		switch_current = readSwitchMask(gamepad);

		// Everything released - safe to start passing input on again.
		if (switch_current == 0) [[likely]]
		{
			suppressOverlayUntilRelease = false;
			suppressGameUntilRelease = false;
		}

		// Two consumers, suppressed independently. The overlay is masked first,
		// so anything hidden from the overlay is hidden from the game as well.
		if (suppressOverlayUntilRelease) [[unlikely]]
			switch_current = 0;

		switch_overlay = switch_current;

		if (suppressGameUntilRelease || Overlay::IsBindingDialogActive) [[unlikely]]
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
	template <typename... Args>
	void addVolumeBinding(ADChannel id, Args&&... args)
	{
		volumeBindings[int(id)].add(InputBinding(std::forward<Args>(args)...));
	}

	template <typename... Args>
	void addSwitchBinding(SwitchId id, Args&&... args)
	{
		switchBindings[int(id)].add(InputBinding(std::forward<Args>(args)...));
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

constexpr uint32_t StartSwitchMask = 1 << int(SwitchId::Start);

void InputManager_Update();
void InputManager_SetVibration(WORD left, WORD right);
