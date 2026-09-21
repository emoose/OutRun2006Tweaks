#include "input_manager.hpp"
#include "wheel_profile_store.hpp"
#include "wheel_ffb_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

//
// Binding editor.
//
// An action holds a list of bindings and fires on whichever of them reads
// highest, so several inputs per action can be assigned.
// This screen displays each action on the left, and selecting one will
// display and allow changing the bindings for it on the right.
//
class InputBindingsUI : public OverlayWindow
{
public:
	// A modal that in-game code can raise on its own, so it has to be drawn
	// whether or not the overlay is open.
	Kind kind() const override { return Kind::Hud; }
	const char* name() const override { return "Input Bindings"; }

private:
	// One action, either an analog channel or a digital switch. Held by value
	// rather than by pointer because the prompt outlives a frame, and
	// setupDefaultBindings()/readBindingIni() replace the binding vectors.
	struct Selection
	{
		using Kind = InputManager::ActionKind;

		Kind kind = Kind::Volume;
		int index = 0;

		bool operator==(const Selection& other) const
		{
			return kind == other.kind && index == other.index;
		}

		bool isVolume() const { return kind == Kind::Volume; }
	};

	// The order actions are listed in, grouped by when they get used.
	struct ActionListEntry
	{
		const char* group;
		Selection::Kind kind;
		int index;
	};

	static constexpr auto Vol = Selection::Kind::Volume;
	static constexpr auto Sw = Selection::Kind::Switch;
	static constexpr auto Mod = Selection::Kind::Mod;

	static inline const ActionListEntry ActionList[] = {
		{ "Driving", Vol, int(ADChannel::Steering)        },
		{ "Driving", Vol, int(ADChannel::Acceleration)    },
		{ "Driving", Vol, int(ADChannel::Brake)           },
		{ "Driving", Sw,  int(SwitchId::GearUp)           },
		{ "Driving", Sw,  int(SwitchId::GearDown)         },
		{ "Driving", Sw,  int(SwitchId::ChangeView)       },

		{ "Menus",   Sw,  int(SwitchId::Start)            },
		{ "Menus",   Sw,  int(SwitchId::Back)             },
		{ "Menus",   Sw,  int(SwitchId::A)                },
		{ "Menus",   Sw,  int(SwitchId::B)                },
		{ "Menus",   Sw,  int(SwitchId::X)                },
		{ "Menus",   Sw,  int(SwitchId::Y)                },
		{ "Menus",   Sw,  int(SwitchId::SelectionUp)      },
		{ "Menus",   Sw,  int(SwitchId::SelectionDown)    },
		{ "Menus",   Sw,  int(SwitchId::SelectionLeft)    },
		{ "Menus",   Sw,  int(SwitchId::SelectionRight)   },

		{ "Online",  Sw,  int(SwitchId::License)          },
		{ "Online",  Sw,  int(SwitchId::SignIn)           },

		{ "Tweaks",  Mod, int(ModAction::OverlayToggle)   },
		{ "Tweaks",  Mod, int(ModAction::HudToggle)       },
		{ "Tweaks",  Mod, int(ModAction::OpenChat)        },
		{ "Tweaks",  Mod, int(ModAction::MusicNext)       },
		{ "Tweaks",  Mod, int(ModAction::MusicPrevious)   },
		{ "VR",      Mod, int(ModAction::VRRecenter)      },
	};

	struct QuickSetupEntry
	{
		const char* title;
		const char* prompt;
		Selection::Kind kind;
		int index;
	};

	static inline const QuickSetupEntry QuickSetupSteps[] = {
		{ "Steering",    "Turn the wheel or move the stick you want to steer with.", Vol, int(ADChannel::Steering) },
		{ "Accelerator", "Press the accelerator fully.",                              Vol, int(ADChannel::Acceleration) },
		{ "Brake",       "Press the brake fully.",                                    Vol, int(ADChannel::Brake) },
		{ "Shift Up",    "Press the upshift paddle or button.",                       Sw,  int(SwitchId::GearUp) },
		{ "Shift Down",  "Press the downshift paddle or button.",                     Sw,  int(SwitchId::GearDown) },
		{ "Start",       "Press the button you want to use for Start and Pause.",      Sw,  int(SwitchId::Start) },
		{ "Confirm",     "Press the button you want to use to confirm menu choices.",  Sw,  int(SwitchId::A) },
		{ "Back",        "Press the button you want to use to go back.",               Sw,  int(SwitchId::B) },
		{ "Menu Up",     "Press Up on the wheel D-pad/POV, or another menu button.",    Sw,  int(SwitchId::SelectionUp) },
		{ "Menu Right",  "Press Right on the wheel D-pad/POV, or another menu button.", Sw,  int(SwitchId::SelectionRight) },
		{ "Menu Down",   "Press Down on the wheel D-pad/POV, or another menu button.",  Sw,  int(SwitchId::SelectionDown) },
		{ "Menu Left",   "Press Left on the wheel D-pad/POV, or another menu button.",  Sw,  int(SwitchId::SelectionLeft) },
	};

	// How far an analog action has to move from rest before its name lights up.
	// Digital actions use the game's own threshold instead, via
	// InputState::isPressed.
	static constexpr float ActiveThreshold = 0.25f;

	Selection selected{ Selection::Kind::Volume, int(ADChannel::Steering) };

	// What the listening prompt will write into. -1 appends a binding instead of
	// replacing one.
	Selection bindTarget;
	int bindIndex = -1;
	std::string bindingName;
	std::unordered_map<SDL_JoystickID, std::vector<Sint16>> axisBaseline;

	// Track binding changes (options tab are handled differently)
	bool unsavedChanges = false;
	bool confirmingReset = false;
	bool confirmingLoad = false;
	std::string persistenceStatus;
	std::vector<std::string> wheelProfiles;
	int selectedWheelProfile = -1;
	char wheelProfileName[65]{};
	bool wheelProfilesLoaded = false;
	bool confirmingProfileLoad = false;
	bool confirmingProfileDelete = false;
	bool confirmingProfileOverwrite = false;
	bool calibrationOpen = false;
	Selection calibrationTarget;
	int calibrationIndex = -1;
	int calibrationRest = 0;
	int calibrationMinimum = 0;
	int calibrationMaximum = 0;
	bool quickSetupActive = false;
	bool quickSetupComplete = false;
	int quickSetupStep = 0;
	bool quickSetupPreviousUnsaved = false;
	std::vector<std::pair<Selection, std::vector<InputBinding>>> quickSetupBackup;
	std::optional<InputBinding> quickSetupCandidate;
	std::optional<InputBinding> releaseGuardBinding;
	bool quickSetupTimedOut = false;
	std::chrono::steady_clock::time_point quickSetupCaptureDeadline{};
	static constexpr auto QuickSetupCaptureTime = std::chrono::seconds(6);

	std::vector<Settings::SettingBase*> pendingSettings;

	static InputAction& action_for(const Selection& selection)
	{
		return InputManager::instance.actionFor(selection.kind, selection.index);
	}

	static const std::string& name_for(const Selection& selection)
	{
		return InputManager::actionName(selection.kind, selection.index);
	}

	// Steering reads as a signed axis, and its bindings are named differently
	// because of it (left/right rather than negated/not).
	static bool is_steering(const Selection& selection)
	{
		return selection.isVolume() && selection.index == int(ADChannel::Steering);
	}

