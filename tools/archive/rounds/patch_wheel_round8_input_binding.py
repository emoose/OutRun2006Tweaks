from pathlib import Path

manager_path = Path('src/input_manager.hpp')
ui_path = Path('src/overlay/input_bindings_ui.cpp')
wheel_ui_path = Path('src/overlay/wheel_setup_ui.cpp')

manager = manager_path.read_text(encoding='utf-8')
ui = ui_path.read_text(encoding='utf-8')
wheel_ui = wheel_ui_path.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'ROUND8 {label}: expected exactly one match, got {count}')
    print(f'ROUND8 patched: {label}')
    return text.replace(old, new, 1)


# 1) Eliminate first-frame undefined input state. These scalar masks were left
# indeterminate by the implicit InputManager constructor, so SwitchOn could see
# a phantom edge before the first real read.
manager = rep(
    manager,
    '''\tInputSourceType lastSourceType;''',
    '''\tInputSourceType lastSourceType = InputSourceType::GamePad;''',
    'initialize InputState source')
manager = rep(
    manager,
    '''\tuint32_t switch_current;\n\tuint32_t switch_previous;\n\tuint32_t switch_overlay;''',
    '''\tuint32_t switch_current = 0;\n\tuint32_t switch_previous = 0;\n\tuint32_t switch_overlay = 0;''',
    'initialize switch masks')

# 2) Digital actions must ignore the inactive/negative side of analog bindings.
# Otherwise a full-scale negative axis can tie a +1 button and win merely
# because it was earlier in the vector, suppressing a valid digital press.
manager = rep(
    manager,
    '''\tconst InputState& update(SDL_Gamepad* primary_pad,\n\t\tconst std::function<SDL_Joystick*(const InputBinding&)>& joystickForBinding)\n\t{\n\t\tfloat maxValue = 0.0f;\n\t\tbool isAxisInput = false;\n\t\tInputSourceType lastSource = state_.lastSourceType;\n\n\t\t// Read all bindings and take the highest absolute value\n\t\tfor (const auto& binding : bindings_)\n\t\t{\n\t\t\tfloat currentValue = binding.read(primary_pad, joystickForBinding);\n\t\t\tif (std::abs(currentValue) > std::abs(maxValue))\n\t\t\t{\n\t\t\t\tmaxValue = currentValue;\n\t\t\t\tisAxisInput = binding.isAxis();\n\t\t\t\tlastSource = binding.sourceType();\n\t\t\t}\n\t\t}''',
    '''\tconst InputState& update(SDL_Gamepad* primary_pad,\n\t\tconst std::function<SDL_Joystick*(const InputBinding&)>& joystickForBinding,\n\t\tbool positiveOnly = false)\n\t{\n\t\tfloat maxValue = 0.0f;\n\t\tbool isAxisInput = false;\n\t\tInputSourceType lastSource = state_.lastSourceType;\n\n\t\t// Analog actions need the largest magnitude so signed steering survives.\n\t\t// Digital actions pass positiveOnly=true: the negative side of an axis is\n\t\t// inactive for that action and must never mask another +1 button binding.\n\t\tfor (const auto& binding : bindings_)\n\t\t{\n\t\t\tfloat currentValue = binding.read(primary_pad, joystickForBinding);\n\t\t\tif (positiveOnly && currentValue <= 0.0f)\n\t\t\t\tcontinue;\n\t\t\tif (std::abs(currentValue) > std::abs(maxValue))\n\t\t\t{\n\t\t\t\tmaxValue = currentValue;\n\t\t\t\tisAxisInput = binding.isAxis();\n\t\t\t\tlastSource = binding.sourceType();\n\t\t\t}\n\t\t}''',
    'digital actions ignore negative axis side')
manager = rep(
    manager,
    '''\t\t\tauto& switchState = switchBindings[i].update(gamepad, resolveJoystick);''',
    '''\t\t\tauto& switchState = switchBindings[i].update(gamepad, resolveJoystick, true);''',
    'switch bindings use positive-only aggregation')
manager = rep(
    manager,
    '''\t\t\tmodStates[i] = modBindings[i].update(gamepad, resolveJoystick);''',
    '''\t\t\tmodStates[i] = modBindings[i].update(gamepad, resolveJoystick, true);''',
    'mod bindings use positive-only aggregation')

