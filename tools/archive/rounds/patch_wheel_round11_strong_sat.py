from pathlib import Path

ffb_path = Path('src/hooks_wheel_ffb.cpp')
input_ui_path = Path('src/overlay/input_bindings_ui.cpp')
setup_ui_path = Path('src/overlay/wheel_setup_ui.cpp')

ffb = ffb_path.read_text(encoding='utf-8')
input_ui = input_ui_path.read_text(encoding='utf-8')
setup_ui = setup_ui_path.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'round11 {label}: expected exactly one match, got {count}')
    print(f'ROUND11 patched: {label}')
    return text.replace(old, new, 1)


# Make self-aligning torque the primary cornering force. Keep the historical
# SteeringWeight INI key for config compatibility, but change its semantics/UI
# to explicit SAT strength. Generic GUID_Spring becomes a lighter backbone and
# GUID_Damper returns to a stabilising role rather than trying to create weight.
ffb = rep(ffb,
'''    Setting<float> WheelFFBSteeringWeight{\n        "WheelFFB", "SteeringWeight", 0.38f,\n        "Cornering load from OutRun lateral physics.", Range<float>{ 0.0f, 1.5f }\n    };\n''',
'''    Setting<float> WheelFFBSteeringWeight{\n        "WheelFFB", "SteeringWeight", 1.10f,\n        "Self-aligning torque strength. Uses steering angle, speed and OutRun lateral load; unloads only in deeper drift.", Range<float>{ 0.0f, 1.5f }\n    };\n''', 'SAT setting/default')

ffb = rep(ffb,
'''    Setting<float> WheelFFBSpringStrength{\n        "WheelFFB", "SpringStrength", 0.60f,\n''',
'''    Setting<float> WheelFFBSpringStrength{\n        "WheelFFB", "SpringStrength", 0.32f,\n''', 'reduce generic spring default')

ffb = rep(ffb,
'''    Setting<float> WheelFFBDamperStrength{\n        "WheelFFB", "DamperStrength", 0.42f,\n''',
'''    Setting<float> WheelFFBDamperStrength{\n        "WheelFFB", "DamperStrength", 0.34f,\n''', 'damping becomes stabilizer')

ffb = rep(ffb,
'''    Setting<float> WheelFFBSpringLoadBoost{\n        "WheelFFB", "SpringLoadBoost", 0.35f,\n''',
'''    Setting<float> WheelFFBSpringLoadBoost{\n        "WheelFFB", "SpringLoadBoost", 0.18f,\n''', 'spring load boost reduced')

ffb = rep(ffb,
'''            const float lateral =\n                latNorm * speedNorm *\n                static_cast<float>(Settings::WheelFFBSteeringWeight) *\n                gripFactor;\n\n            float loadMod = 1.0f;\n''',
'''            // Strong sim-style pseudo self-aligning torque (SAT). OutRun does\n            // not expose tyre pneumatic trail directly, so use the real steering\n            // angle as torque direction and the game's smoothed lateral signal as\n            // a load magnitude. This is intentionally NOT the old signed lateral\n            // ConstantForce, which could partially cancel the centering spring.\n            //\n            // - steering angle determines restoring direction (always toward centre)\n            // - vehicle speed builds SAT progressively\n            // - lateral load makes a loaded corner heavier\n            // - deeper drift unloads SAT so loss of grip is felt in the wheel\n            const float steerAbs = std::clamp(std::abs(steer), 0.0f, 1.0f);\n            const float steerForSat = steerAbs > 0.012f\n                ? std::pow((steerAbs - 0.012f) / 0.988f, 0.78f)\n                : 0.0f;\n            const float satSpeed = std::pow(speedNorm, 0.62f);\n            const float satLoadBoost = 1.0f + 0.80f * cornerLoad;\n            const float satSlip = std::clamp((driftAmt - 0.35f) / 0.65f, 0.0f, 1.0f);\n            const float satGrip = 1.0f -\n                static_cast<float>(Settings::WheelFFBGripLoss) *\n                std::pow(satSlip, 1.35f);\n            const float satStrength = std::clamp(\n                static_cast<float>(Settings::WheelFFBSteeringWeight), 0.0f, 1.5f);\n            const float selfAligningTorque =\n                (steer >= 0.0f ? -1.0f : 1.0f) *\n                steerForSat * satSpeed * satLoadBoost * satGrip * satStrength;\n\n            float loadMod = 1.0f;\n''', 'replace signed lateral with pseudo SAT')

