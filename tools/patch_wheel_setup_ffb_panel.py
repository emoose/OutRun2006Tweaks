from pathlib import Path

path = Path("src/overlay/wheel_setup_ui.cpp")
text = path.read_text(encoding="utf-8")

old = '''    extern Setting<std::string> WheelFFBDeviceName;\n\n    Setting<bool> WheelUniversalSetupEnable{\n'''
new = '''    extern Setting<std::string> WheelFFBDeviceName;\n    extern Setting<bool> WheelMenuR3DirectDPad;\n    extern Setting<bool> WheelMenuR3DirectAB;\n    extern Setting<float> WheelFFBGlobalStrength;\n    extern Setting<float> WheelFFBSpringStrength;\n    extern Setting<float> WheelFFBDamperStrength;\n    extern Setting<float> WheelFFBSteeringWeight;\n    extern Setting<float> WheelFFBGripLoss;\n    extern Setting<float> WheelFFBLowSpeedSpring;\n    extern Setting<float> WheelFFBSpringLoadBoost;\n    extern Setting<float> WheelFFBRoadTexture;\n    extern Setting<float> WheelFFBTireSlip;\n    extern Setting<float> WheelFFBWallImpact;\n    extern Setting<bool> WheelFFBUseHardwareSpring;\n    extern Setting<bool> WheelFFBUseHardwareDamper;\n\n    Setting<bool> WheelUniversalSetupEnable{\n'''
if old not in text:
    raise SystemExit("FFB extern insertion point not found")
text = text.replace(old, new, 1)

old = '''            ImGui::Checkbox("Enable F11 universal wheel profile", Settings::WheelUniversalSetupEnable.ptr());\n            if (ImGui::IsItemHovered())\n                ImGui::SetTooltip("When enabled, these bindings are written into OutRun's original legacy DirectInput device mapping.");\n'''
new = '''            if (ImGui::Checkbox("Enable F11 universal wheel profile", Settings::WheelUniversalSetupEnable.ptr()))\n            {\n                if (Settings::WheelUniversalSetupEnable)\n                {\n                    // The old R3-specific readers assume fixed button numbers.\n                    // Once a universal profile is active, its own bindings must\n                    // be the sole wheel-menu source so Logitech/Thrustmaster/\n                    // Fanatec/Simagic/etc. do not inherit R3 button mappings.\n                    Settings::WheelMenuR3DirectDPad = false;\n                    Settings::WheelMenuR3DirectAB = false;\n                    status_ = "Universal profile enabled; legacy R3 fixed-button menu helpers were disabled.";\n                }\n            }\n            if (ImGui::IsItemHovered())\n                ImGui::SetTooltip("When enabled, these bindings are written into OutRun's original legacy DirectInput device mapping.");\n'''
if old not in text:
    raise SystemExit("universal profile checkbox insertion point not found")
text = text.replace(old, new, 1)

old = '''            ImGui::SeparatorText("Wheel options");\n            int deadzonePercent = int(float(Settings::SteeringDeadZone) * 100.0f + 0.5f);\n'''
new = '''            ImGui::SeparatorText("Simulation FFB");\n            ImGui::TextWrapped(\n                "Aligning force grows strongly with speed and corner load, unloads with grip loss, and GUID_Damper resists steering velocity without acting like a centering spring.");\n\n            ImGui::SliderFloat("Overall Strength", Settings::WheelFFBGlobalStrength.ptr(), 0.0f, 1.0f, "%.2f");\n            ImGui::SliderFloat("Aligning / Spring", Settings::WheelFFBSpringStrength.ptr(), 0.0f, 1.0f, "%.2f");\n            ImGui::SliderFloat("Low-speed Aligning", Settings::WheelFFBLowSpeedSpring.ptr(), 0.0f, 0.30f, "%.2f");\n            ImGui::SliderFloat("Corner-load Boost", Settings::WheelFFBSpringLoadBoost.ptr(), 0.0f, 0.80f, "%.2f");\n            ImGui::SliderFloat("Dynamic Damping", Settings::WheelFFBDamperStrength.ptr(), 0.0f, 0.80f, "%.2f");\n            ImGui::SliderFloat("Lateral / Steering Weight", Settings::WheelFFBSteeringWeight.ptr(), 0.0f, 1.0f, "%.2f");\n            ImGui::SliderFloat("Grip-loss Unload", Settings::WheelFFBGripLoss.ptr(), 0.0f, 1.0f, "%.2f");\n            ImGui::SliderFloat("Road Detail", Settings::WheelFFBRoadTexture.ptr(), 0.0f, 0.50f, "%.2f");\n            ImGui::SliderFloat("Tire Slip", Settings::WheelFFBTireSlip.ptr(), 0.0f, 0.50f, "%.2f");\n            ImGui::SliderFloat("Collision", Settings::WheelFFBWallImpact.ptr(), 0.0f, 1.0f, "%.2f");\n            ImGui::Checkbox("Hardware GUID_Spring", Settings::WheelFFBUseHardwareSpring.ptr());\n            ImGui::SameLine();\n            ImGui::Checkbox("Hardware GUID_Damper", Settings::WheelFFBUseHardwareDamper.ptr());\n\n            if (ImGui::Button("Load simulation baseline"))\n            {\n                Settings::WheelFFBGlobalStrength = 0.50f;\n                Settings::WheelFFBSpringStrength = 0.45f;\n                Settings::WheelFFBLowSpeedSpring = 0.08f;\n                Settings::WheelFFBSpringLoadBoost = 0.35f;\n                Settings::WheelFFBDamperStrength = 0.35f;\n                Settings::WheelFFBSteeringWeight = 0.40f;\n                Settings::WheelFFBGripLoss = 0.65f;\n                Settings::WheelFFBRoadTexture = 0.12f;\n                Settings::WheelFFBTireSlip = 0.15f;\n                Settings::WheelFFBWallImpact = 0.50f;\n                Settings::WheelFFBUseHardwareSpring = true;\n                Settings::WheelFFBUseHardwareDamper = true;\n                status_ = "Loaded simulation baseline. Save the wheel profile after testing.";\n            }\n\n            ImGui::SeparatorText("Wheel options");\n            int deadzonePercent = int(float(Settings::SteeringDeadZone) * 100.0f + 0.5f);\n'''
if old not in text:
    raise SystemExit("FFB panel insertion point not found")
text = text.replace(old, new, 1)

old = '''            ImGui::TextDisabled(\n                "FFB tuning remains under Settings > WheelFFB. Wheel selection is shared with WheelFFB DeviceName; restart after switching to a different physical wheel.");\n'''
new = '''            ImGui::TextDisabled(\n                "Wheel selection is shared with WheelFFB DeviceName. Restart after switching to a different physical wheel; strength sliders update the live FFB calculations immediately where supported.");\n'''
if old not in text:
    raise SystemExit("footer replacement point not found")
text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
print("Added simulation FFB panel and universal-profile menu handoff")
