#pragma once

#include <SDL3/SDL.h>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <memory>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "overlay/overlay.hpp"

#include <array>
#include <optional>
#include <istream>
#include <sstream>
#include "input_names.hpp"

#include "imgui.h"
#include <format>
#include <string>
#include <fstream>
#include <functional>
#include <charconv>

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

// Actions belonging to the mod rather than the game.
enum class ModAction
{
	OverlayToggle,
	HudToggle,
	OpenChat,
	MusicNext,
	MusicPrevious,
	VRRecenter,
	Count
};

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
	InputSourceType lastSourceType = InputSourceType::GamePad;

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
// A single bound input: one gamepad/raw-device button or axis, hat direction,
// or keyboard key. Raw device identity survives SDL instance-ID changes.
//
struct InputBinding
{
	enum class Kind : uint8_t { None, PadButton, PadAxis, JoyButton, JoyAxis, JoyHat, Key };
	enum class AxisMode : uint8_t { Signed, FromRest };

	static constexpr float StickRange = 32768.f;
	static constexpr float RStickDeadzone = 0.7f; // needs a high deadzone or flicks bounce

	Kind kind = Kind::None;
	bool negate = false;
	std::string deviceGuid;
	int deviceOccurrence = 0;
	Uint16 deviceVendor = 0;
	Uint16 deviceProduct = 0;
	std::string deviceSerial;
	std::string devicePath;
	AxisMode axisMode = AxisMode::Signed;
	int axisMinimum = -32768;
	int axisRest = 0;
	int axisMaximum = 32767;
	bool axisPositive = true;
	union
	{
		SDL_GamepadButton button;
		SDL_GamepadAxis axis;
		SDL_Scancode key;
		int controlIndex;
	};
	Uint8 hatMask = SDL_HAT_CENTERED;

	InputBinding() : button(SDL_GAMEPAD_BUTTON_INVALID) {}
	InputBinding(SDL_GamepadButton b, bool n = false) : kind(Kind::PadButton), negate(n), button(b) {}
	InputBinding(SDL_GamepadAxis a, bool n = false) : kind(Kind::PadAxis), negate(n), axis(a) {}
	InputBinding(SDL_Scancode k, bool n = false) : kind(Kind::Key), negate(n), key(k) {}
	static InputBinding joystickButton(std::string guid, int occurrence, int index)
	{
		InputBinding binding;
		binding.kind = Kind::JoyButton;
		binding.deviceGuid = std::move(guid);
		binding.deviceOccurrence = occurrence;
		binding.controlIndex = index;
		return binding;
	}
	static InputBinding joystickAxis(std::string guid, int occurrence, int index, bool n = false,
		AxisMode mode = AxisMode::Signed, int rest = 0, bool positive = true)
	{
		InputBinding binding;
		binding.kind = Kind::JoyAxis;
		binding.deviceGuid = std::move(guid);
		binding.deviceOccurrence = occurrence;
		binding.controlIndex = index;
		binding.negate = n;
		binding.axisMode = mode;
		binding.axisRest = rest;
		binding.axisPositive = positive;
		return binding;
	}
	static InputBinding joystickHat(std::string guid, int occurrence, int index, Uint8 mask)
	{
		InputBinding binding;
		binding.kind = Kind::JoyHat;
		binding.deviceGuid = std::move(guid);
		binding.deviceOccurrence = occurrence;
		binding.controlIndex = index;
		binding.hatMask = mask;
		return binding;
	}

	bool isAxis() const { return kind == Kind::PadAxis || kind == Kind::JoyAxis; }
	bool isKeyboard() const { return kind == Kind::Key; }
	bool isGamepad() const { return kind == Kind::PadButton || kind == Kind::PadAxis; }
	bool isRawDevice() const { return kind == Kind::JoyButton || kind == Kind::JoyAxis || kind == Kind::JoyHat; }
	bool isNegated() const { return negate; }

	InputSourceType sourceType() const
	{
		return isKeyboard() ? InputSourceType::Keyboard : InputSourceType::GamePad;
	}