# 3) The overlay/input dialog must zero game-facing analog controls, not leave
# the last cached throttle/brake/steering value frozen under the menu.
manager = rep(
    manager,
    '''\t\t\tauto& vol = volumeBindings[i].update(gamepad, resolveJoystick);\n\t\t\tif (Overlay::IsBindingDialogActive || Overlay::IsActive) [[unlikely]]\n\t\t\t\tcontinue;\n\n\t\t\tvolumes[i] = vol;''',
    '''\t\t\tauto& vol = volumeBindings[i].update(gamepad, resolveJoystick);\n\t\t\tif (Overlay::IsBindingDialogActive || Overlay::IsActive) [[unlikely]]\n\t\t\t{\n\t\t\t\t// Keep the binding's live state for the UI, but never leave a stale\n\t\t\t\t// throttle/brake/steering command active underneath the overlay.\n\t\t\t\tvolumes[i].previousValue = 0.0f;\n\t\t\t\tvolumes[i].currentValue = 0.0f;\n\t\t\t\tcontinue;\n\t\t\t}\n\n\t\t\tvolumes[i] = vol;''',
    'overlay zeroes stale game-facing analog values')

# 4) Make binding-file loading fail-safe. A syntactically non-empty but totally
# invalid file used to clear every working binding and still return success.
manager = rep(
    manager,
    '''\t\t// we have binds, reset any of our defaults\n\t\tfor (auto& binding : volumeBindings)\n\t\t\tbinding.clear();\n\t\tfor (auto& binding : switchBindings)\n\t\t\tbinding.clear();\n\t\tfor (auto& binding : modBindings)\n\t\t\tbinding.clear();\n\n\t\tfor (const auto& entry : entries)\n\t\t{''',
    '''\t\t// Preserve the current working configuration until at least one entry\n\t\t// has parsed successfully. This makes Load bindings transactional for a\n\t\t// completely malformed/truncated file.\n\t\tauto previousVolumeBindings = volumeBindings;\n\t\tauto previousSwitchBindings = switchBindings;\n\t\tauto previousModBindings = modBindings;\n\t\tfor (auto& binding : volumeBindings)\n\t\t\tbinding.clear();\n\t\tfor (auto& binding : switchBindings)\n\t\t\tbinding.clear();\n\t\tfor (auto& binding : modBindings)\n\t\t\tbinding.clear();\n\t\tint loadedBinds = 0;\n\n\t\tfor (const auto& entry : entries)\n\t\t{''',
    'binding load preserves previous configuration until valid')
manager = rep(
    manager,
    '''\t\t\tbinding->negate = action->negate;\n\t\t\taddBinding(*action, *binding);\n\t\t}\n\n\t\treturn true;\n\t}''',
    '''\t\t\tbinding->negate = action->negate;\n\t\t\tif (binding->negate && !binding->isAxis() &&\n\t\t\t\t!(action->kind == ActionRef::Kind::Volume &&\n\t\t\t\t\taction->index == int(ADChannel::Steering)))\n\t\t\t{\n\t\t\t\t// A '-' suffix on a button/key cannot select an opposite direction;\n\t\t\t\t// it only turns +1 into -1 and makes digital/pedal actions unusable.\n\t\t\t\tspdlog::warn(__FUNCTION__ ": ignoring invalid negative suffix for non-axis action {}", entry.key);\n\t\t\t\tbinding->negate = false;\n\t\t\t}\n\t\t\taddBinding(*action, *binding);\n\t\t\t++loadedBinds;\n\t\t}\n\n\t\tif (loadedBinds == 0)\n\t\t{\n\t\t\tvolumeBindings = std::move(previousVolumeBindings);\n\t\t\tswitchBindings = std::move(previousSwitchBindings);\n\t\t\tmodBindings = std::move(previousModBindings);\n\t\t\tspdlog::error(__FUNCTION__ ": no valid bindings parsed; previous bindings preserved");\n\t\t\treturn false;\n\t\t}\n\n\t\tensureOverlayBindable();\n\t\treturn true;\n\t}''',
    'binding load rejects zero-valid file and normalizes impossible button negation')

# 5) Track the exact input that was captured and require it to be released.
# anyInputPressed() cannot treat arbitrary raw joystick axes as "pressed"
# because many pedals rest at -32768/+32767. Without a captured-input guard,
# Quick Setup can advance while a pedal is still down, then bind its release
# motion as the next pedal.
ui = rep(
    ui,
    '''\tstd::optional<InputBinding> quickSetupCandidate;\n\tbool quickSetupTimedOut = false;''',
    '''\tstd::optional<InputBinding> quickSetupCandidate;\n\tstd::optional<InputBinding> releaseGuardBinding;\n\tbool quickSetupTimedOut = false;''',
    'add captured-input release guard')
ui = rep(
    ui,
    '''\t\taxisBaseline.clear();\n\t\tif (quickSetupActive)''',
    '''\t\taxisBaseline.clear();\n\t\treleaseGuardBinding.reset();\n\t\tif (quickSetupActive)''',
    'new listen clears old release guard')