	// Quick Setup replaces only the source it just captured. For raw SDL
	// devices, source family alone is too broad: a wheel, pedal set, shifter,
	// button box and mapped gamepad may all be Joy* bindings at the same time.
	// Resolve both bindings to their current physical SDL instance and replace
	// only bindings from that same device. Disconnected/unresolved bindings are
	// deliberately preserved rather than guessed away.
	static bool same_control(const InputBinding& a, const InputBinding& b)
	{
		if (a.kind != b.kind)
			return false;
		switch (a.kind)
		{
		case InputBinding::Kind::Key: return a.key == b.key;
		case InputBinding::Kind::PadButton: return a.button == b.button;
		case InputBinding::Kind::PadAxis: return a.axis == b.axis && a.negate == b.negate;
		case InputBinding::Kind::JoyButton:
		case InputBinding::Kind::JoyAxis:
		case InputBinding::Kind::JoyHat:
		{
			const auto* da = InputManager::instance.deviceForBinding(a);
			const auto* db = InputManager::instance.deviceForBinding(b);
			const bool sameDevice = da && db
				? da->instanceId == db->instanceId
				: a.deviceGuid == b.deviceGuid && a.deviceOccurrence == b.deviceOccurrence;
			return sameDevice && a.controlIndex == b.controlIndex &&
				(a.kind != InputBinding::Kind::JoyHat || a.hatMask == b.hatMask);
		}
		default: return false;
		}
	}

	static std::string binding_conflicts(const InputBinding& candidate, const Selection& target)
	{
		std::string result;
		for (const ActionListEntry& entry : ActionList)
		{
			const Selection other{ entry.kind, entry.index };
			if (other == target)
				continue;
			const auto& bindings = action_for(other).bindings();
			if (std::any_of(bindings.begin(), bindings.end(), [&](const InputBinding& b)
				{ return same_control(b, candidate); }))
			{
				if (!result.empty()) result += ", ";
				result += name_for(other);
			}
		}
		return result;
	}

	static bool same_source_family(const InputBinding& existing, const InputBinding& candidate)
	{
		if (candidate.isRawDevice())
		{
			if (!existing.isRawDevice())
				return false;
			const auto* existingDevice = InputManager::instance.deviceForBinding(existing);
			const auto* candidateDevice = InputManager::instance.deviceForBinding(candidate);
			return existingDevice && candidateDevice &&
				existingDevice->instanceId == candidateDevice->instanceId;
		}
		if (candidate.isGamepad()) return existing.isGamepad();
		if (candidate.isKeyboard()) return existing.isKeyboard();
		return false;
	}

	void begin_listening(const Selection& target, int index)
	{
		// Waits for the click that got here to be let go of first, otherwise it
		// binds the mouse button or whatever key triggered it.
		isListeningForInput = ListenState::WaitForButtonRelease;
		bindTarget = target;
		bindIndex = index;
		bindingName = name_for(target);
		axisBaseline.clear();
		releaseGuardBinding.reset();
		if (quickSetupActive)
		{
			quickSetupCandidate.reset();
			quickSetupTimedOut = false;
		}
		for (const auto& device : InputManager::instance.devices)
		{
			auto& baseline = axisBaseline[device.instanceId];
			const int axisCount = SDL_GetNumJoystickAxes(device.joystick);
			baseline.reserve(axisCount);
			for (int axis = 0; axis < axisCount; ++axis)
				baseline.push_back(SDL_GetJoystickAxis(device.joystick, axis));
		}
	}

	void skip_quick_setup_step()
	{
		if (!quickSetupActive || quickSetupStep >= int(std::size(QuickSetupSteps)))
			return;

		++quickSetupStep;
		quickSetupCandidate.reset();
		releaseGuardBinding.reset();
		quickSetupTimedOut = false;
		ImGui::CloseCurrentPopup();

		if (quickSetupStep < int(std::size(QuickSetupSteps)))
			begin_listening(quick_setup_selection(quickSetupStep), -1);
		else
		{
			quickSetupActive = false;
			quickSetupComplete = true;
			isListeningForInput = ListenState::False;
		}
	}

	static Selection quick_setup_selection(int step)
	{
		const auto& entry = QuickSetupSteps[step];
		return Selection{ entry.kind, entry.index };
	}

	void restore_quick_setup_backup()
	{
		for (auto& [selection, bindings] : quickSetupBackup)
			action_for(selection).bindings() = bindings;
		quickSetupBackup.clear();
		quickSetupActive = false;
		quickSetupComplete = false;
		quickSetupStep = 0;
		quickSetupCandidate.reset();
		releaseGuardBinding.reset();
		quickSetupTimedOut = false;
		unsavedChanges = quickSetupPreviousUnsaved;
	}

	void start_quick_setup()
	{
		quickSetupBackup.clear();
		confirmingLoad = false;
		persistenceStatus.clear();
		quickSetupPreviousUnsaved = unsavedChanges;
		for (int i = 0; i < int(std::size(QuickSetupSteps)); ++i)
		{
			const Selection selection = quick_setup_selection(i);
			if (std::find_if(quickSetupBackup.begin(), quickSetupBackup.end(), [&selection](const auto& saved)
				{
					return saved.first == selection;
				}) == quickSetupBackup.end())
				quickSetupBackup.emplace_back(selection, action_for(selection).bindings());
		}
		quickSetupStep = 0;
		quickSetupComplete = false;
		quickSetupActive = true;
		begin_listening(quick_setup_selection(quickSetupStep), -1);
	}

public:
	void init() override {}
	static InputBinding with_device_identity(InputBinding binding, const InputManager::InputDevice& device)
	{
		binding.deviceVendor = device.vendor;
		binding.deviceProduct = device.product;
		binding.deviceSerial = device.serial;
		binding.devicePath = device.path;
		return binding;
	}

	void arm_release_guard(const InputBinding& binding)
	{
		releaseGuardBinding = binding;
		if (releaseGuardBinding->kind == InputBinding::Kind::JoyAxis &&
			releaseGuardBinding->axisMinimum == releaseGuardBinding->axisRest &&
			releaseGuardBinding->axisMaximum == releaseGuardBinding->axisRest)
		{
			// Quick Setup stores a zero-span axis until guided calibration. The
			// release guard is transient only, so restore the raw SDL range here
			// to make read() wait for the physical control to return near rest.
			releaseGuardBinding->axisMinimum = -32768;
			releaseGuardBinding->axisMaximum = 32767;
		}
	}