ffb = rep(ffb,
'''            float structural = 0.0f;\n            if (crashImpulseTimer_ <= CrashCooldownFrames)\n                structural = (softwareSpring + lateral) * loadMod + damper;\n''',
'''            float structural = 0.0f;\n            if (crashImpulseTimer_ <= CrashCooldownFrames)\n                structural = (softwareSpring + selfAligningTorque) * loadMod + damper;\n''', 'SAT in structural force')

# F11 controls and a strong R3 SAT test profile. Old comparison presets stay.
input_ui = rep(input_ui,
'''\t\tif (ImGui::Button("MOZA R3 v0.1 (default)"))\n\t\t\tapplyPreset(0.70f, 0.60f, 0.42f, 0.38f, 0.65f, 0.38f, 0.30f, 0.20f, 0.08f, 0.35f);\n''',
'''\t\tif (ImGui::Button("MOZA R3 SAT test"))\n\t\t\tapplyPreset(0.70f, 0.32f, 0.34f, 1.10f, 0.65f, 0.38f, 0.30f, 0.20f, 0.08f, 0.18f);\n''', 'R3 SAT preset')

input_ui = rep(input_ui,
'''\t\tImGui::TextDisabled("MOZA R3 v0.1 is the personal hardware-tested default; other presets remain comparison references.");\n''',
'''\t\tImGui::TextDisabled("MOZA R3 SAT test makes self-aligning torque the main steering force; old presets remain comparison references.");\n''', 'SAT preset description')

input_ui = rep(input_ui,
'''\t\tif (ImGui::SliderFloat("Aligning / centering", Settings::WheelFFBSpringStrength.ptr(), 0.0f, 1.5f, "%.2f"))\n\t\t\tsetting_changed(Settings::WheelFFBSpringStrength);\n\t\tif (ImGui::SliderFloat("Dynamic damping", Settings::WheelFFBDamperStrength.ptr(), 0.0f, 1.0f, "%.2f"))\n\t\t\tsetting_changed(Settings::WheelFFBDamperStrength);\n\t\tif (ImGui::SliderFloat("Cornering load", Settings::WheelFFBSteeringWeight.ptr(), 0.0f, 1.5f, "%.2f"))\n\t\t\tsetting_changed(Settings::WheelFFBSteeringWeight);\n''',
'''\t\tif (ImGui::SliderFloat("Low-speed centering spring", Settings::WheelFFBSpringStrength.ptr(), 0.0f, 1.5f, "%.2f"))\n\t\t\tsetting_changed(Settings::WheelFFBSpringStrength);\n\t\tif (ImGui::SliderFloat("Dynamic damping", Settings::WheelFFBDamperStrength.ptr(), 0.0f, 1.0f, "%.2f"))\n\t\t\tsetting_changed(Settings::WheelFFBDamperStrength);\n\t\tif (ImGui::SliderFloat("Self-aligning torque (SAT)", Settings::WheelFFBSteeringWeight.ptr(), 0.0f, 1.5f, "%.2f"))\n\t\t\tsetting_changed(Settings::WheelFFBSteeringWeight);\n''', 'bindings UI SAT labels')

setup_ui = rep(setup_ui,
'''            ImGui::TextWrapped(\n                "Aligning force grows strongly with speed and corner load, unloads with grip loss, and GUID_Damper resists steering velocity without acting like a centering spring.");\n''',
'''            ImGui::TextWrapped(\n                "Self-aligning torque is the main cornering force: steering angle sets the return direction, speed and lateral load build torque, and deeper drift unloads it. Spring is now only a lighter centering backbone.");\n''', 'setup SAT explanation')

