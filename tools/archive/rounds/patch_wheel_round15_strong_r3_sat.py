from pathlib import Path

ffb_path = Path('src/hooks_wheel_ffb.cpp')
ui_path = Path('src/overlay/wheel_setup_ui.cpp')
ffb = ffb_path.read_text(encoding='utf-8')
ui = ui_path.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'round15 {label}: expected exactly one match, got {count}')
    print(f'ROUND15 patched: {label}')
    return text.replace(old, new, 1)


# R3 hardware feedback from the Round-11/12 build showed that the structural
# steering forces were still too light and that the ConstantForce/SAT direction
# was reversed. Keep collision/road/tire levels unchanged and strengthen only
# the steering structure: hardware spring plus pseudo-SAT.
ffb = rep(ffb,
    '"WheelFFB", "SpringStrength", 0.32f,',
    '"WheelFFB", "SpringStrength", 0.65f,',
    'stronger centering spring default')
ffb = rep(ffb,
    '"WheelFFB", "LowSpeedSpring", 0.08f,',
    '"WheelFFB", "LowSpeedSpring", 0.20f,',
    'retain noticeable low-speed centering')
ffb = rep(ffb,
    '"WheelFFB", "SpringLoadBoost", 0.18f,',
    '"WheelFFB", "SpringLoadBoost", 0.30f,',
    'stronger spring corner-load response')
ffb = rep(ffb,
    '"WheelFFB", "DamperStrength", 0.34f,',
    '"WheelFFB", "DamperStrength", 0.30f,',
    'damping remains a stabilizer')
ffb = rep(ffb,
    '''"WheelFFB", "SteeringWeight", 1.10f,
        "Self-aligning torque strength. Uses steering angle, speed and OutRun lateral load; unloads only in deeper drift.", Range<float>{ 0.0f, 1.5f }''',
    '''"WheelFFB", "SteeringWeight", 1.45f,
        "Self-aligning torque strength. Uses steering angle, speed and OutRun lateral load; unloads only in deeper drift.", Range<float>{ 0.0f, 2.0f }''',
    'stronger SAT setting and headroom')

# MOZA R3 ConstantForce actuator direction is opposite the logical steering sign
# used by the pseudo-SAT path. Hardware GUID_Spring has its own independent sign
# and remains positive/normal on R3.
ffb = rep(ffb,
    '"WheelFFB", "InvertForce", false,',
    '"WheelFFB", "InvertForce", true,',
    'R3 ConstantForce direction default')

# Make SAT clearly present in ordinary 20-50 degree corners rather than only at
# large steering angles. Speed still gates SAT to zero when stationary, while
# hardware spring supplies the parking/low-speed centering backbone.
ffb = rep(ffb,
    '? std::pow((steerAbs - 0.012f) / 0.988f, 0.78f)',
    '? std::pow((steerAbs - 0.012f) / 0.988f, 0.58f)',
    'increase SAT response at moderate steering angle')
ffb = rep(ffb,
    'const float satSpeed = std::pow(speedNorm, 0.62f);',
    'const float satSpeed = std::pow(speedNorm, 0.50f);',
    'increase SAT at normal cornering speeds')
ffb = rep(ffb,
    'const float satLoadBoost = 1.0f + 0.80f * cornerLoad;',
    'const float satLoadBoost = 1.0f + 1.10f * cornerLoad;',
    'increase SAT corner-load boost')
ffb = rep(ffb,
    'const float satSlip = std::clamp((driftAmt - 0.35f) / 0.65f, 0.0f, 1.0f);',
    'const float satSlip = std::clamp((driftAmt - 0.45f) / 0.55f, 0.0f, 1.0f);',
    'retain SAT until deeper drift')
ffb = rep(ffb,
    'static_cast<float>(Settings::WheelFFBSteeringWeight), 0.0f, 1.5f);',
    'static_cast<float>(Settings::WheelFFBSteeringWeight), 0.0f, 2.0f);',
    'SAT runtime clamp matches setting range')

# Make the F11 controls describe the actual R3 behavior and provide one preset
# that both fixes direction and restores strong steering weight. Persist it so
# an old user.ini cannot silently restore the previous weak/reversed values.
ui = rep(ui,
    'ImGui::SliderFloat("Self-aligning Torque (SAT)", Settings::WheelFFBSteeringWeight.ptr(), 0.0f, 1.50f, "%.2f");',
    'ImGui::SliderFloat("Self-aligning Torque (SAT)", Settings::WheelFFBSteeringWeight.ptr(), 0.0f, 2.00f, "%.2f");',
    'expand SAT slider range')
ui = rep(ui,
    'ImGui::Checkbox("Reverse ConstantForce", Settings::WheelFFBInvertForce.ptr());',
    'ImGui::Checkbox("Reverse SAT / ConstantForce", Settings::WheelFFBInvertForce.ptr());',
    'clarify SAT direction control')
ui = rep(ui,
    'if (ImGui::Button("Load MOZA R3 SAT test"))',
    'if (ImGui::Button("Load MOZA R3 Strong SAT"))',
    'rename strong R3 preset')
ui = rep(ui,
    '''Settings::WheelFFBSpringStrength = 0.32f;
                Settings::WheelFFBLowSpeedSpring = 0.08f;
                Settings::WheelFFBSpringLoadBoost = 0.18f;
                Settings::WheelFFBDamperStrength = 0.34f;
                Settings::WheelFFBSteeringWeight = 1.10f;''',
    '''Settings::WheelFFBSpringStrength = 0.65f;
                Settings::WheelFFBLowSpeedSpring = 0.20f;
                Settings::WheelFFBSpringLoadBoost = 0.30f;
                Settings::WheelFFBDamperStrength = 0.30f;
                Settings::WheelFFBSteeringWeight = 1.45f;''',
    'strong R3 structural-force preset')
ui = rep(ui,
    '''Settings::WheelFFBUseHardwareSpring = true;
                Settings::WheelFFBUseHardwareDamper = true;
                status_ = "Loaded MOZA R3 SAT test: strong speed/load-dependent return torque with lighter generic spring.";''',
    '''Settings::WheelFFBUseHardwareSpring = true;
                Settings::WheelFFBUseHardwareDamper = true;
                Settings::WheelFFBInvertForce = true;
                Settings::WheelFFBInvertSpring = false;
                Settings::write(Module::UserIniPath);
                status_ = "Loaded MOZA R3 Strong SAT: corrected SAT direction, stronger return torque and a firmer centering spring. Saved to user.ini.";''',
    'R3 preset fixes direction and persists')

ffb_path.write_text(ffb, encoding='utf-8')
ui_path.write_text(ui, encoding='utf-8')
print('Applied Round-15 strong MOZA R3 SAT/centering and direction correction')