	float read(SDL_Gamepad* gamepad, const std::function<SDL_Joystick*(const InputBinding&)>& joystickForBinding) const
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
		case Kind::JoyButton:
		{
			auto* joystick = joystickForBinding(*this);
			if (!joystick)
				return 0.0f;
			value = float(SDL_GetJoystickButton(joystick, controlIndex));
			break;
		}
		case Kind::JoyAxis:
		{
			auto* joystick = joystickForBinding(*this);
			if (!joystick)
				return 0.0f;
			const int raw = SDL_GetJoystickAxis(joystick, controlIndex);
			if (axisMode == AxisMode::FromRest)
			{
				const int span = axisPositive ? axisMaximum - axisRest : axisRest - axisMinimum;
				const int travel = axisPositive ? raw - axisRest : axisRest - raw;
				value = span > 0 ? std::clamp(float(travel) / float(span), 0.0f, 1.0f) : 0.0f;
			}
			else if (raw >= axisRest)
			{
				const int span = axisMaximum - axisRest;
				value = span > 0 ? std::clamp(float(raw - axisRest) / float(span), 0.0f, 1.0f) : 0.0f;
			}
			else
			{
				const int span = axisRest - axisMinimum;
				value = span > 0 ? -std::clamp(float(axisRest - raw) / float(span), 0.0f, 1.0f) : 0.0f;
			}
			break;
		}
		case Kind::JoyHat:
		{
			auto* joystick = joystickForBinding(*this);
			if (!joystick)
				return 0.0f;
			value = (SDL_GetJoystickHat(joystick, controlIndex) & hatMask) == hatMask ? 1.0f : 0.0f;
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
		case Kind::JoyAxis:   return std::format("Axis {}", controlIndex + 1);
		case Kind::JoyButton: return std::format("Button {}", controlIndex + 1);
		case Kind::JoyHat:    return std::format("Hat {} (0x{:02X})", controlIndex + 1, hatMask);
		default:              return "";
		}
	}

	static std::string encodeIniField(std::string_view value)
	{
		std::string encoded;
		for (const unsigned char c : value)
		{
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
				encoded.push_back(char(c));
			else
				encoded += std::format("%{:02X}", c);
		}
		return encoded;
	}

	static std::string decodeIniField(std::string_view value)
	{
		std::string decoded;
		for (size_t i = 0; i < value.size(); ++i)
		{
			if (value[i] == '%' && i + 2 < value.size())
			{
				unsigned int byte = 0;
				auto [ptr, error] = std::from_chars(value.data() + i + 1, value.data() + i + 3, byte, 16);
				if (error == std::errc{} && ptr == value.data() + i + 3)
				{
					decoded.push_back(char(byte));
					i += 2;
					continue;
				}
			}
			decoded.push_back(value[i]);
		}
		return decoded;
	}

	std::string deviceIdentityIni() const
	{
		return std::format("{}|{}|{}|{}", deviceVendor, deviceProduct,
			encodeIniField(deviceSerial), encodeIniField(devicePath));
	}

	std::string iniName() const
	{
		switch (kind)
		{
		case Kind::Key:       return SDL_GetScancodeName(key);
		case Kind::PadAxis:   return InputNames::iniNameForAxis(axis);
		case Kind::PadButton: return InputNames::iniNameForButton(button);
		case Kind::JoyAxis:   return std::format("{}|{}|axis|{}|0|{}|{}|{}|{}|{}|{}",
			deviceGuid, deviceOccurrence, controlIndex, int(axisMode), axisMinimum, axisRest, axisMaximum,
			axisPositive, deviceIdentityIni());
		case Kind::JoyButton: return std::format("{}|{}|button|{}|0|0|-32768|0|32767|1|{}",
			deviceGuid, deviceOccurrence, controlIndex, deviceIdentityIni());
		case Kind::JoyHat:    return std::format("{}|{}|hat|{}|{}|0|-32768|0|32767|1|{}",
			deviceGuid, deviceOccurrence, controlIndex, hatMask, deviceIdentityIni());
		default:              return "";
		}
	}
};

