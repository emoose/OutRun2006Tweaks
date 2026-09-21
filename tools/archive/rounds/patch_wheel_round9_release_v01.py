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
        raise SystemExit(f'round9 {label}: expected exactly one match, found {count}')
    print(f'round9 release patch: {label}')
    return text.replace(old, new, 1)

# v0.1 defaults are the hardware-tested MOZA R3 personal tune: more steering
# resistance than the earlier generic defaults, without amplifying collisions.
ffb = rep(ffb,
'''    Setting<float> WheelFFBSpringStrength{\n        "WheelFFB", "SpringStrength", 0.45f,\n''',
'''    Setting<float> WheelFFBSpringStrength{\n        "WheelFFB", "SpringStrength", 0.60f,\n''', 'default spring 0.60')

ffb = rep(ffb,
'''    Setting<float> WheelFFBDamperStrength{\n        "WheelFFB", "DamperStrength", 0.10f,\n''',
'''    Setting<float> WheelFFBDamperStrength{\n        "WheelFFB", "DamperStrength", 0.42f,\n''', 'default damping 0.42')

ffb = rep(ffb,
'''    Setting<float> WheelFFBSteeringWeight{\n        "WheelFFB", "SteeringWeight", 0.45f,\n''',
'''    Setting<float> WheelFFBSteeringWeight{\n        "WheelFFB", "SteeringWeight", 0.38f,\n''', 'default cornering load 0.38')

ffb = rep(ffb,
'''    Setting<float> WheelFFBGripLoss{\n        "WheelFFB", "GripLoss", 0.60f,\n''',
'''    Setting<float> WheelFFBGripLoss{\n        "WheelFFB", "GripLoss", 0.65f,\n''', 'default grip-loss unload 0.65')

ffb = rep(ffb,
'''    Setting<float> WheelFFBWallImpact{\n        "WheelFFB", "WallImpact", 0.35f,\n''',
'''    Setting<float> WheelFFBWallImpact{\n        "WheelFFB", "WallImpact", 0.38f,\n''', 'default collision 0.38')

ffb = rep(ffb,
'''    Setting<float> WheelFFBTireSlip{\n        "WheelFFB", "TireSlip", 0.18f,\n''',
'''    Setting<float> WheelFFBTireSlip{\n        "WheelFFB", "TireSlip", 0.20f,\n''', 'default tire slip 0.20')

# Add the tested personal profile without removing the versioned comparison
# presets that were used during development.
old_presets = '''\t\tImGui::SeparatorText("Versioned baseline presets");\n\t\tif (ImGui::Button("Simulation Balanced v1"))\n\t\t\tapplyPreset(0.70f, 0.60f, 0.32f, 0.38f, 0.65f, 0.65f, 0.30f, 0.20f, 0.08f, 0.35f);\n'''
new_presets = '''\t\tImGui::SeparatorText("Versioned baseline presets");\n\t\tif (ImGui::Button("MOZA R3 v0.1 (default)"))\n\t\t\tapplyPreset(0.70f, 0.60f, 0.42f, 0.38f, 0.65f, 0.38f, 0.30f, 0.20f, 0.08f, 0.35f);\n\t\tif (ImGui::Button("Simulation Balanced v1"))\n\t\t\tapplyPreset(0.70f, 0.60f, 0.32f, 0.38f, 0.65f, 0.65f, 0.30f, 0.20f, 0.08f, 0.35f);\n'''
input_ui = rep(input_ui, old_presets, new_presets, 'add MOZA R3 v0.1 preset to bindings FFB panel')

input_ui = rep(input_ui,
'''\t\tImGui::TextDisabled("Preset names are immutable test references; adjust sliders afterward for a custom tune.");\n''',
'''\t\tImGui::TextDisabled("MOZA R3 v0.1 is the personal hardware-tested default; other presets remain comparison references.");\n''', 'describe tested v0.1 preset')

old_setup = '''            if (ImGui::Button("Load Simulation Balanced v1"))\n            {\n                Settings::WheelFFBGlobalStrength = 0.70f;\n                Settings::WheelFFBSpringStrength = 0.60f;\n                Settings::WheelFFBLowSpeedSpring = 0.08f;\n                Settings::WheelFFBSpringLoadBoost = 0.35f;\n                Settings::WheelFFBDamperStrength = 0.32f;\n                Settings::WheelFFBSteeringWeight = 0.38f;\n                Settings::WheelFFBGripLoss = 0.65f;\n                Settings::WheelFFBRoadTexture = 0.30f;\n                Settings::WheelFFBTireSlip = 0.20f;\n                Settings::WheelFFBWallImpact = 0.65f;\n                Settings::WheelFFBUseHardwareSpring = true;\n                Settings::WheelFFBUseHardwareDamper = true;\n                status_ = "Loaded Simulation Balanced v1. Test first, then save your preferred profile.";\n            }\n'''
new_setup = '''            if (ImGui::Button("Load MOZA R3 v0.1 (default)"))\n            {\n                Settings::WheelFFBGlobalStrength = 0.70f;\n                Settings::WheelFFBSpringStrength = 0.60f;\n                Settings::WheelFFBLowSpeedSpring = 0.08f;\n                Settings::WheelFFBSpringLoadBoost = 0.35f;\n                Settings::WheelFFBDamperStrength = 0.42f;\n                Settings::WheelFFBSteeringWeight = 0.38f;\n                Settings::WheelFFBGripLoss = 0.65f;\n                Settings::WheelFFBRoadTexture = 0.30f;\n                Settings::WheelFFBTireSlip = 0.20f;\n                Settings::WheelFFBWallImpact = 0.38f;\n                Settings::WheelFFBUseHardwareSpring = true;\n                Settings::WheelFFBUseHardwareDamper = true;\n                status_ = "Loaded MOZA R3 v0.1: stronger steering resistance with restrained collision feedback.";\n            }\n'''
setup_ui = rep(setup_ui, old_setup, new_setup, 'make F11 setup load MOZA R3 v0.1')

ffb_path.write_text(ffb, encoding='utf-8')
input_ui_path.write_text(input_ui, encoding='utf-8')
setup_ui_path.write_text(setup_ui, encoding='utf-8')
print('Applied Round-9 v0.1 release profile')