ui = rep(
    ui,
    '''\t\tquickSetupCandidate.reset();\n\t\tquickSetupTimedOut = false;\n\t\tunsavedChanges = quickSetupPreviousUnsaved;''',
    '''\t\tquickSetupCandidate.reset();\n\t\treleaseGuardBinding.reset();\n\t\tquickSetupTimedOut = false;\n\t\tunsavedChanges = quickSetupPreviousUnsaved;''',
    'quick setup restore clears release guard')
ui = rep(
    ui,
    '''\t\t\telse if (bindIndex >= 0 && bindIndex < int(bindings.size()))\n\t\t\t\tbindings[bindIndex] = binding;\n\t\t\telse\n\t\t\t\taction.add(binding);\n\n\t\t\tisListeningForInput = ListenState::WaitForBindButtonRelease;''',
    '''\t\t\telse if (bindIndex >= 0 && bindIndex < int(bindings.size()))\n\t\t\t\tbindings[bindIndex] = binding;\n\t\t\telse\n\t\t\t\taction.add(binding);\n\n\t\t\treleaseGuardBinding = binding;\n\t\t\tisListeningForInput = ListenState::WaitForBindButtonRelease;''',
    'normal binding waits for captured input release')
ui = rep(
    ui,
    '''\t\tif (isListeningForInput == ListenState::WaitForBindButtonRelease)\n\t\t{\n\t\t\tif (!manager.anyInputPressed())\n\t\t\t{\n\t\t\t\tif (quickSetupActive && quickSetupStep < int(std::size(QuickSetupSteps)))\n\t\t\t\t\tbegin_listening(quick_setup_selection(quickSetupStep), -1);\n\t\t\t\telse\n\t\t\t\t{\n\t\t\t\t\tisListeningForInput = ListenState::False;\n\t\t\t\t\tif (quickSetupActive)\n\t\t\t\t\t{\n\t\t\t\t\t\tquickSetupActive = false;\n\t\t\t\t\t\tquickSetupComplete = true;\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t\treturn;\n\t\t}''',
    '''\t\tif (isListeningForInput == ListenState::WaitForBindButtonRelease)\n\t\t{\n\t\t\tbool capturedReleased = true;\n\t\t\tif (releaseGuardBinding)\n\t\t\t{\n\t\t\t\tconst auto resolveJoystick = [&manager](const InputBinding& binding)\n\t\t\t\t\t{\n\t\t\t\t\t\treturn manager.joystickForBinding(binding);\n\t\t\t\t\t};\n\t\t\t\tcapturedReleased = std::abs(releaseGuardBinding->read(\n\t\t\t\t\tmanager.getPrimaryGamepad(), resolveJoystick)) < 0.25f;\n\t\t\t}\n\n\t\t\tif (capturedReleased && !manager.anyInputPressed())\n\t\t\t{\n\t\t\t\treleaseGuardBinding.reset();\n\t\t\t\tif (quickSetupActive && quickSetupStep < int(std::size(QuickSetupSteps)))\n\t\t\t\t\tbegin_listening(quick_setup_selection(quickSetupStep), -1);\n\t\t\t\telse\n\t\t\t\t{\n\t\t\t\t\tisListeningForInput = ListenState::False;\n\t\t\t\t\tif (quickSetupActive)\n\t\t\t\t\t{\n\t\t\t\t\t\tquickSetupActive = false;\n\t\t\t\t\t\tquickSetupComplete = true;\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t\treturn;\n\t\t}''',
    'captured raw axis must release before next binding')
ui = rep(
    ui,
    '''\t\t\t\t\tstd::erase_if(bindings, [](const InputBinding& existing) { return !existing.isKeyboard(); });\n\t\t\t\t\taction_for(bindTarget).add(*quickSetupCandidate);\n\t\t\t\t\tquickSetupCandidate.reset();\n\t\t\t\t\t++quickSetupStep;''',
    '''\t\t\t\t\tstd::erase_if(bindings, [](const InputBinding& existing) { return !existing.isKeyboard(); });\n\t\t\t\t\taction_for(bindTarget).add(*quickSetupCandidate);\n\t\t\t\t\treleaseGuardBinding = *quickSetupCandidate;\n\t\t\t\t\tquickSetupCandidate.reset();\n\t\t\t\t\t++quickSetupStep;''',
    'quick setup confirmation waits for captured axis release')
