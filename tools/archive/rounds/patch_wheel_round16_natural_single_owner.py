from pathlib import Path

ffb_path = Path('src/hooks_wheel_ffb.cpp')
ui_path = Path('src/overlay/wheel_setup_ui.cpp')
vibration_path = Path('src/hooks_forcefeedback.cpp')
ffb = ffb_path.read_text(encoding='utf-8')
ui = ui_path.read_text(encoding='utf-8')
vibration = vibration_path.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'round16 {label}: expected exactly one match, got {count}')
    print(f'ROUND16 patched: {label}')
    return text.replace(old, new, 1)


# The Xbox lateral/vibration signal is useful as a slow corner-load envelope,
# but its frame-to-frame steps are not tyre physics. Smooth it more heavily so
# an arcade state transition cannot become a steering torque step.
ffb = rep(ffb,
'''            const float alpha =
                std::abs(lateralRaw) > std::abs(smoothedLateral_) ? 0.25f : 0.20f;
''',
'''            const float alpha =
                std::abs(lateralRaw) > std::abs(smoothedLateral_) ? 0.18f : 0.12f;
''',
'lateral load smoothing')

# GUID_Spring must not stack a second high-speed centering law on top of SAT.
# Keep a firm parking/low-speed centre and smoothly fade it as the car gets
# moving. SAT becomes the main high-speed cornering force.
ffb = rep(ffb,
'''            const float lowSpeedSpring = std::clamp(
                static_cast<float>(Settings::WheelFFBLowSpeedSpring), 0.0f, 0.5f);
            const float speedCurve = std::clamp(
                lowSpeedSpring + (1.0f - lowSpeedSpring) *
                    std::pow(speedNorm, 1.60f),
                0.0f, 1.0f);
            const float cornerLoad = std::clamp(std::abs(latNorm), 0.0f, 1.0f);
            const float loadBoost = 1.0f +
                cornerLoad * static_cast<float>(Settings::WheelFFBSpringLoadBoost);

            const float springStrength =
                std::clamp(
                    static_cast<float>(Settings::WheelFFBSpringStrength) *
                    speedCurve * loadBoost * gripFactor,
                    0.0f, 1.0f);
''',
'''            const float cornerLoad = std::clamp(std::abs(latNorm), 0.0f, 1.0f);

            // Keep GUID_Spring as a low-speed/near-centre stabilizer instead of
            // stacking a second high-speed SAT on top of ConstantForce. The
            // fade is a smoothstep, so crossing the blend region cannot create
            // a coefficient step or a sudden return-to-centre kick.
            const float springFadeT = std::clamp(
                (speedNorm - 0.05f) / 0.35f, 0.0f, 1.0f);
            const float springFade =
                springFadeT * springFadeT * (3.0f - 2.0f * springFadeT);
            const float springSpeed = 1.0f - 0.88f * springFade;
            const float springGrip = 0.80f + 0.20f * gripFactor;
            const float springStrength = std::clamp(
                static_cast<float>(Settings::WheelFFBSpringStrength) *
                    springSpeed * springGrip,
                0.0f, 1.0f);
''',
'low-speed-only hardware spring')

