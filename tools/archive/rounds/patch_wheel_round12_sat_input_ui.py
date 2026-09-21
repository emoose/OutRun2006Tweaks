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
        raise SystemExit(f'round12 {label}: expected exactly one match, got {count}')
    print(f'ROUND12 patched: {label}')
    return text.replace(old, new, 1)


# 1) SAT must use the exact same steering value as the active SDL3 multi-device
# input path. Calling the game's GetVolume address indirectly worked for legacy
# DirectInput, but it is needlessly dependent on the inline-hook chain when
# UseNewInput is enabled. Round11 made SAT the dominant force, so a zero/stale
# value here makes almost all steering feedback disappear. Use the exported
# InputManager steering value directly for the new-input path and preserve the
# original game function only for legacy compatibility mode.
ffb = rep(
    ffb,
'''extern double __cdecl sub_1149C0(unsigned int surfaceMask, int loadColiType, DWORD* waterFlag);\n\nnamespace Settings\n{\n''',
'''extern double __cdecl sub_1149C0(unsigned int surfaceMask, int loadColiType, DWORD* waterFlag);\nextern float InputManager_SteeringValue();\n\nnamespace Settings\n{\n    extern Setting<bool> UseNewInput;\n''',
    'declare direct multi-device steering source')

ffb = rep(
    ffb,
'''        float read_game_steering() const\n        {\n            using GetVolumeFn = int(__cdecl*)(ADChannel);\n            auto getVolume = Module::fn_ptr<GetVolumeFn>(0x53720);\n            if (!getVolume)\n                return 0.0f;\n\n            const int raw = getVolume(ADChannel::Steering);\n            return std::clamp(raw / 127.0f, -1.0f, 1.0f);\n        }\n''',
'''        float read_game_steering() const\n        {\n            if (Settings::UseNewInput)\n            {\n                const float steering = InputManager_SteeringValue();\n                return std::isfinite(steering)\n                    ? std::clamp(steering, -1.0f, 1.0f)\n                    : 0.0f;\n            }\n\n            using GetVolumeFn = int(__cdecl*)(ADChannel);\n            auto getVolume = Module::fn_ptr<GetVolumeFn>(0x53720);\n            if (!getVolume)\n                return 0.0f;\n\n            const int raw = getVolume(ADChannel::Steering);\n            return std::clamp(raw / 127.0f, -1.0f, 1.0f);\n        }\n''',
    'SAT reads SDL steering directly')

# Include SAT itself and the steering-source label in the two-second diagnostic
# line. If a particular wheel still reports no torque, one log now tells us
# whether steering/SAT or the DirectInput output stage is at fault.
ffb = rep(
    ffb,
'''            maybe_log(speedNorm, steer, steerRate, driftAmt, roughness, level);\n''',
'''            maybe_log(speedNorm, steer, steerRate, driftAmt, roughness, selfAligningTorque, level);\n''',
    'pass SAT torque to diagnostics')

ffb = rep(
    ffb,
'''        void maybe_log(\n            float speedNorm,\n            float steer,\n            float steerRate,\n            float driftAmt,\n            float roughness,\n            LONG level)\n''',
'''        void maybe_log(\n            float speedNorm,\n            float steer,\n            float steerRate,\n            float driftAmt,\n            float roughness,\n            float satTorque,\n            LONG level)\n''',
    'diagnostic signature carries SAT')

ffb = rep(
    ffb,
'''                "WheelFFB DIAG: spd={:.2f} steer={:.3f} rate={:.4f} lat={:.2f} drift={:.2f} rough={:.2f} out={} invCF={} spring={} invSpring={} coeff={} damper={} dcoeff={} periodic={}",\n                speedNorm,\n                steer,\n                steerRate,\n                smoothedLateral_,\n                driftAmt,\n                roughness,\n                static_cast<int>(level),\n''',
'''                "WheelFFB DIAG: spd={:.2f} steer={:.3f} rate={:.4f} lat={:.2f} drift={:.2f} rough={:.2f} sat={:.3f} steerSrc={} out={} invCF={} spring={} invSpring={} coeff={} damper={} dcoeff={} periodic={}",\n                speedNorm,\n                steer,\n                steerRate,\n                smoothedLateral_,\n                driftAmt,\n                roughness,\n                satTorque,\n                Settings::UseNewInput ? "SDL" : "legacy",\n                static_cast<int>(level),\n''',
    'diagnostics identify SAT and steering source')


# 2) Remove the duplicate Force Feedback page from the in-game Controls/input
# binding dialog. That dialog now owns only SDL input/binding/calibration. FFB
# device selection and tuning live in one F11 shell tab below.
input_ui = rep(
    input_ui,
'''\n\t\t\t\tif (ImGui::BeginTabItem("Force Feedback"))\n\t\t\t\t{\n\t\t\t\t\tdraw_force_feedback();\n\t\t\t\t\tImGui::EndTabItem();\n\t\t\t\t}\n''',
'''\n''',
    'remove duplicate FFB tab from Controls dialog')

# 3) The main Wheel Setup page mixed a legacy DirectInput input mapper with the
# current FFB controls. In the default SDL3 path, present it explicitly as the
# one Force Feedback page and hide legacy bindings/options. When compatibility
# input is deliberately selected, retain the old legacy setup functionality.
setup_ui = rep(
    setup_ui,
'''    extern Setting<float> SteeringDeadZone;\n''',
'''    extern Setting<float> SteeringDeadZone;\n    extern Setting<bool> UseNewInput;\n    extern Setting<bool> WheelFFBEnable;\n''',
    'wheel setup knows active input path and FFB enable')