class InputAction
{
	std::vector<InputBinding> bindings_;
	InputState state_;

public:
	const InputState& update(SDL_Gamepad* primary_pad,
		const std::function<SDL_Joystick*(const InputBinding&)>& joystickForBinding,
		bool positiveOnly = false)
	{
		float maxValue = 0.0f;
		bool isAxisInput = false;
		InputSourceType lastSource = state_.lastSourceType;

		// Analog actions need the largest magnitude so signed steering survives.
		// Digital actions pass positiveOnly=true: the negative side of an axis is
		// inactive for that action and must never mask another +1 button binding.
		for (const auto& binding : bindings_)
		{
			float currentValue = binding.read(primary_pad, joystickForBinding);
			if (positiveOnly && currentValue <= 0.0f)
				continue;
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

// ReadSwitch translates the raw DirectInput button mask into the SwitchId bits
// the rest of the game uses. Code that reads the raw mask instead needs to see
// the same presses, so this is that translation table, inverted.
struct RawButtonMapping
{
	uint32_t rawBit;
	SwitchId switchId;
};
inline constexpr RawButtonMapping RawButtonMap[] = {
	{ 0x00000001, SwitchId::Start          },
	{ 0x00000200, SwitchId::Back           },
	{ 0x00000002, SwitchId::A              },
	{ 0x00000004, SwitchId::B              },
	{ 0x00000008, SwitchId::X              },
	{ 0x00000010, SwitchId::Y              },
	{ 0x00000040, SwitchId::SelectionUp    },
	{ 0x00000020, SwitchId::SelectionDown  },
	{ 0x00000100, SwitchId::SelectionLeft  },
	{ 0x00000080, SwitchId::SelectionRight },
	{ 0x00000800, SwitchId::License        },
	{ 0x00000400, SwitchId::SignIn         },
	{ 0x08000000, SwitchId::Unknown0x200   },
	{ 0x00100000, SwitchId::Unknown0x100   },
};

// Two buttons that ReadSwitch has no entry for, so they reach the game only
// through the raw mask. A DirectInput pad reports the triggers here, and the
// Sumo car select screen toggles between its two car lists on the right one.
inline constexpr uint32_t RawTriggerLeft = 0x1000;
inline constexpr uint32_t RawTriggerRight = 0x2000;
inline constexpr float RawTriggerThreshold = 0.5f;

class InputManager
{
public:
	// Which of the three binding tables an action lives in.
	enum class ActionKind { Volume, Switch, Mod };

	// Every SDL joystick is kept here, including devices SDL does not classify as
	// a standard gamepad (wheels, pedal sets, shifters and button boxes). A
	// gamepad may also have an SDL_Gamepad handle in controllers; the shared
	// instance ID lets the UI and future bindings treat those as one device.
	struct InputDevice
	{
		SDL_JoystickID instanceId = 0;
		SDL_Joystick* joystick = nullptr;
		bool isGamepad = false;
		std::string guid;
		int occurrence = 0;
		Uint16 vendor = 0;
		Uint16 product = 0;
		std::string serial;
		std::string path;
	};

private:
	std::array<InputAction, size_t(ADChannel::Count)> volumeBindings;
	std::array<InputAction, size_t(SwitchId::Count)> switchBindings;
	std::array<InputAction, size_t(ModAction::Count)> modBindings;

	std::mutex mtx;
	std::vector<InputDevice> devices;
	std::vector<SDL_Gamepad*> controllers;
	int primaryControllerIndex = -1;

	SDL_Window* window = nullptr;

	// cached values as of last update call
	std::array<InputState, size_t(ADChannel::Count)> volumes;
	std::array<InputState, size_t(ModAction::Count)> modStates;

	// Mod actions are readable while the overlay is up, since the overlay toggle
	// has to be able to close it again, but not while the binding dialog owns
	// every input.
	bool modActionsDeaf = false;

	// Switch bitmasks. switch_current/_previous are what the game sees;
	// switch_overlay is the same data before game-side suppression, so the
	// overlay can still be driven while the game is deaf.
	uint32_t switch_current = 0;
	uint32_t switch_previous = 0;
	uint32_t switch_overlay = 0;

	// Mirror of the raw DirectInput masks. Edges are tracked here rather than
	// read back out of the game's copy, because DInputUpdate rewrites the same
	// fields whenever a device is still being polled.
	uint32_t raw_buttons = 0;
	uint32_t raw_pressed = 0;
	uint32_t raw_released = 0;

	// Latched until the user releases everything - see update().
	// Previously function-local statics inside update().
	bool suppressOverlayUntilRelease = false;
	bool suppressGameUntilRelease = false;
	InputSourceType lastInputSource_ = InputSourceType::GamePad;

private:
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

	static inline const std::string modNames[] = {
		"Overlay",
		"HUD Toggle",
		"Open Chat",
		"Music Next",
		"Music Previous",
		"VR Recenter"
	};
	static_assert(std::size(modNames) == size_t(ModAction::Count));

	// switchNames must stay 1:1 with SwitchId - the ini reader/writer index both
	// by the same value. (volumeNames deliberately does NOT match ADChannel:
	// the enum reserves 4 unused AD channels, so always bound by std::size.)
	static_assert(std::size(switchNames) == size_t(SwitchId::Count));

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
		if (std::find_if(controllers.begin(), controllers.end(), [instanceId](SDL_Gamepad* existing)
			{
				return SDL_GetGamepadID(existing) == instanceId;
			}) != controllers.end())
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

	void onJoystickAdded(SDL_JoystickID instanceId)
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (std::find_if(devices.begin(), devices.end(), [instanceId](const InputDevice& device)
			{
				return device.instanceId == instanceId;
			}) != devices.end())
			return;

		SDL_Joystick* joystick = SDL_OpenJoystick(instanceId);
		if (!joystick)
		{
			spdlog::error(__FUNCTION__ "({}): failed to open joystick: {}", instanceId, SDL_GetError());
			return;
		}

		char guidText[33]{};
		SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guidText, int(std::size(guidText)));
		const std::string guid(guidText);
		int occurrence = 0;
		while (std::any_of(devices.begin(), devices.end(), [&guid, occurrence](const InputDevice& device)
			{
				return device.guid == guid && device.occurrence == occurrence;
			}))
		{
			++occurrence;
		}
		const char* serialText = SDL_GetJoystickSerial(joystick);
		const char* pathText = SDL_GetJoystickPath(joystick);
		InputDevice device{ instanceId, joystick, SDL_IsGamepad(instanceId), guid, occurrence,
			SDL_GetJoystickVendor(joystick), SDL_GetJoystickProduct(joystick),
			serialText ? serialText : "", pathText ? pathText : "" };
		devices.push_back(device);
		spdlog::info("Input device connected: {} (id {}, {} axes, {} buttons, {} hats, gamepad: {})",
			SDL_GetJoystickName(joystick), instanceId,
			SDL_GetNumJoystickAxes(joystick), SDL_GetNumJoystickButtons(joystick),
			SDL_GetNumJoystickHats(joystick), device.isGamepad);
	}

	static bool deviceMatchesBinding(const InputDevice& device, const InputBinding& binding)
	{
		const bool usbIdentityMatches = (!binding.deviceVendor || device.vendor == binding.deviceVendor) &&
			(!binding.deviceProduct || device.product == binding.deviceProduct);
		if (!usbIdentityMatches)
			return false;

		// A real serial is the strongest stable identity. USB paths are not: they
		// can change when the same wheel/pedal set is moved to another port or hub.
		if (!binding.deviceSerial.empty())
			return device.serial == binding.deviceSerial;

		if (!binding.deviceGuid.empty() && device.guid != binding.deviceGuid)
			return false;
		if (device.occurrence == binding.deviceOccurrence)
			return true;

		// Path is only a duplicate-device tie-breaker/fallback, never a mandatory
		// identity for otherwise matching VID/PID/GUID hardware.
		return !binding.devicePath.empty() && device.path == binding.devicePath;
	}

	SDL_Joystick* joystickForBinding(const InputBinding& binding) const
	{
		auto it = std::find_if(devices.begin(), devices.end(), [&binding](const InputDevice& device)
			{
				return deviceMatchesBinding(device, binding);
			});
		return it == devices.end() ? nullptr : it->joystick;
	}

	const InputDevice* deviceForBinding(const InputBinding& binding) const
	{
		auto it = std::find_if(devices.begin(), devices.end(), [&binding](const InputDevice& device)
			{
				return deviceMatchesBinding(device, binding);
			});
		return it == devices.end() ? nullptr : &*it;
	}

	int deviceMatchCount(const InputBinding& binding) const
	{
		return int(std::count_if(devices.begin(), devices.end(), [&binding](const InputDevice& device)
			{
				return deviceMatchesBinding(device, binding);
			}));
	}

	void onJoystickRemoved(SDL_JoystickID instanceId)
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = std::find_if(devices.begin(), devices.end(), [instanceId](const InputDevice& device)
			{
				return device.instanceId == instanceId;
			});
		if (it == devices.end())
			return;

		spdlog::info("Input device disconnected: {} (id {})", SDL_GetJoystickName(it->joystick), instanceId);
		SDL_CloseJoystick(it->joystick);
		devices.erase(it);
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
			const int removedIndex = int(std::distance(controllers.begin(), it));
			const bool removedPrimary = (removedIndex == primaryControllerIndex);

			SDL_CloseGamepad(*it);
			controllers.erase(it);

			spdlog::debug(__FUNCTION__ "(instance {}): removed instance", instanceId);

			if (controllers.empty())
			{
				Game::CurrentPadType = Game::GamepadType::PC;
				setPrimaryGamepad(-1);
			}
			else if (removedPrimary)
			{
				const int replacementIndex =
                    removedIndex < int(controllers.size())
                        ? removedIndex
                        : int(controllers.size()) - 1;
                setPrimaryGamepad(replacementIndex);
			}
			else if (primaryControllerIndex > removedIndex)
			{
				--primaryControllerIndex;
				if (auto* pad = getPrimaryGamepad())
					setupGamepad(pad);
			}
		}
	}