# Round-15 deliberately made SAT very strong, but pow(angle, .58) makes a tiny
# input disproportionately large. Replace that arcade snap with a continuous
# pseudo pneumatic-trail curve. We cannot reconstruct full tyre contact-patch
# physics from OutRun, so steering sets restoring direction while speed and the
# smoothed lateral signal only shape magnitude. Fast inward wheel motion also
# relaxes SAT temporarily so it does not launch through centre.
ffb = rep(ffb,
'''            const float steerAbs = std::clamp(std::abs(steer), 0.0f, 1.0f);
            const float steerForSat = steerAbs > 0.012f
                ? std::pow((steerAbs - 0.012f) / 0.988f, 0.58f)
                : 0.0f;
            const float satSpeed = std::pow(speedNorm, 0.50f);
            const float satLoadBoost = 1.0f + 1.10f * cornerLoad;
            const float satSlip = std::clamp((driftAmt - 0.45f) / 0.55f, 0.0f, 1.0f);
            const float satGrip = 1.0f -
                static_cast<float>(Settings::WheelFFBGripLoss) *
                std::pow(satSlip, 1.35f);
            const float satStrength = std::clamp(
                static_cast<float>(Settings::WheelFFBSteeringWeight), 0.0f, 2.0f);
            const float selfAligningTorque =
                (steer >= 0.0f ? -1.0f : 1.0f) *
                steerForSat * satSpeed * satLoadBoost * satGrip * satStrength;
''',
'''            const float steerAbs = std::clamp(std::abs(steer), 0.0f, 1.0f);

            // Natural pseudo-SAT: a soft 2.5% centre deadband followed by a
            // sine-shaped pneumatic-trail curve. Unlike the previous power
            // curve, tiny steering angles stay tiny instead of immediately
            // generating a large ConstantForce. The curve remains progressive
            // through normal cornering angles and naturally flattens near lock.
            const float satAngleInput = std::clamp(
                (steerAbs - 0.025f) / 0.975f, 0.0f, 1.0f);
            constexpr float HalfPi = 1.57079632679f;
            const float steerForSat = std::sin(satAngleInput * HalfPi);

            // Motion gate removes SAT at rest, then builds it continuously once
            // the car is rolling. This retains strong loaded-corner steering
            // without the old square-root-like jump at small steering angles.
            const float satMotionT = std::clamp(
                (speedNorm - 0.015f) / 0.085f, 0.0f, 1.0f);
            const float satMotionGate =
                satMotionT * satMotionT * (3.0f - 2.0f * satMotionT);
            const float satSpeed = satMotionGate *
                (0.20f + 0.80f * std::sqrt(speedNorm));

            // OutRun exposes an arcade lateral signal rather than tyre
            // pneumatic trail. Use only its magnitude as a gentle load modifier
            // and never let its sign decide the FFB direction.
            const float cornerLoadSmooth =
                cornerLoad * cornerLoad * (3.0f - 2.0f * cornerLoad);
            const float satLoadBoost = 0.72f + 0.38f * cornerLoadSmooth;

            // Lateral magnitude is only a proxy for actual slip. Unload late and
            // smoothly so an ordinary loaded corner cannot repeatedly lose and
            // regain SAT as the Xbox vibration signal crosses a threshold.
            const float satSlipT = std::clamp(
                (driftAmt - 0.60f) / 0.35f, 0.0f, 1.0f);
            const float satSlip =
                satSlipT * satSlipT * (3.0f - 2.0f * satSlipT);
            const float satGrip = 1.0f -
                0.65f * static_cast<float>(Settings::WheelFFBGripLoss) * satSlip;

            // A real steering rack loses net aligning acceleration as the wheel
            // is already rotating quickly back toward centre. Apply a bounded
            // return-rate relief to stop a DD base from whipping through zero;
            // holding a corner (rate ~= 0) still gets the full SAT magnitude.
            const bool returningToCentre = steer * steerRate < 0.0f;
            const float returnRateT = returningToCentre
                ? std::clamp(std::abs(steerRate) / 0.08f, 0.0f, 1.0f)
                : 0.0f;
            const float returnRateSmooth =
                returnRateT * returnRateT * (3.0f - 2.0f * returnRateT);
            const float satReturnRelief = 1.0f - 0.45f * returnRateSmooth;

            const float satStrength = std::clamp(
                static_cast<float>(Settings::WheelFFBSteeringWeight), 0.0f, 2.0f);
            const float selfAligningTorque =
                (steer >= 0.0f ? -1.0f : 1.0f) *
                steerForSat * satSpeed * satLoadBoost * satGrip *
                satReturnRelief * satStrength;
''',
'natural progressive SAT curve')

# Acceleration history is coarse at 60 Hz and previously allowed +/-20..30%
# structural gain jumps. Keep the effect as subtle weight transfer only.
ffb = rep(ffb,
'''                loadMod = 1.0f + std::clamp(
                    -longAccel * static_cast<float>(Settings::WheelFFBWeightTransfer),
                    -0.20f, 0.30f);
''',
'''                loadMod = 1.0f + std::clamp(
                    -longAccel * static_cast<float>(Settings::WheelFFBWeightTransfer),
                    -0.06f, 0.08f);
''',
'bounded weight-transfer modulation')

# There is one FFB UI. Hide the [WheelFFB] registry entries from the generic
# Settings window so those duplicate sliders cannot look like a second backend.
ffb = rep(ffb,
'''        bool validate() override
        {
''',
'''        void declare_settings() override
        {
            // Wheel FFB has one dedicated F11 page. Keep every [WheelFFB]
            // setting out of the generic Settings window so there is no second
            // UI that looks like a competing backend.
            for (auto* setting : Settings::SettingBase::registry())
            {
                if (setting && setting->section() == "WheelFFB")
                    setting->hidden(true);
            }
        }

        bool validate() override
        {
''',
'hide duplicate generic WheelFFB settings')