	//
	// Listens for any input at all rather than being told up front whether to
	// expect a key or a pad: the binding records which it was. Escape cancels
	// and Delete clears, in every case, so there is no separate keyboard timeout
	// and the two halves behave the same.
	//
	bool HandleNewBinding()
	{
		InputAction& action = action_for(bindTarget);
		auto& bindings = action.bindings();

		if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			if (quickSetupActive)
				restore_quick_setup_backup();
			isListeningForInput = ListenState::False;
			ImGui::CloseCurrentPopup();
			return false;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
		{
			if (quickSetupActive)
			{
				restore_quick_setup_backup();
				isListeningForInput = ListenState::False;
				ImGui::CloseCurrentPopup();
				return false;
			}
			const bool removed = bindIndex >= 0 && bindIndex < int(bindings.size());
			if (removed)
				bindings.erase(bindings.begin() + bindIndex);

			isListeningForInput = ListenState::False;
			ImGui::CloseCurrentPopup();
			return removed;
		}

		// Replaces the binding that was clicked, or appends when the add button
		// was. Appending is the whole point of the list, so nothing here clears
		// what is already bound.
		const auto commit = [&](const InputBinding& binding)
		{
			InputBinding prepared = binding;
			if (quickSetupActive && prepared.kind == InputBinding::Kind::JoyAxis)
			{
				prepared.axisMinimum = prepared.axisRest;
				prepared.axisMaximum = prepared.axisRest;
			}

			const std::string conflicts = binding_conflicts(prepared, bindTarget);
			if (!conflicts.empty())
				persistenceStatus = "Note: this control is also bound to " + conflicts + ".";
			if (quickSetupActive)
			{
				quickSetupCandidate = prepared;
				return;
			}
			else if (bindIndex >= 0 && bindIndex < int(bindings.size()))
				bindings[bindIndex] = prepared;
			else
				action.add(prepared);

			arm_release_guard(prepared);
			isListeningForInput = ListenState::WaitForBindButtonRelease;
			ImGui::CloseCurrentPopup();
		};

		if (quickSetupCandidate || quickSetupTimedOut)
			return false;

		// Keyboard
		{
			int keyCount = 0;
			const bool* keyState = SDL_GetKeyboardState(&keyCount);
			for (int i = 0; i < keyCount; i++)
			{
				// Reserved above, so they can't be bound to anything.
				if (i == SDL_SCANCODE_ESCAPE || i == SDL_SCANCODE_DELETE || i == SDL_SCANCODE_BACKSPACE)
					continue;

				if (keyState[i])
				{
					commit(InputBinding(static_cast<SDL_Scancode>(i)));
					return true;
				}
			}
		}

		// Controller
		for (const auto& device : InputManager::instance.devices)
		{
			for (int button = 0; button < SDL_GetNumJoystickButtons(device.joystick); ++button)
				if (SDL_GetJoystickButton(device.joystick, button))
				{
					commit(with_device_identity(
						InputBinding::joystickButton(device.guid, device.occurrence, button), device));
					return true;
				}

			for (int hat = 0; hat < SDL_GetNumJoystickHats(device.joystick); ++hat)
			{
				const Uint8 value = SDL_GetJoystickHat(device.joystick, hat);
				if (value != SDL_HAT_CENTERED)
				{
					commit(with_device_identity(
						InputBinding::joystickHat(device.guid, device.occurrence, hat, value), device));
					return true;
				}
			}

			const auto baselineIt = axisBaseline.find(device.instanceId);
			if (baselineIt == axisBaseline.end())
				continue;
			for (int axis = 0; axis < SDL_GetNumJoystickAxes(device.joystick) && axis < int(baselineIt->second.size()); ++axis)
			{
				const Sint16 current = SDL_GetJoystickAxis(device.joystick, axis);
				const int delta = int(current) - int(baselineIt->second[axis]);
				if (std::abs(delta) > 16384)
				{
					const auto mode = is_steering(bindTarget)
						? InputBinding::AxisMode::Signed : InputBinding::AxisMode::FromRest;
					commit(with_device_identity(
						InputBinding::joystickAxis(device.guid, device.occurrence, axis, false,
							mode, baselineIt->second[axis], delta > 0), device));
					return true;
				}
			}
		}

		// Legacy gamepad capture remains as a fallback when SDL exposes no raw
		// joystick handle for the selected gamepad.
		if (auto* controller = InputManager::instance.getPrimaryGamepad())
		{
			for (int i = SDL_GAMEPAD_BUTTON_SOUTH; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
			{
				if (SDL_GetGamepadButton(controller, static_cast<SDL_GamepadButton>(i)))
				{
					commit(InputBinding(static_cast<SDL_GamepadButton>(i)));
					return true;
				}
			}

			for (int i = SDL_GAMEPAD_AXIS_LEFTX; i < SDL_GAMEPAD_AXIS_COUNT; i++)
			{
				const float value = SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(i)) / 32768.0f;
				if (std::abs(value) > 0.5f)
				{
					// The direction it was pushed becomes the binding's, which
					// the invert toggle can flip afterwards.
					commit(InputBinding(static_cast<SDL_GamepadAxis>(i), value < 0));
					return true;
				}
			}
		}

		return false;
	}

private:
	void begin_calibration(const Selection& target, int index)
	{
		auto& bindings = action_for(target).bindings();
		if (index < 0 || index >= int(bindings.size()) || bindings[index].kind != InputBinding::Kind::JoyAxis)
			return;
		auto* joystick = InputManager::instance.joystickForBinding(bindings[index]);
		if (!joystick)
			return;

		calibrationTarget = target;
		calibrationIndex = index;
		calibrationRest = SDL_GetJoystickAxis(joystick, bindings[index].controlIndex);
		calibrationMinimum = calibrationRest;
		calibrationMaximum = calibrationRest;
		calibrationOpen = true;
	}

