from pathlib import Path

input_ui_path = Path('src/overlay/input_bindings_ui.cpp')
setup_ui_path = Path('src/overlay/wheel_setup_ui.cpp')

input_ui = input_ui_path.read_text(encoding='utf-8')
setup_ui = setup_ui_path.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'round14 {label}: expected exactly one match, got {count}')
    print(f'ROUND14 patched: {label}')
    return text.replace(old, new, 1)


# In the default SDL3 multi-device path, Input Bindings is the only page that
# writes bindings consumed by InputManager. Make that ownership explicit in the
# UI so users do not try to use the legacy DirectInput mapper for controls.
input_ui = rep(
    input_ui,
'''\t\tif (ImGui::BeginPopupModal("Input Bindings", &dialogOpen, ImGuiWindowFlags_NoSavedSettings |\n\t\t\tImGuiWindowFlags_NoTitleBar |\n\t\t\tImGuiWindowFlags_NoResize |\n\t\t\tImGuiWindowFlags_NoMove))\n\t\t{\n\t\t\tif (ImGui::Button("Quick Setup"))\n''',
'''\t\tif (ImGui::BeginPopupModal("Input Bindings", &dialogOpen, ImGuiWindowFlags_NoSavedSettings |\n\t\t\tImGuiWindowFlags_NoTitleBar |\n\t\t\tImGuiWindowFlags_NoResize |\n\t\t\tImGuiWindowFlags_NoMove))\n\t\t{\n\t\t\tImGui::TextWrapped(\n\t\t\t\t"Multi-device input setup. With UseNewInput enabled, steering, pedals, buttons, menu controls and calibration are saved and applied only from Input Bindings.");\n\t\t\tImGui::TextDisabled("Force feedback is configured separately in the Force Feedback tab.");\n\t\t\tImGui::Separator();\n\n\t\t\tif (ImGui::Button("Quick Setup"))\n''',
    'make Input Bindings the explicit SDL input owner')

# The Force Feedback page is deliberately not another input mapper. State this
# unambiguously and use the exact Input Bindings name shown by the game.
setup_ui = rep(
    setup_ui,
'''                ImGui::TextWrapped(\n                    "Force feedback only. Configure steering, pedals, buttons and calibration in the game Controls / Controller Setup screen. This page selects the DirectInput FFB wheel and tunes its forces.");\n''',
'''                ImGui::TextWrapped(\n                    "Force feedback only. With UseNewInput enabled, steering, pedals, buttons, menu controls and calibration come only from Input Bindings. This page does not create input bindings; it only selects the DirectInput FFB wheel and tunes its forces.");\n''',
    'make Force Feedback page explicitly FFB-only')

setup_ui = rep(
    setup_ui,
'''                    ? "Input setup: game Controls / Controller Setup. FFB setup: this page only. Restart after switching to a different physical wheel."\n''',
'''                    ? "Input setup: Input Bindings only. FFB setup: this Force Feedback page only. Restart after switching to a different physical wheel."\n''',
    'use exact Input Bindings navigation name')

input_ui_path.write_text(input_ui, encoding='utf-8')
setup_ui_path.write_text(setup_ui, encoding='utf-8')
print('Applied Round-14 single-owner input/FFB UI roles')
