from pathlib import Path

path = Path('src/overlay/wheel_setup_ui.cpp')
text = path.read_text(encoding='utf-8')


def rep(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'round13 {label}: expected exactly one match, got {count}')
    print(f'ROUND13 patched: {label}')
    text = text.replace(old, new, 1)


rep(
'''#include "overlay.hpp"\n\nnamespace Settings\n''',
'''#include "overlay.hpp"\n\nvoid WheelFFB_RequestDirectionTest(int direction);\n\nnamespace Settings\n''',
'forward declare safe direction test')

rep(
'''    extern Setting<bool> WheelFFBInvertSpring;\n''',
'''    extern Setting<bool> WheelFFBInvertSpring;\n    extern Setting<bool> WheelFFBUsePeriodicEffects;\n    extern Setting<bool> WheelFFBDebugLog;\n''',
'declare remaining FFB controls')

rep(
'''            ImGui::SeparatorText("Simulation FFB");\n            ImGui::TextWrapped(\n''',
'''            ImGui::SeparatorText("Simulation FFB");\n            ImGui::Checkbox("Enable Force Feedback", Settings::WheelFFBEnable.ptr());\n            ImGui::TextDisabled("gameplay FFB follows the exact selected DirectInput GUID.");\n            ImGui::TextWrapped(\n''',
'put FFB enable and exact GUID guidance on single page')

rep(
'''            ImGui::Checkbox("Hardware GUID_Damper", Settings::WheelFFBUseHardwareDamper.ptr());\n            ImGui::Checkbox("Reverse ConstantForce", Settings::WheelFFBInvertForce.ptr());\n''',
'''            ImGui::Checkbox("Hardware GUID_Damper", Settings::WheelFFBUseHardwareDamper.ptr());\n            ImGui::Checkbox("Hardware road/slip sine effects", Settings::WheelFFBUsePeriodicEffects.ptr());\n            ImGui::SameLine();\n            ImGui::Checkbox("Diagnostic logging", Settings::WheelFFBDebugLog.ptr());\n            ImGui::Checkbox("Reverse ConstantForce", Settings::WheelFFBInvertForce.ptr());\n''',
'keep periodic and diagnostic toggles')

rep(
'''            if (ImGui::IsItemHovered())\n                ImGui::SetTooltip("Use Reverse Spring only if the wheel pushes farther away from centre. ConstantForce direction is independent.");\n\n            if (ImGui::Button("Load MOZA R3 SAT test"))\n''',
'''            if (ImGui::IsItemHovered())\n                ImGui::SetTooltip("Use Reverse Spring only if the wheel pushes farther away from centre. ConstantForce direction is independent.");\n\n            ImGui::SeparatorText("Safe direction test");\n            if (ImGui::Button("Test Left (20%)"))\n                WheelFFB_RequestDirectionTest(-1);\n            ImGui::SameLine();\n            if (ImGui::Button("Test Right (20%)"))\n                WheelFFB_RequestDirectionTest(1);\n            ImGui::SameLine();\n            if (ImGui::Button("Stop Test"))\n                WheelFFB_RequestDirectionTest(0);\n            ImGui::TextDisabled("Direction tests are hard-capped at 20% and only run during active gameplay.");\n\n            if (ImGui::Button("Load MOZA R3 SAT test"))\n''',
'keep safe direction tests on single FFB page')

path.write_text(text, encoding='utf-8')
print('Applied Round-13 complete single-page FFB controls')