	void draw_calibration_popup()
	{
		if (!calibrationOpen)
			return;
		ImGui::OpenPopup("Calibrate axis");
		if (!ImGui::BeginPopupModal("Calibrate axis", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;

		auto& bindings = action_for(calibrationTarget).bindings();
		if (calibrationIndex < 0 || calibrationIndex >= int(bindings.size()))
		{
			calibrationOpen = false;
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		auto& binding = bindings[calibrationIndex];
		auto* joystick = InputManager::instance.joystickForBinding(binding);
		if (!joystick)
		{
			ImGui::TextWrapped("Reconnect this device to continue calibration.");
		}
		else
		{
			const int current = SDL_GetJoystickAxis(joystick, binding.controlIndex);
			calibrationMinimum = (std::min)(calibrationMinimum, current);
			calibrationMaximum = (std::max)(calibrationMaximum, current);

			ImGui::TextWrapped(binding.axisMode == InputBinding::AxisMode::Signed
				? "Leave the wheel centered and set its center. Then turn fully left and fully right."
				: "Release the pedal and set its resting position. Then press it fully and release it.");
			ImGui::Spacing();
			ImGui::ProgressBar((current + 32768.0f) / 65535.0f, ImVec2(320.0f, 0),
				std::format("Current: {}", current).c_str());
			ImGui::TextDisabled("Detected range: %d to %d", calibrationMinimum, calibrationMaximum);

			if (ImGui::Button(binding.axisMode == InputBinding::AxisMode::Signed ? "Set center" : "Set resting position"))
			{
				calibrationRest = current;
				calibrationMinimum = current;
				calibrationMaximum = current;
			}

			const int negativeTravel = calibrationRest - calibrationMinimum;
			const int positiveTravel = calibrationMaximum - calibrationRest;
			const bool enoughTravel = binding.axisMode == InputBinding::AxisMode::Signed
				? negativeTravel > 4096 && positiveTravel > 4096
				: (std::max)(negativeTravel, positiveTravel) > 4096;

			ImGui::SameLine();
			if (!enoughTravel)
				ImGui::BeginDisabled();
			if (ImGui::Button("Save calibration"))
			{
				binding.axisMinimum = calibrationMinimum;
				binding.axisRest = calibrationRest;
				binding.axisMaximum = calibrationMaximum;
				binding.axisPositive = positiveTravel >= negativeTravel;
				unsavedChanges = true;
				calibrationOpen = false;
				ImGui::CloseCurrentPopup();
			}
			if (!enoughTravel)
				ImGui::EndDisabled();
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			calibrationOpen = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	// Left pane: every action, grouped, with the ones currently reading input
	// picked out. Watching a name light up is how a binding gets verified
	// without leaving the screen.
	void draw_action_list()
	{
		const ImVec4 activeColour = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];

		const char* group = nullptr;
		for (const ActionListEntry& entry : ActionList)
		{
			if (!group || std::strcmp(group, entry.group) != 0)
			{
				group = entry.group;
				ImGui::SeparatorText(group);
			}

			const Selection action{ entry.kind, entry.index };
			const InputState& state = action_for(action).getState();

			// A binding's value keeps its sign, and negating a binding flips it,
			// so an axis bound to two opposing digital actions gives one of them
			// a positive value and the other a negative one. Only the positive
			// side fires, which is what isPressed tests for - taking the
			// magnitude instead would light up both ends of the same stick.
			// An analog action uses the sign for direction rather than for on
			// and off, so there either end of the range counts as movement.
			const bool active = action.isVolume()
				? std::abs(state.currentValue) >= ActiveThreshold
				: state.isPressed();

			if (active)
				ImGui::PushStyleColor(ImGuiCol_Text, activeColour);

			if (ImGui::Selectable(name_for(action).c_str(), selected == action))
				selected = action;

			if (active)
				ImGui::PopStyleColor();
		}
	}

	// Right pane: the selected action's bindings, one row each.
	void draw_binding_editor(SDL_GamepadType padType)
	{
		InputAction& action = action_for(selected);
		auto& bindings = action.bindings();
		const bool steering = is_steering(selected);

		ImGui::SeparatorText(name_for(selected).c_str());

		int removeIndex = -1;

		if (ImGui::BeginTable("##bindings", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##calibrate", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("##invert", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed);

			for (int i = 0; i < int(bindings.size()); i++)
			{
				InputBinding& binding = bindings[i];

				ImGui::PushID(i);
				ImGui::TableNextRow();

				// The binding's own name is the rebind button, so there is no
				// separate control for the most common thing to want.
				ImGui::TableNextColumn();
				std::string sourceName = binding.isKeyboard() ? "Keyboard" : "Gamepad";
				if (binding.isRawDevice())
				{
					if (const auto* device = InputManager::instance.deviceForBinding(binding))
					{
						const char* name = SDL_GetJoystickName(device->joystick);
						sourceName = name && name[0] ? name : "USB device";
						if (InputManager::instance.deviceMatchCount(binding) > 1)
							sourceName += " - choose device";
					}
					else
						sourceName = "Reconnect device";
				}
				const std::string label = std::format("{}  ({})",
					binding.displayName(padType, steering), sourceName);

				const bool listeningHere = isListeningForInput != ListenState::False &&
					!quickSetupActive && bindTarget == selected && bindIndex == i;
				if (listeningHere)
				{
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Text]);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
				}
				if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0)))
					begin_listening(selected, i);
				if (listeningHere)
					ImGui::PopStyleColor(2);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Rebind this input");

				ImGui::TableNextColumn();
				if (binding.kind == InputBinding::Kind::JoyAxis)
				{
					if (ImGui::Button("Calibrate"))
						begin_calibration(selected, i);
				}

				// Inverting is the only way to reach the '-' suffix the INI
				// format has always had: it sends an analog action the opposite
				// direction, and makes a digital action fire on negative input.
				ImGui::TableNextColumn();
				if (binding.kind == InputBinding::Kind::JoyAxis && binding.axisMode == InputBinding::AxisMode::FromRest)
				{
					ImGui::TextDisabled("Auto");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Pedal direction is detected automatically during calibration");
				}
				else if (steering || binding.isAxis())
				{
					if (ImGui::Checkbox("##invert", &binding.negate))
						unsavedChanges = true;
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(steering
							? "Steer the other way with this input"
							: "Use the opposite side of this axis");
				}
				else
				{
					ImGui::TextDisabled("-");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Invert is only meaningful for axes or steering-direction buttons");
				}

				ImGui::TableNextColumn();
				if (ImGui::Button("X"))
					removeIndex = i;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Remove this binding");

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		if (removeIndex >= 0)
		{
			bindings.erase(bindings.begin() + removeIndex);
			unsavedChanges = true;
		}

		if (bindings.empty())
			ImGui::TextDisabled("Nothing bound.");

		const bool listeningForNew = isListeningForInput != ListenState::False &&
			!quickSetupActive && bindTarget == selected && bindIndex == -1;
		if (listeningForNew)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Text]);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
		}
		if (ImGui::Button(listeningForNew ? "LISTENING...##addBinding" : "+ Add binding"))
			begin_listening(selected, -1);
		if (listeningForNew)
			ImGui::PopStyleColor(2);

		// Live value of the selected action.
		const float value = action.getState().currentValue;
		const float filled = selected.isVolume() ? std::abs(value) : value;

