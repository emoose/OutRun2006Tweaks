#include "input_manager.hpp"

#include <algorithm>
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
		bool isVolume = true;
		int index = 0;

		bool operator==(const Selection& other) const
		{
			return isVolume == other.isVolume && index == other.index;
		}
	};

	// The order actions are listed in, grouped by when they get used.
	struct ActionListEntry
	{
		const char* group;
		bool isVolume;
		int index;
	};

	static inline const ActionListEntry ActionList[] = {
		{ "Driving", true,  int(ADChannel::Steering)        },
		{ "Driving", true,  int(ADChannel::Acceleration)    },
		{ "Driving", true,  int(ADChannel::Brake)           },
		{ "Driving", false, int(SwitchId::GearUp)           },
		{ "Driving", false, int(SwitchId::GearDown)         },
		{ "Driving", false, int(SwitchId::ChangeView)       },

		{ "Menus",   false, int(SwitchId::Start)            },
		{ "Menus",   false, int(SwitchId::Back)             },
		{ "Menus",   false, int(SwitchId::A)                },
		{ "Menus",   false, int(SwitchId::B)                },
		{ "Menus",   false, int(SwitchId::X)                },
		{ "Menus",   false, int(SwitchId::Y)                },
		{ "Menus",   false, int(SwitchId::SelectionUp)      },
		{ "Menus",   false, int(SwitchId::SelectionDown)    },
		{ "Menus",   false, int(SwitchId::SelectionLeft)    },
		{ "Menus",   false, int(SwitchId::SelectionRight)   },

		{ "Online",  false, int(SwitchId::License)          },
		{ "Online",  false, int(SwitchId::SignIn)           },
	};

	// How far an analog action has to move from rest before its name lights up.
	// Digital actions use the game's own threshold instead, via
	// InputState::isPressed.
	static constexpr float ActiveThreshold = 0.25f;

	Selection selected{ true, int(ADChannel::Steering) };

	// What the listening prompt will write into. -1 appends a binding instead of
	// replacing one.
	Selection bindTarget;
	int bindIndex = -1;
	std::string bindingName;

	// Track binding changes (options tab are handled differently)
	bool unsavedChanges = false;
	bool confirmingReset = false;

	std::vector<Settings::SettingBase*> pendingSettings;

	static InputAction& action_for(const Selection& selection)
	{
		auto& manager = InputManager::instance;
		return selection.isVolume ? manager.volumeBindings[selection.index]
			: manager.switchBindings[selection.index];
	}

	static const std::string& name_for(const Selection& selection)
	{
		return selection.isVolume ? InputManager::volumeNames[selection.index]
			: InputManager::switchNames[selection.index];
	}

	// Steering reads as a signed axis, and its bindings are named differently
	// because of it (left/right rather than negated/not).
	static bool is_steering(const Selection& selection)
	{
		return selection.isVolume && selection.index == int(ADChannel::Steering);
	}

	void begin_listening(const Selection& target, int index)
	{
		// Waits for the click that got here to be let go of first, otherwise it
		// binds the mouse button or whatever key triggered it.
		isListeningForInput = ListenState::WaitForButtonRelease;
		bindTarget = target;
		bindIndex = index;
		bindingName = name_for(target);
	}