public:
	~InputManager()
	{
		shutdown();
	}

	void shutdown()
	{
		for (auto controller : controllers)
			SDL_CloseGamepad(controller);
		controllers.clear();
		for (auto& device : devices)
			SDL_CloseJoystick(device.joystick);
		devices.clear();
		primaryControllerIndex = -1;
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

	void init(HWND hwnd);

	// Actions introduced after a bindings INI was written load with no entry.
	// Keep the overlay reachable and give VR recenter an F12 default that does not collide with legacy HUD F10
	// until the user chooses a custom keyboard/wheel/gamepad binding.
	void ensureOverlayBindable()
	{
		InputAction& overlay = modBindings[size_t(ModAction::OverlayToggle)];
		if (overlay.bindings().empty())
		{
			spdlog::warn(__FUNCTION__ ": overlay toggle had no bindings, restoring F11");
			addModBinding(ModAction::OverlayToggle, SDL_SCANCODE_F11);
		}

		InputAction& recenter = modBindings[size_t(ModAction::VRRecenter)];
		if (recenter.bindings().empty())
		{
			spdlog::info(__FUNCTION__ ": VR recenter had no binding, restoring F12");
			addModBinding(ModAction::VRRecenter, SDL_SCANCODE_F12);
		}
	}

	void setupDefaultBindings()
	{
		// Remove any previous bindings
		for (auto& binding : volumeBindings)
			binding.clear();
		for (auto& binding : switchBindings)
			binding.clear();
		for (auto& binding : modBindings)
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

		// Mod actions.
		addModBinding(ModAction::OverlayToggle, SDL_SCANCODE_F11);
		addModBinding(ModAction::VRRecenter, SDL_SCANCODE_F12);
		addModBinding(ModAction::OpenChat, SDL_SCANCODE_Y);
		addModBinding(ModAction::MusicNext, SDL_SCANCODE_X);
		addModBinding(ModAction::MusicNext, SDL_GAMEPAD_BUTTON_BACK);
		addModBinding(ModAction::MusicPrevious, SDL_SCANCODE_Z);
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
		using Kind = ActionKind;

		Kind kind = Kind::Switch;
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

		for (int i = 0; i < int(std::size(volumeNames)); i++)
			if (!stricmp(trimmed.c_str(), volumeNames[i].c_str()))
			{
				ref.kind = ActionRef::Kind::Volume;
				ref.index = i;
				return ref;
			}

		for (int i = 0; i < int(SwitchId::Count); i++)
			if (!stricmp(trimmed.c_str(), switchNames[i].c_str()))
			{
				ref.kind = ActionRef::Kind::Switch;
				ref.index = i;
				return ref;
			}

		for (int i = 0; i < int(ModAction::Count); i++)
			if (!stricmp(trimmed.c_str(), modNames[i].c_str()))
			{
				ref.kind = ActionRef::Kind::Mod;
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

	// guid|occurrence|axis/button/hat|control-index|hat-mask, followed by
	// mode|min|rest|max|positive|vendor|product|serial|path.
	static std::optional<InputBinding> parseDeviceBindingValue(const std::string& value)
	{
		std::vector<std::string> fields;
		std::istringstream stream(value);
		std::string field;
		while (std::getline(stream, field, '|'))
			fields.push_back(field);
		if (fields.size() < 5)
			return std::nullopt;

		try
		{
			const auto parseBool = [](const std::string& text)
			{
				if (!stricmp(text.c_str(), "true")) return true;
				if (!stricmp(text.c_str(), "false")) return false;
				return std::stoi(text) != 0;
			};
			const int occurrence = std::stoi(fields[1]);
			const int control = std::stoi(fields[3]);
			const int hatMask = std::stoi(fields[4]);
			if (occurrence < 0 || control < 0)
				return std::nullopt;

			std::optional<InputBinding> binding;
			if (!stricmp(fields[2].c_str(), "axis"))
			{
				binding = InputBinding::joystickAxis(fields[0], occurrence, control);
				if (fields.size() >= 10)
				{
					binding->axisMode = std::stoi(fields[5]) == int(InputBinding::AxisMode::FromRest)
						? InputBinding::AxisMode::FromRest : InputBinding::AxisMode::Signed;
					binding->axisMinimum = std::clamp(std::stoi(fields[6]), -32768, 32767);
					binding->axisRest = std::clamp(std::stoi(fields[7]), -32768, 32767);
					binding->axisMaximum = std::clamp(std::stoi(fields[8]), -32768, 32767);
					// std::format serializes bools as "true"/"false" while older
					// binding files used 1/0. Accept both so calibration always
					// survives an upgrade and restart.
					binding->axisPositive = parseBool(fields[9]);
				}
			}
			else if (!stricmp(fields[2].c_str(), "button"))
				binding = InputBinding::joystickButton(fields[0], occurrence, control);
			else if (!stricmp(fields[2].c_str(), "hat") && hatMask > 0 && hatMask <= 0xFF)
				binding = InputBinding::joystickHat(fields[0], occurrence, control, Uint8(hatMask));

			if (!binding)
				return std::nullopt;
			if (fields.size() >= 14)
			{
				binding->deviceVendor = Uint16(std::clamp(std::stoi(fields[10]), 0, 0xFFFF));
				binding->deviceProduct = Uint16(std::clamp(std::stoi(fields[11]), 0, 0xFFFF));
				binding->deviceSerial = InputBinding::decodeIniField(fields[12]);
				binding->devicePath = InputBinding::decodeIniField(fields[13]);
			}
			return binding;
		}
		catch (const std::exception&)
		{
			return std::nullopt;
		}
		return std::nullopt;
	}

	void addBinding(const ActionRef& ref, const InputBinding& binding)
	{
		switch (ref.kind)
		{
		case ActionRef::Kind::Volume: volumeBindings[ref.index].add(binding); break;
		case ActionRef::Kind::Mod:    modBindings[ref.index].add(binding); break;
		default:                      switchBindings[ref.index].add(binding); break;
		}
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
		int device_binds = 0;
		for (const auto& entry : entries)
		{
			if (!stricmp(entry.section.c_str(), "Keyboard"))
				key_binds++;
			else if (!stricmp(entry.section.c_str(), "Gamepad"))
				pad_binds++;
			else if (!stricmp(entry.section.c_str(), "Device"))
				device_binds++;
		}

		if (key_binds <= 0 && pad_binds <= 0 && device_binds <= 0)
		{
			spdlog::error(__FUNCTION__ " - failed to read binds from INI, using defaults");
			return false;
		}

		spdlog::info(__FUNCTION__ " - {} key binds, {} pad binds, {} device binds", key_binds, pad_binds, device_binds);

		// Preserve the current working configuration until at least one entry
		// has parsed successfully. This makes Load bindings transactional for a
		// completely malformed/truncated file.
		auto previousVolumeBindings = volumeBindings;
		auto previousSwitchBindings = switchBindings;
		auto previousModBindings = modBindings;
		for (auto& binding : volumeBindings)
			binding.clear();
		for (auto& binding : switchBindings)
			binding.clear();
		for (auto& binding : modBindings)
			binding.clear();
		int loadedBinds = 0;

		for (const auto& entry : entries)
		{
			const bool keyboard = !stricmp(entry.section.c_str(), "Keyboard");
			const bool gamepad = !stricmp(entry.section.c_str(), "Gamepad");
			const bool device = !stricmp(entry.section.c_str(), "Device");
			if (!keyboard && !gamepad && !device)
				continue; // unknown section

			auto action = parseActionName(entry.key);
			if (!action)
			{
				spdlog::error(__FUNCTION__ ": failed to parse binding action for {} = {}", entry.key, entry.value);
				continue;
			}

			auto binding = device ? parseDeviceBindingValue(entry.value) : parseBindingValue(entry.value, keyboard);
			if (!binding)
			{
				spdlog::error(__FUNCTION__ ": failed to parse binding for {} = {}", entry.key, entry.value);
				continue;
			}

			binding->negate = action->negate;
			if (binding->negate && !binding->isAxis() &&
				!(action->kind == ActionRef::Kind::Volume &&
					action->index == int(ADChannel::Steering)))
			{
				// A '-' suffix on a button/key cannot select an opposite direction;
				// it only turns +1 into -1 and makes digital/pedal actions unusable.
				spdlog::warn(__FUNCTION__ ": ignoring invalid negative suffix for non-axis action {}", entry.key);
				binding->negate = false;
			}
			addBinding(*action, *binding);
			++loadedBinds;
		}

		if (loadedBinds == 0)
		{
			volumeBindings = std::move(previousVolumeBindings);
			switchBindings = std::move(previousSwitchBindings);
			modBindings = std::move(previousModBindings);
			spdlog::error(__FUNCTION__ ": no valid bindings parsed; previous bindings preserved");
			return false;
		}

		ensureOverlayBindable();
		return true;
	}

	// Writes every binding of one source kind (pad or keyboard) for one section.
	void writeBindingSection(std::ostream& file, bool keyboard)
	{
		auto writeAction = [&](const std::string& name, const InputAction& action)
		{
			for (const auto& bind : action.bindings())
			{
				if ((keyboard && !bind.isKeyboard()) || (!keyboard && !bind.isGamepad()))
					continue;

				const std::string direction = bind.isNegated() ? "-" : "";
				file << name << direction << " = " << bind.iniName() << "\n";
			}
		};

		for (int i = 0; i < int(std::size(volumeNames)); ++i)
			writeAction(volumeNames[i], volumeBindings[i]);

		for (int i = 0; i < int(SwitchId::Count); ++i)
			writeAction(switchNames[i], switchBindings[i]);

		for (int i = 0; i < int(ModAction::Count); ++i)
			writeAction(modNames[i], modBindings[i]);
	}

	void writeDeviceBindingSection(std::ostream& file)
	{
		auto writeAction = [&](const std::string& name, const InputAction& action)
			{
				for (const auto& bind : action.bindings())
				{
					if (!bind.isRawDevice())
						continue;
					file << name << (bind.isNegated() ? "-" : "") << " = " << bind.iniName() << "\n";
				}
			};
		for (int i = 0; i < int(std::size(volumeNames)); ++i)
			writeAction(volumeNames[i], volumeBindings[i]);
		for (int i = 0; i < int(SwitchId::Count); ++i)
			writeAction(switchNames[i], switchBindings[i]);
		for (int i = 0; i < int(ModAction::Count); ++i)
			writeAction(modNames[i], modBindings[i]);
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
		file << "# Manage these through the in-game Controls > Configuration screen.\n";
		file << "# This file is maintained automatically and should not need manual editing.\n";
		file << "# If this file doesn't exist or is empty, bindings will be reset to default.\n";
		file << "#\n";
		file << "# Actions with a negative symbol after them ('Steering-') either treat the input as a negative value, or only trigger the action on negative inputs\n";
		file << "# Analog actions bound to digital inputs, eg. 'Steering- = DPad-Left', will make DPad-Left send a negative Steering value, making it move to the left\n";
		file << "# Digital actions bound to analog inputs, eg. 'Gear Down- = RS-Y', will only trigger the action when RS-Y is negative\n";
		file << "# Analog -> analog actions can also be inverted by adding a negative to them\n\n";

		file << "[Gamepad]\n";
		writeBindingSection(file, false);
		file << "\n[Device]\n";
		writeDeviceBindingSection(file);

		file << "\n[Keyboard]\n";
		writeBindingSection(file, true);

		file.flush();
		if (!file)
		{
			spdlog::error(__FUNCTION__ ": failed while writing INI file: {}", iniPath.string());
			file.close();
			return false;
		}
		file.close();
		if (file.fail())
		{
			spdlog::error(__FUNCTION__ ": failed while closing INI file: {}", iniPath.string());
			return false;
		}
		spdlog::info(__FUNCTION__": saved to INI file: {}", iniPath.string());

		return true;
	}

	void pumpSdlEvents()
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
			switch (event.type)
			{
			case SDL_EVENT_JOYSTICK_ADDED:
				onJoystickAdded(event.jdevice.which);
				break;
			case SDL_EVENT_JOYSTICK_REMOVED:
				onJoystickRemoved(event.jdevice.which);
				break;
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
		const auto resolveJoystick = [this](const InputBinding& binding)
			{
				return joystickForBinding(binding);
			};
		for (size_t i = 0; i < volumeBindings.size(); ++i)
		{
			auto& vol = volumeBindings[i].update(gamepad, resolveJoystick);
			if (Overlay::IsBindingDialogActive || Overlay::IsActive) [[unlikely]]
			{
				// Keep the binding's live state for the UI, but never leave a stale
				// throttle/brake/steering command active underneath the overlay.
				volumes[i].previousValue = 0.0f;
				volumes[i].currentValue = 0.0f;
				continue;
			}

			volumes[i] = vol;

			// Steering runs through the game's own sensitivity curve so that
			// the in-game sensitivity setting still applies.
			if (i == 0 && !Settings::BypassGameSensitivity)
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
		const auto resolveJoystick = [this](const InputBinding& binding)
			{
				return joystickForBinding(binding);
			};
		uint32_t mask = 0;
		for (size_t i = 0; i < switchBindings.size(); ++i)
		{
			auto& switchState = switchBindings[i].update(gamepad, resolveJoystick, true);
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

		// The overlay being open used to stop update() running at all, which is
		// what kept the game from seeing input through it. Mod actions have to be
		// read while it is open, so it runs either way now and the game is held
		// off here instead.
		if (suppressGameUntilRelease || Overlay::IsBindingDialogActive || Overlay::IsActive) [[unlikely]]
			switch_current = 0;

		updateModActions(gamepad);

		updateRawDInputState();
	}

	// Mod actions take the overlay's suppression but not the game's, so the
	// overlay toggle can still close the overlay. The binding dialog owns every
	// input while it is up, so nothing fires under it. States keep updating
	// regardless, so a key held across the dialog closing reads as held rather
	// than newly pressed.
	void updateModActions(SDL_Gamepad* gamepad)
	{
		modActionsDeaf = suppressOverlayUntilRelease || Overlay::IsBindingDialogActive;
		const auto resolveJoystick = [this](const InputBinding& binding)
			{
				return joystickForBinding(binding);
			};

		for (size_t i = 0; i < modBindings.size(); ++i)
			modStates[i] = modBindings[i].update(gamepad, resolveJoystick, true);
	}

	// Rebuilds the raw DirectInput masks from the bindings. Called once per
	// update so that a held button produces a single press edge.
	void updateRawDInputState()
	{
		uint32_t buttons = 0;
		for (const auto& mapping : RawButtonMap)
			if (switch_current & (1u << int(mapping.switchId)))
				buttons |= mapping.rawBit;

		if (volumes[int(ADChannel::Acceleration)].currentValue >= RawTriggerThreshold)
			buttons |= RawTriggerRight;
		if (volumes[int(ADChannel::Brake)].currentValue >= RawTriggerThreshold)
			buttons |= RawTriggerLeft;

		raw_pressed = buttons & ~raw_buttons;
		raw_released = raw_buttons & ~buttons;
		raw_buttons = buttons;

		applyRawDInputState();
	}

	// Copies the cached masks into the game's state. Also called after ReadIO,
	// which rebuilds them from whatever device DInputUpdate polled. Copying
	// rather than recomputing means running twice in a frame cannot swallow a
	// press edge.
	void applyRawDInputState()
	{
		if (!Game::dinput_state)
			return;

		Game::dinput_state->buttons_4 = raw_buttons;
		Game::dinput_state->pressed_8 = raw_pressed;
		Game::dinput_state->released_C = raw_released;
	}

	// Table lookups by kind, for the bindings UI which walks all three.
	InputAction& actionFor(ActionKind kind, int index)
	{
		switch (kind)
		{
		case ActionKind::Volume: return volumeBindings[index];
		case ActionKind::Mod:    return modBindings[index];
		default:                 return switchBindings[index];
		}
	}

	static const std::string& actionName(ActionKind kind, int index)
	{
		switch (kind)
		{
		case ActionKind::Volume: return volumeNames[index];
		case ActionKind::Mod:    return modNames[index];
		default:                 return switchNames[index];
		}
	}

	// What to call a mod action's binding in a prompt. Prefers a keyboard one:
	// that is what a reader can press without a pad plugged in, and the pad
	// labels depend on which pad is connected.
	std::string modActionDisplayName(ModAction action)
	{
		const auto& bindings = modBindings[size_t(action)].bindings();
		if (bindings.empty())
			return "(unbound)";

		const InputBinding* pick = &bindings.front();
		for (const auto& binding : bindings)
			if (binding.isKeyboard())
			{
				pick = &binding;
				break;
			}

		auto padType = SDL_GAMEPAD_TYPE_XBOX360;
		if (auto* primary = getPrimaryGamepad())
			padType = SDL_GetGamepadType(primary);

		return pick->displayName(padType);
	}

	// Whether a mod action's binding is down right now. Deliberately not an edge:
	// this updates once per game tick while callers read on their own cadence,
	// which for the overlay is once per rendered frame, so an edge taken here
	// would be seen twice above tick rate and missed below it. Callers compare
	// against their own previous value instead, which is right at any rate.
	//
	// False while the binding dialog is up, so a key being bound never also fires
	// what it is bound to.
	bool modActionHeld(ModAction action) const
	{
		if (modActionsDeaf) [[unlikely]]
			return false;

		return modStates[size_t(action)].isPressed();
	}

	const InputAction& modAction(ModAction action) const { return modBindings[size_t(action)]; }
	InputAction& modAction(ModAction action) { return modBindings[size_t(action)]; }
	static const std::string& modActionName(ModAction action) { return modNames[size_t(action)]; }

	void stopVibration()
	{
		std::lock_guard<std::mutex> lock(mtx);
		for (auto* controller : controllers)
			SDL_RumbleGamepad(controller, 0, 0, 0);
	}

	void setVibration(WORD left, WORD right)
	{
		auto* controller = getPrimaryGamepad();
		if (!controller)
			return;

		std::lock_guard<std::mutex> lock(mtx);
		SDL_RumbleGamepad(controller, left, right, 1000);

		// TODO: SDL_RumbleGamepadTriggers doesn't appear to work with any backend?
		// Disabling this code for now, we'll rely on the old ImpulseVibration / DetourDeviceIoControl method instead.
#if 0
		if (Settings::ImpulseVibrationMode != 0)
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
#endif
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
	template <typename... Args>
	void addModBinding(ModAction id, Args&&... args)
	{
		modBindings[int(id)].add(InputBinding(std::forward<Args>(args)...));
	}

	bool anyInputPressed()
	{
		int key_count = 0;
		const bool* key_state = SDL_GetKeyboardState(&key_count);
		for (int i = 0; i < key_count; i++)
			if (key_state[i])
				return true;

		for (const auto& device : devices)
		{
			for (int button = 0; button < SDL_GetNumJoystickButtons(device.joystick); ++button)
				if (SDL_GetJoystickButton(device.joystick, button))
					return true;
			for (int hat = 0; hat < SDL_GetNumJoystickHats(device.joystick); ++hat)
				if (SDL_GetJoystickHat(device.joystick, hat) != SDL_HAT_CENTERED)
					return true;
		}

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

	// Process-lifetime storage avoids calling SDL's DirectInput backend from a
	// C++ global destructor after SDL/Windows have begun their own teardown.
	// Normal window close still calls shutdown(); forced ExitProcess cleanup is
	// deliberately left to the operating system.
	static InputManager& instance;
};

constexpr uint32_t StartSwitchMask = 1 << int(SwitchId::Start);

void InputManager_Update();
bool InputManager_ModActionHeld(ModAction action);
std::string InputManager_ModActionDisplayName(ModAction action);
void InputManager_SetVibration(WORD left, WORD right);
void InputManager_Shutdown();
float InputManager_SteeringValue();