setup_ui = rep(setup_ui,
'''            ImGui::SliderFloat("Aligning / Spring", Settings::WheelFFBSpringStrength.ptr(), 0.0f, 1.0f, "%.2f");\n            ImGui::SliderFloat("Low-speed Aligning", Settings::WheelFFBLowSpeedSpring.ptr(), 0.0f, 0.30f, "%.2f");\n            ImGui::SliderFloat("Corner-load Boost", Settings::WheelFFBSpringLoadBoost.ptr(), 0.0f, 0.80f, "%.2f");\n            ImGui::SliderFloat("Dynamic Damping", Settings::WheelFFBDamperStrength.ptr(), 0.0f, 0.80f, "%.2f");\n            ImGui::SliderFloat("Lateral / Steering Weight", Settings::WheelFFBSteeringWeight.ptr(), 0.0f, 1.0f, "%.2f");\n''',
'''            ImGui::SliderFloat("Centering Spring", Settings::WheelFFBSpringStrength.ptr(), 0.0f, 1.0f, "%.2f");\n            ImGui::SliderFloat("Low-speed Aligning", Settings::WheelFFBLowSpeedSpring.ptr(), 0.0f, 0.30f, "%.2f");\n            ImGui::SliderFloat("Spring Corner-load Boost", Settings::WheelFFBSpringLoadBoost.ptr(), 0.0f, 0.80f, "%.2f");\n            ImGui::SliderFloat("Dynamic Damping", Settings::WheelFFBDamperStrength.ptr(), 0.0f, 0.80f, "%.2f");\n            ImGui::SliderFloat("Self-aligning Torque (SAT)", Settings::WheelFFBSteeringWeight.ptr(), 0.0f, 1.50f, "%.2f");\n''', 'setup SAT sliders')

setup_ui = rep(setup_ui,
'''            if (ImGui::Button("Load MOZA R3 v0.1 (default)"))\n            {\n                Settings::WheelFFBGlobalStrength = 0.70f;\n                Settings::WheelFFBSpringStrength = 0.60f;\n                Settings::WheelFFBLowSpeedSpring = 0.08f;\n                Settings::WheelFFBSpringLoadBoost = 0.35f;\n                Settings::WheelFFBDamperStrength = 0.42f;\n                Settings::WheelFFBSteeringWeight = 0.38f;\n                Settings::WheelFFBGripLoss = 0.65f;\n                Settings::WheelFFBRoadTexture = 0.30f;\n                Settings::WheelFFBTireSlip = 0.20f;\n                Settings::WheelFFBWallImpact = 0.38f;\n                Settings::WheelFFBUseHardwareSpring = true;\n                Settings::WheelFFBUseHardwareDamper = true;\n                status_ = "Loaded MOZA R3 v0.1: stronger steering resistance with restrained collision feedback.";\n            }\n''',
'''            if (ImGui::Button("Load MOZA R3 SAT test"))\n            {\n                Settings::WheelFFBGlobalStrength = 0.70f;\n                Settings::WheelFFBSpringStrength = 0.32f;\n                Settings::WheelFFBLowSpeedSpring = 0.08f;\n                Settings::WheelFFBSpringLoadBoost = 0.18f;\n                Settings::WheelFFBDamperStrength = 0.34f;\n                Settings::WheelFFBSteeringWeight = 1.10f;\n                Settings::WheelFFBGripLoss = 0.65f;\n                Settings::WheelFFBRoadTexture = 0.30f;\n                Settings::WheelFFBTireSlip = 0.20f;\n                Settings::WheelFFBWallImpact = 0.38f;\n                Settings::WheelFFBUseHardwareSpring = true;\n                Settings::WheelFFBUseHardwareDamper = true;\n                status_ = "Loaded MOZA R3 SAT test: strong speed/load-dependent return torque with lighter generic spring.";\n            }\n''', 'setup R3 SAT preset')

ffb_path.write_text(ffb, encoding='utf-8')
input_ui_path.write_text(input_ui, encoding='utf-8')
setup_ui_path.write_text(setup_ui, encoding='utf-8')
print('Applied Round-11 strong pseudo-SAT model')