ui = rep(
    ui,
    '''\t\t\t\tif (ImGui::Button("Try again"))\n\t\t\t\t{\n\t\t\t\t\tbegin_listening(bindTarget, -1);\n\t\t\t\t\tImGui::CloseCurrentPopup();\n\t\t\t\t}''',
    '''\t\t\t\tif (ImGui::Button("Try again"))\n\t\t\t\t{\n\t\t\t\t\treleaseGuardBinding = *quickSetupCandidate;\n\t\t\t\t\tquickSetupCandidate.reset();\n\t\t\t\t\tisListeningForInput = ListenState::WaitForBindButtonRelease;\n\t\t\t\t\tImGui::CloseCurrentPopup();\n\t\t\t\t}''',
    'quick setup retry also waits for captured input release')

# 6) Do not expose an "Invert" switch where it can only break a button/key.
# Steering buttons need it for left/right, and digital actions need it for
# signed axes; pedal FromRest direction remains auto-detected.
ui = rep(
    ui,
    '''\t\t\t\tif (binding.kind == InputBinding::Kind::JoyAxis && binding.axisMode == InputBinding::AxisMode::FromRest)\n\t\t\t\t{\n\t\t\t\t\tImGui::TextDisabled("Auto");\n\t\t\t\t\tif (ImGui::IsItemHovered())\n\t\t\t\t\t\tImGui::SetTooltip("Pedal direction is detected automatically during calibration");\n\t\t\t\t}\n\t\t\t\telse\n\t\t\t\t{\n\t\t\t\t\tif (ImGui::Checkbox("##invert", &binding.negate))\n\t\t\t\t\t\tunsavedChanges = true;\n\t\t\t\t\tif (ImGui::IsItemHovered())\n\t\t\t\t\t\tImGui::SetTooltip(steering\n\t\t\t\t\t\t\t? "Steer the other way with this input"\n\t\t\t\t\t\t\t: "Invert this input");\n\t\t\t\t}''',
    '''\t\t\t\tif (binding.kind == InputBinding::Kind::JoyAxis && binding.axisMode == InputBinding::AxisMode::FromRest)\n\t\t\t\t{\n\t\t\t\t\tImGui::TextDisabled("Auto");\n\t\t\t\t\tif (ImGui::IsItemHovered())\n\t\t\t\t\t\tImGui::SetTooltip("Pedal direction is detected automatically during calibration");\n\t\t\t\t}\n\t\t\t\telse if (steering || binding.isAxis())\n\t\t\t\t{\n\t\t\t\t\tif (ImGui::Checkbox("##invert", &binding.negate))\n\t\t\t\t\t\tunsavedChanges = true;\n\t\t\t\t\tif (ImGui::IsItemHovered())\n\t\t\t\t\t\tImGui::SetTooltip(steering\n\t\t\t\t\t\t\t? "Steer the other way with this input"\n\t\t\t\t\t\t\t: "Use the opposite side of this axis");\n\t\t\t\t}\n\t\t\t\telse\n\t\t\t\t{\n\t\t\t\t\tImGui::TextDisabled("-");\n\t\t\t\t\tif (ImGui::IsItemHovered())\n\t\t\t\t\t\tImGui::SetTooltip("Invert is only meaningful for axes or steering-direction buttons");\n\t\t\t\t}''',
    'hide invalid invert toggle on ordinary buttons and keys')

# 7) Legacy F11 import enables the universal profile too, so it must disable
# the fixed MOZA R3 menu readers just like the explicit enable checkbox does.
wheel_ui = rep(
    wheel_ui,
    '''            if (ImGui::Button("Import current game mapping"))\n            {\n                if (UniversalWheelProfile::copy_current_mapping())\n                {\n                    Settings::WheelUniversalSetupEnable = true;\n                    status_ = "Imported current steering/pedal/shift bindings. Menu bindings can now be added below.";\n                }\n                else\n                    status_ = "No regular legacy DirectInput game device is ready yet.";\n            }''',
    '''            if (ImGui::Button("Import current game mapping"))\n            {\n                if (UniversalWheelProfile::copy_current_mapping())\n                {\n                    Settings::WheelUniversalSetupEnable = true;\n                    Settings::WheelMenuR3DirectDPad = false;\n                    Settings::WheelMenuR3DirectAB = false;\n                    status_ = "Imported current steering/pedal/shift bindings. Universal menu ownership enabled; add menu bindings below.";\n                }\n                else\n                    status_ = "No regular legacy DirectInput game device is ready yet.";\n            }''',
    'legacy mapping import disables fixed R3 menu helpers')

manager_path.write_text(manager, encoding='utf-8')
ui_path.write_text(ui, encoding='utf-8')
wheel_ui_path.write_text(wheel_ui, encoding='utf-8')
print('Applied round-8 input binding, quick-setup, and legacy menu hardening')