public:
	void init() override {}

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
			isListeningForInput = ListenState::False;
			ImGui::CloseCurrentPopup();
			return false;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
		{
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
			if (bindIndex >= 0 && bindIndex < int(bindings.size()))
				bindings[bindIndex] = binding;
			else
				action.add(binding);

			isListeningForInput = ListenState::WaitForBindButtonRelease;
			ImGui::CloseCurrentPopup();
		};

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

			const Selection action{ entry.isVolume, entry.index };
			const InputState& state = action_for(action).getState();

			// A binding's value keeps its sign, and negating a binding flips it,
			// so an axis bound to two opposing digital actions gives one of them
			// a positive value and the other a negative one. Only the positive
			// side fires, which is what isPressed tests for - taking the
			// magnitude instead would light up both ends of the same stick.
			// An analog action uses the sign for direction rather than for on
			// and off, so there either end of the range counts as movement.
			const bool active = action.isVolume
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

		if (ImGui::BeginTable("##bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthStretch);
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
				const std::string label = std::format("{}  ({})",
					binding.displayName(padType, steering),
					binding.isKeyboard() ? "keyboard" : "controller");

				if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0)))
					begin_listening(selected, i);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Rebind this input");

				// Inverting is the only way to reach the '-' suffix the INI
				// format has always had: it sends an analog action the opposite
				// direction, and makes a digital action fire on negative input.
				ImGui::TableNextColumn();
				if (ImGui::Checkbox("##invert", &binding.negate))
					unsavedChanges = true;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(steering
						? "Steer the other way with this input"
						: "Invert this input");

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

		if (ImGui::Button("+ Add binding"))
			begin_listening(selected, -1);

		// Live value of the selected action.
		const float value = action.getState().currentValue;
		const float filled = selected.isVolume ? std::abs(value) : value;

		ImGui::Spacing();
		ImGui::TextDisabled("Reading");
		ImGui::ProgressBar(std::clamp(filled, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0),
			std::format("{:.2f}", value).c_str());
	}

	void draw_controllers()
	{
		auto& manager = InputManager::instance;

		if (manager.controllers.empty())
		{
			ImGui::TextDisabled("No controllers detected.");
			return;
		}

		for (size_t i = 0; i < manager.controllers.size(); i++)
		{
			auto* controller = manager.controllers[i];
			const bool primary = int(i) == manager.primaryControllerIndex;

			ImGui::PushID(int(i));
			if (ImGui::RadioButton(SDL_GetGamepadName(controller), primary))
				manager.setPrimaryGamepad(i);
			ImGui::PopID();
		}

		ImGui::Spacing();
		ImGui::TextDisabled("Bindings apply to whichever controller is selected.");
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

	void draw_options()
	{
		auto& manager = InputManager::instance;

		const char* vibrationModes[] = { "Disabled", "Enabled", "Swap L/R", "Merge L/R" };
		if (ImGui::Combo("Vibration Mode", Settings::VibrationMode.ptr(), vibrationModes, IM_ARRAYSIZE(vibrationModes)))
			setting_changed(Settings::VibrationMode);
		if (ImGui::SliderInt("Vibration Strength", Settings::VibrationStrength.ptr(), 0, 10))
			setting_changed(Settings::VibrationStrength);
		if (ImGui::Combo("Impulse Vibration", Settings::ImpulseVibrationMode.ptr(), vibrationModes, IM_ARRAYSIZE(vibrationModes)))
			setting_changed(Settings::ImpulseVibrationMode);

		int deadzonePercent = int(Settings::SteeringDeadZone * 100.f);
		if (ImGui::SliderInt("Steering Deadzone", &deadzonePercent, 5, 20, "%d%%"))
		{
			Settings::SteeringDeadZone = float(deadzonePercent) / 100.f;
			setting_changed(Settings::SteeringDeadZone);
		}

		ImGui::Checkbox("Bypass Sensitivity", &manager.BypassGameSensitivity);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Passes steering input to the game directly, allows for more sensitive controls");
	}

	// The prompt shown while an input is being waited on.
	void draw_listening_popup()
	{
		if (isListeningForInput == ListenState::False)
			return;

		auto& manager = InputManager::instance;

		if (isListeningForInput == ListenState::WaitForButtonRelease)
		{
			if (!manager.anyInputPressed())
				isListeningForInput = ListenState::Listening;
			return;
		}

		if (isListeningForInput == ListenState::WaitForBindButtonRelease)
		{
			if (!manager.anyInputPressed())
				isListeningForInput = ListenState::False;
			return;
		}

		ImGui::OpenPopup("Listening for Input");
		if (ImGui::BeginPopupModal("Listening for Input", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Press any input to bind to %s", bindingName.c_str());
			ImGui::Spacing();
			ImGui::TextDisabled("Escape to cancel, Delete to clear");

			if (HandleNewBinding())
				unsavedChanges = true;

			ImGui::EndPopup();
		}
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
			if (ImGui::Button(unsavedChanges ? "Save bindings*##save" : "Save bindings##save"))
				if (manager.saveBindingIni(Module::BindingsIniPath))
					unsavedChanges = false;

			ImGui::SameLine();

			if (ImGui::Button("Load bindings"))
				if (manager.readBindingIni(Module::BindingsIniPath))
					unsavedChanges = false;

			// Two lines are reserved below: the unsaved note and the button row.
			const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();

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
			ImGui::TextDisabled("%s", unsavedChanges ? "Note: you have unsaved changes!" : "");

			if (ImGui::Button("Return to game"))
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
					Settings::SteeringDeadZone = 0.2f;
					setting_changed(Settings::SteeringDeadZone);
					manager.BypassGameSensitivity = false;
					manager.setupDefaultBindings();
					if (auto* controller = manager.getPrimaryGamepad())
						manager.setupGamepad(controller);
					confirmingReset = false;
				}
			}

			// Back/B leaves the dialog, but only while nothing is being bound -
			// otherwise the press meant for a binding closes the screen instead.
			if (isListeningForInput == ListenState::False)
			{
				if ((manager.switch_overlay & (1 << int(SwitchId::Back) | 1 << int(SwitchId::B))) != 0)
					dialogOpen = false;
			}

			draw_listening_popup();

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
		}
	}

	static InputBindingsUI instance;
};
InputBindingsUI InputBindingsUI::instance;