		ImGui::Spacing();
		ImGui::TextDisabled("Reading");
		ImGui::ProgressBar(std::clamp(filled, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0),
			std::format("{:.2f}", value).c_str());

	}

	void draw_controllers()
	{
		auto& manager = InputManager::instance;

		if (manager.devices.empty())
		{
			ImGui::TextDisabled("No input devices detected.");
			ImGui::Spacing();
			ImGui::TextWrapped("Connect a controller, wheel, pedal set or shifter. Devices appear here automatically.");
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::TextDisabled("Multi-device input architecture adapted from hyp36rmax (MIT)");
			return;
		}

		ImGui::Text("%d input device%s detected", int(manager.devices.size()), manager.devices.size() == 1 ? "" : "s");
		ImGui::TextDisabled("Move a control to verify that OutRun can see it.");
		ImGui::Spacing();

		for (const auto& device : manager.devices)
		{
			SDL_Joystick* joystick = device.joystick;
			const char* deviceName = SDL_GetJoystickName(joystick);
			if (!deviceName || !deviceName[0])
				deviceName = "Unknown input device";

			ImGui::PushID(int(device.instanceId));
			if (ImGui::TreeNodeEx("##device", ImGuiTreeNodeFlags_DefaultOpen,
				"%s  [%s]", deviceName, device.isGamepad ? "Gamepad" : "USB device"))
			{
				const int axisCount = SDL_GetNumJoystickAxes(joystick);
				const int buttonCount = SDL_GetNumJoystickButtons(joystick);
				const int hatCount = SDL_GetNumJoystickHats(joystick);
				ImGui::TextDisabled("%d axes  |  %d buttons  |  %d hats", axisCount, buttonCount, hatCount);

				for (int axis = 0; axis < axisCount; ++axis)
				{
					const float value = SDL_GetJoystickAxis(joystick, axis) / 32768.0f;
					ImGui::Text("Axis %d", axis + 1);
					ImGui::SameLine();
					ImGui::ProgressBar((value + 1.0f) * 0.5f, ImVec2(-FLT_MIN, 0),
						std::format("{:.2f}", value).c_str());
				}

				bool anyButton = false;
				for (int button = 0; button < buttonCount; ++button)
					if (SDL_GetJoystickButton(joystick, button))
					{
						if (!anyButton)
							ImGui::Text("Pressed:");
						ImGui::SameLine();
						ImGui::Text("%d", button + 1);
						anyButton = true;
					}
				if (!anyButton && buttonCount > 0)
					ImGui::TextDisabled("Press a button to test it");

				for (int hat = 0; hat < hatCount; ++hat)
					ImGui::Text("Hat %d: 0x%02X", hat + 1, SDL_GetJoystickHat(joystick, hat));

				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextDisabled("Multi-device input architecture adapted from hyp36rmax (MIT)");
	}

	// These are tweaks settings rather than bindings, so they go to the tweaks INI
	// the moment they are changed, the way the settings window writes them. Save
	// and its unsaved marker stay about bindings alone.
	void setting_changed(Settings::SettingBase& setting)
	{
		if (std::find(pendingSettings.begin(), pendingSettings.end(), &setting) == pendingSettings.end())
			pendingSettings.emplace_back(&setting);
	}

	// Held back until the control being dragged is let go of, so a slider doesn't
	// write the INI on every frame of the drag.
	void flush_settings()
	{
		if (pendingSettings.empty() || ImGui::IsAnyItemActive())
			return;

		for (Settings::SettingBase* setting : pendingSettings)
			setting->notify();
		pendingSettings.clear();

		Settings::write(Module::UserIniPath);
	}


	void refresh_wheel_profiles(const std::string& selectName = {})
	{
		wheelProfiles = WheelProfileStore::list_profiles(WheelProfileStore::Kind::Input);
		selectedWheelProfile = -1;
		const std::string wanted = WheelProfileStore::normalize_profile_name(selectName);
		if (!wanted.empty())
		{
			for (size_t i = 0; i < wheelProfiles.size(); ++i)
				if (WheelProfileStore::lower_ascii(wheelProfiles[i]) == WheelProfileStore::lower_ascii(wanted))
				{
					selectedWheelProfile = int(i);
					break;
				}
		}
		if (selectedWheelProfile >= 0)
			strncpy_s(wheelProfileName, wheelProfiles[selectedWheelProfile].c_str(), sizeof(wheelProfileName) - 1);
		wheelProfilesLoaded = true;
	}

	const std::string* selected_wheel_profile() const
	{
		return selectedWheelProfile >= 0 && selectedWheelProfile < int(wheelProfiles.size())
			? &wheelProfiles[selectedWheelProfile] : nullptr;
	}

	bool wheel_profile_exists_cached(const std::string& name) const
	{
		const std::string wanted = WheelProfileStore::lower_ascii(name);
		return std::any_of(wheelProfiles.begin(), wheelProfiles.end(), [&](const std::string& profile)
		{
			return WheelProfileStore::lower_ascii(profile) == wanted;
		});
	}

	void draw_profiles()
	{
		auto& manager = InputManager::instance;
		if (!wheelProfilesLoaded)
			refresh_wheel_profiles();

		ImGui::TextWrapped(
			"Named wheel profiles store the complete multi-device binding set, so a wheel can stay paired with its pedals, shifter and button box. Steering deadzone, sensitivity bypass and input backend are stored with the profile too.");
		ImGui::TextDisabled("Loading a profile also updates OutRun2006Tweaks.input.ini, so it remains active after restart.");
		ImGui::Spacing();

		const std::string* selectedProfile = selected_wheel_profile();
		const char* preview = selectedProfile ? selectedProfile->c_str() : "Select a saved wheel profile";
		if (ImGui::BeginCombo("Saved wheel profile", preview))
		{
			for (size_t i = 0; i < wheelProfiles.size(); ++i)
			{
				const bool selectedNow = int(i) == selectedWheelProfile;
				if (ImGui::Selectable(wheelProfiles[i].c_str(), selectedNow))
				{
					selectedWheelProfile = int(i);
					strncpy_s(wheelProfileName, wheelProfiles[i].c_str(), sizeof(wheelProfileName) - 1);
					confirmingProfileLoad = false;
					confirmingProfileDelete = false;
					confirmingProfileOverwrite = false;
				}
				if (selectedNow)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		if (ImGui::InputText("Profile name", wheelProfileName, sizeof(wheelProfileName)))
			confirmingProfileOverwrite = false;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Letters, numbers, spaces, '.', '_' and '-' are supported. '.ini' is optional.");

		const std::string requestedName = WheelProfileStore::normalize_profile_name(wheelProfileName);
		const bool profileAlreadyExists =
			!requestedName.empty() && wheel_profile_exists_cached(requestedName);
		const char* saveLabel = confirmingProfileOverwrite
			? "Confirm overwrite##inputProfileSave"
			: (profileAlreadyExists ? "Overwrite profile##inputProfileSave" : "Save as profile##inputProfileSave");
		if (ImGui::Button(saveLabel))
		{
			std::string error;
			const bool existsOnDisk = !requestedName.empty() &&
				WheelProfileStore::profile_exists(WheelProfileStore::Kind::Input, requestedName);
			if (existsOnDisk && !confirmingProfileOverwrite)
			{
				confirmingProfileOverwrite = true;
				persistenceStatus = "Click Confirm overwrite to replace the existing wheel profile.";
			}
			else if (auto profilePath = WheelProfileStore::profile_path(
				WheelProfileStore::Kind::Input, requestedName, &error))
			{
				const auto stagedPath = WheelProfileStore::staged_profile_path(*profilePath);
				{
					std::error_code ignored;
					std::filesystem::remove(stagedPath, ignored);
				}
				if (!manager.saveBindingIni(stagedPath))
				{
					persistenceStatus = "Could not stage wheel profile bindings.";
				}
				else if (!WheelProfileStore::append_input_options(stagedPath, &error))
				{
					std::error_code ignored;
					std::filesystem::remove(stagedPath, ignored);
					persistenceStatus = error;
				}
				else if (!WheelProfileStore::commit_staged_profile(stagedPath, *profilePath, &error))
				{
					std::error_code ignored;
					std::filesystem::remove(stagedPath, ignored);
					persistenceStatus = error;
				}
				else
				{
					const bool currentSaved = manager.saveBindingIni(Module::BindingsIniPath);
					const bool optionsSaved = Settings::write(Module::UserIniPath);
					unsavedChanges = !currentSaved;
					persistenceStatus = currentSaved && optionsSaved
						? "Wheel profile saved and made current: " + requestedName
						: "Wheel profile saved, but part of the current configuration could not be persisted.";
					confirmingProfileOverwrite = false;
					refresh_wheel_profiles(requestedName);
				}
			}
			else
				persistenceStatus = error;
		}

		ImGui::SameLine();
		const bool canLoadProfile = selected_wheel_profile() != nullptr;
		if (!canLoadProfile) ImGui::BeginDisabled();
		const char* loadLabel = unsavedChanges && confirmingProfileLoad
			? "Discard edits & load profile?##inputProfileLoad" : "Load selected##inputProfileLoad";
		if (ImGui::Button(loadLabel))
		{
			if (unsavedChanges && !confirmingProfileLoad)
			{
				confirmingProfileLoad = true;
				persistenceStatus = "Click again to discard unsaved binding edits and load the selected wheel profile.";
			}
			else if (const std::string* selected = selected_wheel_profile())
			{
				std::string error;
				auto profilePath = WheelProfileStore::profile_path(WheelProfileStore::Kind::Input, *selected, &error);
				const auto optionBackup = WheelProfileStore::capture_input_options();
				if (!profilePath || !WheelProfileStore::load_input_options(*profilePath, &error))
				{
					persistenceStatus = error.empty() ? "Could not load wheel profile options." : error;
				}
				else if (!manager.readBindingIni(*profilePath))
				{
					WheelProfileStore::restore_input_options(optionBackup);
					persistenceStatus = "Could not load wheel profile bindings; current bindings were preserved.";
				}
				else
				{
					const bool currentSaved = manager.saveBindingIni(Module::BindingsIniPath);
					const bool optionsSaved = Settings::write(Module::UserIniPath);
					WheelFFB_ResetDirectionTest();
					unsavedChanges = !currentSaved;
					persistenceStatus = currentSaved && optionsSaved
						? "Wheel profile loaded and made current: " + *selected
						: "Wheel profile is active now, but part of it could not be persisted for restart.";
					if (Settings::InputBackend.changed_since_startup())
						persistenceStatus += " Restart the game to apply its input backend.";
				}
				confirmingProfileLoad = false;
			}
		}
		if (!canLoadProfile) ImGui::EndDisabled();

		ImGui::SameLine();
		if (!canLoadProfile) ImGui::BeginDisabled();
		const char* deleteLabel = confirmingProfileDelete
			? "Confirm delete##inputProfileDelete" : "Delete selected##inputProfileDelete";
		if (ImGui::Button(deleteLabel))
		{
			if (!confirmingProfileDelete)
			{
				confirmingProfileDelete = true;
				persistenceStatus = "Click Confirm delete to remove the selected profile file. Current bindings will not change.";
			}
			else if (const std::string* selected = selected_wheel_profile())
			{
				std::string error;
				if (WheelProfileStore::delete_profile(WheelProfileStore::Kind::Input, *selected, &error))
				{
					persistenceStatus = "Deleted wheel profile: " + *selected;
					wheelProfileName[0] = '\0';
					refresh_wheel_profiles();
				}
				else
					persistenceStatus = error;
				confirmingProfileDelete = false;
			}
		}
		if (!canLoadProfile) ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::Button("Refresh profile list"))
			refresh_wheel_profiles(selected_wheel_profile() ? *selected_wheel_profile() : std::string{});

		ImGui::Spacing();
		ImGui::TextDisabled("Profile folder: OutRun2006Tweaks.profiles\\Input");
	}

	void draw_options()
	{
		auto& manager = InputManager::instance;

		ImGui::TextUnformatted("Controller compatibility");
		const char* inputBackends[] = { "Automatic (recommended)", "Raw Input", "DirectInput (wheels)", "XInput" };
		if (ImGui::Combo("Input backend", Settings::InputBackend.ptr(), inputBackends, IM_ARRAYSIZE(inputBackends)))
			setting_changed(Settings::InputBackend);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Use DirectInput when a wheel is listed but its axes or buttons do not respond.");
		ImGui::TextDisabled("Restart the game after changing the input backend.");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const char* vibrationModes[] = { "Disabled", "Enabled", "Swap L/R", "Merge L/R" };
		if (ImGui::Combo("Vibration Mode", Settings::VibrationMode.ptr(), vibrationModes, IM_ARRAYSIZE(vibrationModes)))
			setting_changed(Settings::VibrationMode);
		if (ImGui::SliderInt("Vibration Strength", Settings::VibrationStrength.ptr(), 0, 10))
			setting_changed(Settings::VibrationStrength);
		if (ImGui::Combo("Impulse Vibration", Settings::ImpulseVibrationMode.ptr(), vibrationModes, IM_ARRAYSIZE(vibrationModes)))
			setting_changed(Settings::ImpulseVibrationMode);

		int deadzonePercent = int(Settings::SteeringDeadZone * 100.f);
		if (ImGui::SliderInt("Steering Deadzone", &deadzonePercent, 0, 20, "%d%%"))
		{
			Settings::SteeringDeadZone = float(deadzonePercent) / 100.f;
			setting_changed(Settings::SteeringDeadZone);
		}

		if (ImGui::Checkbox("Bypass Sensitivity", Settings::BypassGameSensitivity.ptr()))
			setting_changed(Settings::BypassGameSensitivity);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Passes steering input to the game directly, allows for more sensitive controls.\n"
				"Only used when UseNewInput is enabled.");
	}

	void draw_listening_popup()
	{
		if (isListeningForInput == ListenState::False)
			return;

		auto& manager = InputManager::instance;

		if (isListeningForInput == ListenState::WaitForButtonRelease)
		{
			if (!manager.anyInputPressed())
			{
				isListeningForInput = ListenState::Listening;
				if (quickSetupActive)
					quickSetupCaptureDeadline = std::chrono::steady_clock::now() + QuickSetupCaptureTime;
			}
			return;
		}

		if (isListeningForInput == ListenState::WaitForBindButtonRelease)
		{
			bool capturedReleased = true;
			if (releaseGuardBinding)
			{
				const auto resolveJoystick = [&manager](const InputBinding& binding)
					{
						return manager.joystickForBinding(binding);
					};
				capturedReleased = std::abs(releaseGuardBinding->read(
					manager.getPrimaryGamepad(), resolveJoystick)) < 0.25f;
			}

			if (capturedReleased && !manager.anyInputPressed())
			{
				releaseGuardBinding.reset();
				if (quickSetupActive && quickSetupStep < int(std::size(QuickSetupSteps)))
					begin_listening(quick_setup_selection(quickSetupStep), -1);
				else
				{
					isListeningForInput = ListenState::False;
					if (quickSetupActive)
					{
						quickSetupActive = false;
						quickSetupComplete = true;
					}
				}
			}
			return;
		}

		ImGui::OpenPopup("Listening for Input");
		if (ImGui::BeginPopupModal("Listening for Input", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			if (quickSetupActive && quickSetupStep < int(std::size(QuickSetupSteps)))
			{
				const auto& step = QuickSetupSteps[quickSetupStep];
				ImGui::TextDisabled("Quick Setup  |  Step %d of %d", quickSetupStep + 1, int(std::size(QuickSetupSteps)));
				ImGui::SeparatorText(step.title);
				ImGui::TextWrapped("%s", step.prompt);
				if (!quickSetupCandidate && !quickSetupTimedOut)
				{
					const auto remaining = std::chrono::duration<float>(quickSetupCaptureDeadline - std::chrono::steady_clock::now()).count();
					if (remaining <= 0.f)
						quickSetupTimedOut = true;
					else
					{
						ImGui::ProgressBar(remaining / 6.f, ImVec2(320.f, 0.f), std::format("{:.1f} seconds", remaining).c_str());
						ImGui::TextDisabled("Only the first deliberate input will be proposed.");
					}
				}
			}
			else
				ImGui::Text("Press any input to bind to %s", bindingName.c_str());
			ImGui::Spacing();
			if (quickSetupActive && quickSetupCandidate)
			{
				ImGui::SeparatorText("Confirm input");
				ImGui::Text("Detected: %s", quickSetupCandidate->displayName().c_str());
				if (const auto* device = manager.deviceForBinding(*quickSetupCandidate))
					ImGui::TextDisabled("Device: %s", SDL_GetJoystickName(device->joystick));
				const std::string conflicts = binding_conflicts(*quickSetupCandidate, bindTarget);
				if (!conflicts.empty())
					ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f),
						"Also bound to: %s", conflicts.c_str());
				ImGui::TextWrapped("Confirm this input before Quick Setup moves to the next control.");
				if (ImGui::Button("Use this input"))
				{
					auto& bindings = action_for(bindTarget).bindings();
					std::erase_if(bindings, [&](const InputBinding& existing)
						{ return same_source_family(existing, *quickSetupCandidate); });
					action_for(bindTarget).add(*quickSetupCandidate);
					arm_release_guard(*quickSetupCandidate);
					quickSetupCandidate.reset();
					++quickSetupStep;
					unsavedChanges = true;
					isListeningForInput = ListenState::WaitForBindButtonRelease;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Try again"))
				{
					arm_release_guard(*quickSetupCandidate);
					quickSetupCandidate.reset();
					isListeningForInput = ListenState::WaitForBindButtonRelease;
					ImGui::CloseCurrentPopup();
				}
			}
			else if (quickSetupActive && quickSetupTimedOut)
			{
				ImGui::TextWrapped("No deliberate input was detected. Retry this control or skip it and keep its existing bindings.");
				if (ImGui::Button("Try this step again"))
				{
					begin_listening(bindTarget, -1);
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Skip this step"))
					skip_quick_setup_step();
			}
			else
			{
				ImGui::TextDisabled(quickSetupActive ? "Escape to cancel Quick Setup" : "Escape to cancel, Delete to clear");
				if (HandleNewBinding())
					unsavedChanges = true;
				if (quickSetupActive && !quickSetupCandidate)
				{
					ImGui::SameLine();
					if (ImGui::Button("Skip this step"))
						skip_quick_setup_step();
				}
			}

			if (quickSetupActive && (quickSetupCandidate || quickSetupTimedOut))
			{
				ImGui::TextDisabled("Escape to cancel Quick Setup");
				if (ImGui::IsKeyPressed(ImGuiKey_Escape))
				{
					restore_quick_setup_backup();
					isListeningForInput = ListenState::False;
					ImGui::CloseCurrentPopup();
				}
			}

			ImGui::EndPopup();
		}
	}

	int first_raw_axis_binding(const Selection& selection) const
	{
		const auto& bindings = action_for(selection).bindings();
		for (int i = 0; i < int(bindings.size()); ++i)
			if (bindings[i].kind == InputBinding::Kind::JoyAxis)
				return i;
		return -1;
	}

	bool axis_calibrated(const Selection& selection) const
	{
		const int index = first_raw_axis_binding(selection);
		if (index < 0)
			return true; // SDL gamepad axes already use normalized platform ranges.
		const auto& binding = action_for(selection).bindings()[index];
		const int negative = binding.axisRest - binding.axisMinimum;
		const int positive = binding.axisMaximum - binding.axisRest;
		return binding.axisMode == InputBinding::AxisMode::Signed
			? negative > 4096 && positive > 4096
			: (std::max)(negative, positive) > 4096;
	}

	void quick_calibration_row(const char* label, const Selection& selection)
	{
		const int index = first_raw_axis_binding(selection);
		if (index < 0)
		{
			ImGui::TextDisabled("%s: platform/gamepad calibration", label);
			return;
		}
		const bool ready = axis_calibrated(selection);
		ImGui::TextColored(ready ? ImVec4(0.35f, 0.90f, 0.45f, 1.0f) : ImVec4(1.0f, 0.70f, 0.20f, 1.0f),
			"%s: %s", label, ready ? "calibrated" : "needs calibration");
		ImGui::SameLine();
		ImGui::PushID(label);
		if (ImGui::SmallButton(ready ? "Recalibrate" : "Calibrate now"))
			begin_calibration(selection, index);
		ImGui::PopID();
	}

	void draw_quick_setup_complete(bool& dialogOpen)
	{
		if (!quickSetupComplete)
			return;
		ImGui::OpenPopup("Test your driving controls");
		if (!ImGui::BeginPopupModal("Test your driving controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;

		ImGui::TextWrapped("Move the wheel, accelerator and brake. If everything responds correctly, save and drive.");
		ImGui::Spacing();
		const float steering = action_for({ Vol, int(ADChannel::Steering) }).getState().currentValue;
		const float accelerator = action_for({ Vol, int(ADChannel::Acceleration) }).getState().currentValue;
		const float brake = action_for({ Vol, int(ADChannel::Brake) }).getState().currentValue;
		ImGui::Text("Steering");
		ImGui::SameLine();
		ImGui::ProgressBar(std::clamp((steering + 1.0f) * 0.5f, 0.0f, 1.0f), ImVec2(280.0f, 0),
			std::format("{:.2f}", steering).c_str());
		ImGui::Text("Accelerator");
		ImGui::SameLine();
		ImGui::ProgressBar(std::clamp(accelerator, 0.0f, 1.0f), ImVec2(280.0f, 0),
			std::format("{:.2f}", accelerator).c_str());
		ImGui::Text("Brake");
		ImGui::SameLine();
		ImGui::ProgressBar(std::clamp(brake, 0.0f, 1.0f), ImVec2(280.0f, 0),
			std::format("{:.2f}", brake).c_str());

		ImGui::SeparatorText("Guided axis calibration");
		ImGui::TextWrapped("Finish wheel center/end-stops and pedal rest/full-travel here before saving. This keeps calibration inside Quick Setup instead of hiding it in the manual editor.");
		quick_calibration_row("Steering", { Vol, int(ADChannel::Steering) });
		quick_calibration_row("Accelerator", { Vol, int(ADChannel::Acceleration) });
		quick_calibration_row("Brake", { Vol, int(ADChannel::Brake) });
		const Selection steeringSelection{ Vol, int(ADChannel::Steering) };
		const Selection acceleratorSelection{ Vol, int(ADChannel::Acceleration) };
		const Selection brakeSelection{ Vol, int(ADChannel::Brake) };
		const bool coreControlsPresent =
			!action_for(steeringSelection).bindings().empty() &&
			!action_for(acceleratorSelection).bindings().empty() &&
			!action_for(brakeSelection).bindings().empty();
		const bool guidedCalibrationReady =
			axis_calibrated(steeringSelection) &&
			axis_calibrated(acceleratorSelection) &&
			axis_calibrated(brakeSelection);
		const bool readyToSaveAndDrive = coreControlsPresent && guidedCalibrationReady;
		if (!coreControlsPresent)
			ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f),
				"Steering, accelerator and brake need bindings before Save & Drive.");
		else if (!guidedCalibrationReady)
			ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f),
				"Calibrate the raw wheel/pedal axes above before Save & Drive.");

		ImGui::Spacing();
		if (!readyToSaveAndDrive) ImGui::BeginDisabled();
		const bool saveAndDrive = ImGui::Button("Save & Drive");
		if (!readyToSaveAndDrive) ImGui::EndDisabled();
		if (saveAndDrive)
		{
			if (InputManager::instance.saveBindingIni(Module::BindingsIniPath))
			{
				quickSetupBackup.clear();
				quickSetupComplete = false;
				unsavedChanges = false;
				confirmingLoad = false;
				persistenceStatus = "Bindings saved.";
				dialogOpen = false;
				ImGui::CloseCurrentPopup();
			}
			else
				persistenceStatus = "Could not save bindings. Check folder permissions and OutRun2006Tweaks.log.";
		}
		ImGui::SameLine();
		if (ImGui::Button("Keep & Fine-tune"))
		{
			quickSetupBackup.clear();
			quickSetupComplete = false;
			unsavedChanges = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Start Over"))
		{
			restore_quick_setup_backup();
			start_quick_setup();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			restore_quick_setup_backup();
			ImGui::CloseCurrentPopup();
		}
		if (!persistenceStatus.empty())
			ImGui::TextWrapped("%s", persistenceStatus.c_str());
		ImGui::EndPopup();
	}

public:
	void render(bool overlayEnabled) override
	{
		if (!Overlay::IsBindingDialogActive)
			return;

		auto& manager = InputManager::instance;

		auto padType = SDL_GAMEPAD_TYPE_XBOX360;
		if (auto* primary = manager.getPrimaryGamepad())
			padType = SDL_GetGamepadType(primary);

		bool dialogOpen = true;

		// Sized from the font rather than from the screen, so it tracks what is
		// in it instead of how large the monitor is. The action list is the tall
		// part and scrolls; nothing else grows.
		const Overlay::ContentRect content = Overlay::content_rect();
		const float fontSize = ImGui::GetFontSize();

		float width = fontSize * 27.0f;
		float height = fontSize * 26.0f;

		if (width > content.width * 0.9f)
			width = content.width * 0.9f;
		if (height > content.height * 0.9f)
			height = content.height * 0.9f;

		ImGui::SetNextWindowPos(ImVec2(content.x + (content.width * 0.5f), content.y + (content.height * 0.5f)),
			ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);

		ImGui::OpenPopup("Input Bindings");
		if (ImGui::BeginPopupModal("Input Bindings", &dialogOpen, ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove))
		{
			ImGui::TextWrapped(
				"Multi-device input setup. With UseNewInput enabled, steering, pedals, buttons, menu controls and calibration are saved and applied only from Input Bindings.");
			ImGui::TextDisabled("Force feedback is configured separately in the Force Feedback tab.");
			ImGui::Separator();

			if (ImGui::Button("Quick Setup"))
				start_quick_setup();

			ImGui::SameLine();
			if (ImGui::Button(unsavedChanges ? "Save bindings*##save" : "Save bindings##save"))
			{
				if (manager.saveBindingIni(Module::BindingsIniPath))
				{
					unsavedChanges = false;
					confirmingLoad = false;
					persistenceStatus = "Bindings saved.";
				}
				else
					persistenceStatus = "Could not save bindings. Check folder permissions and OutRun2006Tweaks.log.";
			}

			ImGui::SameLine();

			const char* loadLabel = unsavedChanges && confirmingLoad
				? "Discard edits & load?##load" : "Load bindings##load";
			if (ImGui::Button(loadLabel))
			{
				if (unsavedChanges && !confirmingLoad)
				{
					confirmingLoad = true;
					persistenceStatus = "Click again to discard unsaved edits and reload the saved binding file.";
				}
				else
				{
					if (manager.readBindingIni(Module::BindingsIniPath))
					{
						unsavedChanges = false;
						persistenceStatus = "Saved bindings loaded.";
					}
					else
						persistenceStatus = "Could not load bindings; the current bindings were kept.";
					confirmingLoad = false;
				}
			}

			// Three lines are reserved below: unsaved state, persistence status
			// and the exit/reset button row.
			const float footerHeight = ImGui::GetFrameHeightWithSpacing() +
				(ImGui::GetTextLineHeightWithSpacing() * 2.0f);

			ImGui::BeginChild("##body", ImVec2(0, -footerHeight));
			if (ImGui::BeginTabBar("##sections"))
			{
				if (ImGui::BeginTabItem("Bindings"))
				{
					// The action list is the only part that can outgrow the
					// dialog, so it is the only part that scrolls.
					if (ImGui::BeginChild("##actions", ImVec2(fontSize * 9.0f, 0), ImGuiChildFlags_Borders))
						draw_action_list();
					ImGui::EndChild();

					ImGui::SameLine();

					if (ImGui::BeginChild("##editor", ImVec2(0, 0)))
						draw_binding_editor(padType);
					ImGui::EndChild();

					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("Controllers"))
				{
					draw_controllers();
					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("Profiles"))
				{
					draw_profiles();
					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("Options"))
				{
					draw_options();
					ImGui::EndTabItem();
				}

				ImGui::EndTabBar();
			}
			ImGui::EndChild();

			// Kept on its own line whether or not there are changes, so the
			// buttons below don't shift as it appears.
			ImGui::TextDisabled("%s", unsavedChanges
				? "Note: unsaved bindings are active now but will be lost after restart."
				: "");
			ImGui::TextDisabled("%s", persistenceStatus.c_str());

			if (unsavedChanges)
			{
				if (ImGui::Button("Save & Return to game"))
				{
					if (manager.saveBindingIni(Module::BindingsIniPath))
					{
						unsavedChanges = false;
						confirmingLoad = false;
						persistenceStatus = "Bindings saved.";
						dialogOpen = false;
					}
					else
						persistenceStatus = "Could not save bindings. Check folder permissions and OutRun2006Tweaks.log.";
				}
				ImGui::SameLine();
				if (ImGui::Button("Return to game (not saved)"))
					dialogOpen = false;
			}
			else if (ImGui::Button("Return to game"))
				dialogOpen = false;

			ImGui::SameLine();

			if (ImGui::Button(!confirmingReset ? "Reset to default##clear" : "Are you sure?##clear"))
			{
				if (!confirmingReset)
				{
					confirmingReset = true;
				}
				else
				{
					unsavedChanges = true;
					Settings::SteeringDeadZone = 0.0f;
					setting_changed(Settings::SteeringDeadZone);
					Settings::BypassGameSensitivity = false;
					setting_changed(Settings::BypassGameSensitivity);
					manager.setupDefaultBindings();
					if (auto* controller = manager.getPrimaryGamepad())
						manager.setupGamepad(controller);
					confirmingReset = false;
				}
			}

			// Back/B leaves the dialog, but only while nothing is being bound -
			// otherwise the press meant for a binding closes the screen instead.
			if (isListeningForInput == ListenState::False && !unsavedChanges)
			{
				if ((manager.switch_overlay & (1 << int(SwitchId::Back) | 1 << int(SwitchId::B))) != 0)
					dialogOpen = false;
			}

			draw_listening_popup();
			draw_quick_setup_complete(dialogOpen);
			draw_calibration_popup();

			ImGui::EndPopup();
		}

		// Outside the popup so a change still reaches the INI on the frame the
		// dialog is closed.
		flush_settings();

		if (!dialogOpen)
		{
			Overlay::IsBindingDialogActive = false;
			Overlay::RequestMouseHide = true;
			confirmingReset = false;
			confirmingProfileLoad = false;
			confirmingProfileDelete = false;
			confirmingProfileOverwrite = false;
		}
	}

	static InputBindingsUI instance;
};
InputBindingsUI InputBindingsUI::instance;
