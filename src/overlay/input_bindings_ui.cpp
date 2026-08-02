#include "input_manager.hpp"

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
	// Index into the target action's bindings, or -1 to add a new one.
	// Deliberately not a pointer: the prompt persists across frames, and
	// setupDefaultBindings()/readBindingIni() clear these vectors.
	int currentBindingIndex = -1;

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

					if (currentBindingIndex >= 0 && currentBindingIndex < int(action->bindings().size()))
					{
						auto& target = action->bindings()[currentBindingIndex];
						target = InputBinding(key, target.negate);
					}
					else
					{
						// remove all existing keyboard bindings

						action->bindings().erase(
							std::remove_if(
								action->bindings().begin(),
								action->bindings().end(),
								[this](const InputBinding& bind)
								{
									return bind.isKeyboard() && bind.isNegated() == currentIsNegate;
								}
							),
							action->bindings().end()
						);

						action->add(InputBinding(key, currentIsNegate));
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
					if (currentBindingIndex >= 0 && currentBindingIndex < int(action->bindings().size()))
					{
						auto& target = action->bindings()[currentBindingIndex];
						target = InputBinding(static_cast<SDL_GamepadButton>(i), target.negate);
					}
					else
					{
						// remove all existing bindings
						action->bindings().erase(
							std::remove_if(
								action->bindings().begin(),
								action->bindings().end(),
								[this](const InputBinding& bind)
								{
									return bind.isGamepad() && bind.isNegated() == currentIsNegate;
								}
							),
							action->bindings().end()
						);

						action->add(InputBinding(static_cast<SDL_GamepadButton>(i), currentIsNegate));
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
					if (currentBindingIndex >= 0 && currentBindingIndex < int(action->bindings().size()))
					{
						auto& target = action->bindings()[currentBindingIndex];
						// Use the direction just flicked, not the one the old binding
						// happened to have
						target = InputBinding(static_cast<SDL_GamepadAxis>(i), negate);
					}
					else
					{
						action->bindings().erase(
							std::remove_if(
								action->bindings().begin(),
								action->bindings().end(),
								[](const InputBinding& bind)
								{
									return bind.isGamepad();
								}
							),
							action->bindings().end()
						);
						action->add(InputBinding(static_cast<SDL_GamepadAxis>(i), negate));
					}

					isListeningForInput = ListenState::WaitForBindButtonRelease;
					ImGui::CloseCurrentPopup();
					return true;
				}
			}
		}
		return false;
	}

	void showPromptVolume(ADChannel channel, bool isNegate, bool isKeyboard, int replacingIndex)
	{
		isListeningForInput = ListenState::WaitForButtonRelease;
		isBindingVolume = true;
		currentVolumeChannel = channel;
		currentIsNegate = isNegate;
		isBindingKeyboard = isKeyboard;
		currentlyBinding = InputManager::volumeNames[int(channel)];
		currentBindingIndex = replacingIndex;
		bindScreenDisplayTime = BindScreenTimeout;
	}

	void showPromptSwitch(SwitchId switchId, bool isNegate, bool isKeyboard, int replacingIndex)
	{
		isListeningForInput = ListenState::WaitForButtonRelease;
		isBindingVolume = false;
		currentSwitchId = switchId;
		currentIsNegate = isNegate;
		isBindingKeyboard = isKeyboard;
		currentlyBinding = InputManager::switchNames[int(switchId)];
		currentBindingIndex = replacingIndex;
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
							for (auto& bind : volAction.bindings())
							{
								if (bind.isGamepad())
								{
									hasBinds = true;

									if (ImGui::Button(std::format("{}##vol_ctrl_{}_{}", bind.displayName(padType, i == 0), i, sourceIdx).c_str()))
										showPromptVolume(ADChannel(i), bind.isNegated(), false, sourceIdx);
								}
								sourceIdx++;
							}

							// If no binds were displayed, show a single None button to allow user to bind it
							if (!hasBinds)
							{
								if (ImGui::Button(std::format("None##vol_ctrl_{}", i).c_str()))
									showPromptVolume(ADChannel(i), false, false, -1);
							}
						}

						// Keyboard binding
						{
							ImGui::TableNextColumn();
							std::string kbd_negative = "None";
							std::string kbd_positive = "None";
							for (auto& bind : volAction.bindings())
							{
								if (bind.isKeyboard())
								{
									if (volChannel != ADChannel::Steering)
									{
										kbd_negative = bind.displayName();
										break;
									}
									else
									{
										if (bind.isNegated())
											kbd_negative = bind.displayName(padType, true);
										else
											kbd_positive = bind.displayName(padType, true);
									}
								}
							}
							if (ImGui::Button(std::format("{}##vol_kb_neg_{}", kbd_negative, i).c_str()))
								showPromptVolume(ADChannel(i), volChannel == ADChannel::Steering ? true : false, true, -1);

							if (volChannel == ADChannel::Steering)
							{
								ImGui::SameLine();
								if (ImGui::Button(std::format("{}##vol_kb_pos_{}", kbd_positive, i).c_str()))
									showPromptVolume(ADChannel(i), false, true, -1);
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
							for (auto& bind : switchAction.bindings())
							{
								if (bind.isGamepad())
								{
									hasBinds = true;

									if (ImGui::Button(std::format("{}##switch_ctrl_{}_{}", bind.displayName(), i, sourceIdx).c_str()))
										showPromptSwitch(SwitchId(i), bind.isNegated(), false, sourceIdx);
								}
								sourceIdx++;
							}

							if(!hasBinds)
								if (ImGui::Button(std::format("None##switch_ctrl_{}", i).c_str()))
									showPromptSwitch(SwitchId(i), false, false, -1);
						}

						// Keyboard binding
						{
							ImGui::TableNextColumn();
							std::string keyboardBind = "None";
							for (auto& bind : switchAction.bindings())
							{
								if (bind.isKeyboard())
								{
									keyboardBind = bind.displayName(padType, false);
									break;
								}
							}
							if (ImGui::Button(std::format("{}##switch_kb_{}", keyboardBind, i).c_str()))
								showPromptSwitch(SwitchId(i), false, true, -1);
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