setup_ui = rep(
    setup_ui,
'''        Kind kind() const override { return Kind::Tab; }\n        const char* name() const override { return "Wheel Setup"; }\n''',
'''        Kind kind() const override { return Kind::Tab; }\n        const char* name() const override\n        {\n            return Settings::UseNewInput ? "Force Feedback" : "Legacy Wheel Setup";\n        }\n''',
    'rename main tab by role')

setup_ui = rep(
    setup_ui,
'''            ImGui::TextWrapped(\n                "Universal DirectInput wheel setup. Bind here from a game menu; gameplay FFB follows the exact selected DirectInput GUID.");\n''',
'''            if (Settings::UseNewInput)\n            {\n                ImGui::TextWrapped(\n                    "Force feedback only. Configure steering, pedals, buttons and calibration in the game Controls / Controller Setup screen. This page selects the DirectInput FFB wheel and tunes its forces.");\n            }\n            else\n            {\n                ImGui::TextWrapped(\n                    "Legacy DirectInput wheel setup. Use this page for compatibility-mode input bindings and force feedback.");\n            }\n''',
    'explain single-purpose FFB page')

setup_ui = rep(
    setup_ui,
'''            const int regularSlots = UniversalWheelProfile::regular_device_count();\n''',
'''            if (!Settings::UseNewInput)\n            {\n            const int regularSlots = UniversalWheelProfile::regular_device_count();\n''',
    'hide legacy input mapper in SDL mode start')

setup_ui = rep(
    setup_ui,
'''            if (ImGui::Button("MOZA R3/ES menu defaults"))\n                apply_r3_menu_defaults();\n\n            ImGui::SeparatorText("Simulation FFB");\n''',
'''            if (ImGui::Button("MOZA R3/ES menu defaults"))\n                apply_r3_menu_defaults();\n            }\n\n            ImGui::SeparatorText("Simulation FFB");\n''',
    'hide legacy input mapper in SDL mode end')

# Selecting a wheel in the FFB page must persist even though the legacy Save
# Wheel Profile controls are hidden in SDL mode.
setup_ui = rep(
    setup_ui,
'''            gReader.select_by_identity(info.guidKey, info.name);\n            status_ = UniversalWheelProfile::regular_device_count() > 1\n                ? "Selected wheel/FFB device. Confirm the OutRun legacy input slot below when multiple controllers are present."\n                : "Selected wheel; FFB device follows this product name.";\n''',
'''            gReader.select_by_identity(info.guidKey, info.name);\n            Settings::write(Module::UserIniPath);\n            if (Settings::UseNewInput)\n                status_ = "Selected FFB wheel and saved its exact DirectInput GUID. Restart the game after changing physical wheel.";\n            else\n                status_ = UniversalWheelProfile::regular_device_count() > 1\n                    ? "Selected wheel/FFB device. Confirm the OutRun legacy input slot below when multiple controllers are present."\n                    : "Selected wheel; FFB device follows this product name.";\n''',
    'persist FFB device selection without legacy save')

# Loading the SAT test profile should recover from a previously disabled FFB
# setting as well as loading all force values.
setup_ui = rep(
    setup_ui,
'''            if (ImGui::Button("Load MOZA R3 SAT test"))\n            {\n                Settings::WheelFFBGlobalStrength = 0.70f;\n''',
'''            if (ImGui::Button("Load MOZA R3 SAT test"))\n            {\n                Settings::WheelFFBEnable = true;\n                Settings::WheelFFBGlobalStrength = 0.70f;\n''',
    'SAT preset explicitly enables FFB')

# Wheel options below the FFB controls are legacy input/profile settings; hide
# them in the normal SDL3 path so there is no second steering configuration.
setup_ui = rep(
    setup_ui,
'''            ImGui::SeparatorText("Wheel options");\n            int deadzonePercent = int(float(Settings::SteeringDeadZone) * 100.0f + 0.5f);\n''',
'''            if (!Settings::UseNewInput)\n            {\n            ImGui::SeparatorText("Wheel options");\n            int deadzonePercent = int(float(Settings::SteeringDeadZone) * 100.0f + 0.5f);\n''',
    'hide legacy wheel options in SDL mode start')

setup_ui = rep(
    setup_ui,
'''            if (ImGui::Button("Save Wheel Profile"))\n                save();\n\n            if (target_ != BindTarget::None)\n''',
'''            if (ImGui::Button("Save Wheel Profile"))\n                save();\n            }\n\n            if (target_ != BindTarget::None)\n''',
    'hide legacy wheel options in SDL mode end')

setup_ui = rep(
    setup_ui,
'''            ImGui::TextDisabled(\n                "Wheel selection is shared with WheelFFB DeviceName. Restart after switching to a different physical wheel; strength sliders update the live FFB calculations immediately where supported.");\n''',
'''            ImGui::TextDisabled(\n                Settings::UseNewInput\n                    ? "Input setup: game Controls / Controller Setup. FFB setup: this page only. Restart after switching to a different physical wheel."\n                    : "Legacy compatibility input and FFB are configured on this page. Restart after switching to a different physical wheel.");\n''',
    'final setup navigation guidance')

ffb_path.write_text(ffb, encoding='utf-8')
input_ui_path.write_text(input_ui, encoding='utf-8')
setup_ui_path.write_text(setup_ui, encoding='utf-8')
print('Applied Round-12 SAT steering-source fix and UI consolidation')