# Dedicated Force Feedback page: expose only meaningful live controls. Legacy
# spring-shaping variables remain readable for old user.ini files but are no
# longer part of the active spring formula or shown as confusing extra knobs.
ui = rep(ui,
'''    extern Setting<float> WheelFFBSpringStrength;
    extern Setting<float> WheelFFBDamperStrength;
''',
'''    extern Setting<float> WheelFFBSpringStrength;
    extern Setting<float> WheelFFBSpringSaturation;
    extern Setting<float> WheelFFBDamperStrength;
''',
'F11 spring saturation setting')
ui = rep(ui,
'''    extern Setting<float> WheelFFBSpringLoadBoost;
    extern Setting<float> WheelFFBRoadTexture;
''',
'''    extern Setting<float> WheelFFBSpringLoadBoost;
    extern Setting<float> WheelFFBWeightTransfer;
    extern Setting<float> WheelFFBSlewRate;
    extern Setting<int> VibrationMode;
    extern Setting<float> WheelFFBRoadTexture;
''',
'F11 internal profile settings')
ui = rep(ui,
'''            ImGui::TextWrapped(
                "Self-aligning torque is the main cornering force: steering angle sets the return direction, speed and lateral load build torque, and deeper drift unloads it. Spring is now only a lighter centering backbone.");
''',
'''            ImGui::TextWrapped(
                "Single-owner wheel FFB: DirectInput COM only. SAT now rises smoothly from centre, builds with speed/corner load and unloads only in a deep slide. Centering Spring is mainly a low-speed stabilizer, so it no longer stacks a second strong high-speed return force.");
            ImGui::TextDisabled("Settings > WheelFFB is hidden; changes on this page apply live. SDL gamepad rumble is suppressed while wheel FFB is enabled.");
''',
'F11 single-owner explanation')
ui = rep(ui,
'''            ImGui::SliderFloat("Centering Spring", Settings::WheelFFBSpringStrength.ptr(), 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Low-speed Aligning", Settings::WheelFFBLowSpeedSpring.ptr(), 0.0f, 0.30f, "%.2f");
            ImGui::SliderFloat("Spring Corner-load Boost", Settings::WheelFFBSpringLoadBoost.ptr(), 0.0f, 0.80f, "%.2f");
            ImGui::SliderFloat("Dynamic Damping", Settings::WheelFFBDamperStrength.ptr(), 0.0f, 0.80f, "%.2f");
''',
'''            ImGui::SliderFloat("Centering Spring (low speed)", Settings::WheelFFBSpringStrength.ptr(), 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Dynamic Damping", Settings::WheelFFBDamperStrength.ptr(), 0.0f, 0.80f, "%.2f");
''',
'remove obsolete spring-shaping sliders')
ui = rep(ui,
'''            if (ImGui::Button("Load MOZA R3 Strong SAT"))
            {
''',
'''            if (ImGui::Button("Load MOZA R3 Natural SAT"))
            {
''',
'rename R3 natural preset')
ui = rep(ui,
'''                Settings::WheelFFBSpringStrength = 0.65f;
                Settings::WheelFFBLowSpeedSpring = 0.20f;
                Settings::WheelFFBSpringLoadBoost = 0.30f;
                Settings::WheelFFBDamperStrength = 0.30f;
                Settings::WheelFFBSteeringWeight = 1.45f;
                Settings::WheelFFBGripLoss = 0.65f;
''',
'''                Settings::WheelFFBSpringStrength = 0.65f;
                Settings::WheelFFBSpringSaturation = 0.95f;
                Settings::WheelFFBLowSpeedSpring = 0.20f;
                Settings::WheelFFBSpringLoadBoost = 0.30f;
                Settings::WheelFFBDamperStrength = 0.30f;
                Settings::WheelFFBSteeringWeight = 1.75f;
                Settings::WheelFFBGripLoss = 0.65f;
                Settings::WheelFFBWeightTransfer = 0.20f;
                Settings::WheelFFBSlewRate = 0.045f;
''',
'natural SAT profile values')
ui = rep(ui,
'''                Settings::WheelFFBInvertForce = true;
                Settings::WheelFFBInvertSpring = false;
                Settings::write(Module::UserIniPath);
                status_ = "Loaded MOZA R3 Strong SAT: corrected SAT direction, stronger return torque and a firmer centering spring. Saved to user.ini.";
''',
'''                Settings::WheelFFBInvertForce = true;
                Settings::WheelFFBInvertSpring = false;
                Settings::VibrationMode = 0;
                Settings::write(Module::UserIniPath);
                status_ = "Loaded MOZA R3 Natural SAT: smooth progressive SAT, low-speed-only spring assist and single DirectInput COM wheel FFB. Saved to user.ini.";
''',
'natural preset owns vibration/output')

# The original Xbox vibration hook may route to SDL_RumbleGamepad under
# UseNewInput. While native wheel FFB is active, suppress that independent
# rumble path so only WheelFFBEngine owns wheel force output.
vibration = rep(vibration,
'''namespace Settings
{
\tSetting<int> VibrationMode{ "Controls", "VibrationMode", 0,
''',
'''namespace Settings
{
    extern Setting<bool> WheelFFBEnable;
\tSetting<int> VibrationMode{ "Controls", "VibrationMode", 0,
''',
'declare native wheel FFB owner in vibration hook')
vibration = rep(vibration,
'''void SetVibration(int userId, float leftMotor, float rightMotor)
{
    if (!Settings::VibrationMode)
        return;
''',
'''void SetVibration(int userId, float leftMotor, float rightMotor)
{
    // With SDL multi-device input, wheel FFB is owned exclusively by the
    // DirectInput COM WheelFFBEngine. Do not let the independent gamepad-rumble
    // path reach a wheel/gamepad interface at the same time.
    if (Settings::UseNewInput && Settings::WheelFFBEnable)
        return;

    if (!Settings::VibrationMode)
        return;
''',
'suppress SDL rumble while native wheel FFB is enabled')

ffb_path.write_text(ffb, encoding='utf-8')
ui_path.write_text(ui, encoding='utf-8')
vibration_path.write_text(vibration, encoding='utf-8')
print('Applied Round-16 natural SAT, low-speed spring and single-owner FFB routing')
