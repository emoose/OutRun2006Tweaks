from pathlib import Path

ffb_path = Path('src/hooks_wheel_ffb.cpp')
input_path = Path('src/input_manager.cpp')
ui_path = Path('src/overlay/input_bindings_ui.cpp')
setup_path = Path('src/overlay/wheel_setup_ui.cpp')
ini_path = Path('OutRun2006Tweaks.ini')

ffb = ffb_path.read_text(encoding='utf-8')
ims = input_path.read_text(encoding='utf-8')
uis = ui_path.read_text(encoding='utf-8')
setup = setup_path.read_text(encoding='utf-8')
inis = ini_path.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected exactly one match, found {count}')
    print(f'community patch: {label}')
    return text.replace(old, new, 1)


def first_insert(text: str, needle: str, insertion: str, label: str) -> str:
    pos = text.find(needle)
    if pos < 0:
        raise SystemExit(f'{label}: insertion marker not found')
    print(f'community patch: {label}')
    return text[:pos] + insertion + text[pos:]


def replace_between(text: str, start: str, end: str, replacement: str, label: str) -> str:
    a = text.find(start)
    if a < 0:
        raise SystemExit(f'{label}: start marker not found')
    b = text.find(end, a)
    if b < 0:
        raise SystemExit(f'{label}: end marker not found')
    print(f'community patch: {label}')
    return text[:a] + replacement + text[b:]


# ---------------------------------------------------------------------------
# 1. Finish the hybrid input migration. New installs use the multi-device path;
#    the proven original-game DirectInput path remains an explicit fallback.
# ---------------------------------------------------------------------------
if 'Setting<bool> WheelInputCompatibility{' not in ims:
    ims = rep(
        ims,
        '\tSetting<bool> UseNewInput{ "Controls", "UseNewInput", true,\n',
        '''\tSetting<bool> WheelInputCompatibility{ "Controls", "WheelInputCompatibility", false,\n\t\t"Use the original-game DirectInput wheel path as a compatibility fallback. Disable UseNewInput when enabling this." };\n\n\tSetting<bool> UseNewInput{ "Controls", "UseNewInput", true,\n''',
        'restore WheelInputCompatibility definition')

if 'WheelInputCompatibility = true' in inis:
    inis = inis.replace('WheelInputCompatibility = true', 'WheelInputCompatibility = false', 1)
if 'UseNewInput = false' in inis:
    inis = inis.replace('UseNewInput = false', 'UseNewInput = true', 1)


# ---------------------------------------------------------------------------
# 2. d-b-c-e toolkit v0.8.0 signal-order lesson.
#    Master strength and inversion belong before tanh/slew/device limits.
#    DirectInput dwGain stays at 100%; 100-150% is model headroom, not a driver
#    gain outside the DirectInput 0..10000 range.
# ---------------------------------------------------------------------------
ffb = rep(
    ffb,
'''    Setting<float> WheelFFBGlobalStrength{\n        "WheelFFB", "GlobalStrength", 0.25f,\n        "Master DirectInput effect gain. Start low on direct-drive wheels.", Range<float>{ 0.0f, 1.0f }\n    };\n''',
'''    Setting<float> WheelFFBGlobalStrength{\n        "WheelFFB", "GlobalStrength", 0.70f,\n        "Master force-model gain. Applied before soft saturation/slew; 1.0=100%, 1.5=150% headroom.", Range<float>{ 0.0f, 1.5f }\n    };\n''',
    'v0.8-style pre-shaper master gain')

ffb = rep(
    ffb,
'''    Setting<float> WheelFFBRoadTexture{\n        "WheelFFB", "RoadTexture", 0.20f,\n''',
'''    Setting<float> WheelFFBRoadTexture{\n        "WheelFFB", "RoadTexture", 0.30f,\n''',
    'slightly stronger road-detail baseline')

ffb = rep(
    ffb,
'''            const float speed = car->field_1C4;\n            const float speedNorm = std::clamp(speed / 2.0f, 0.0f, 1.0f);\n''',
'''            const float speed = car->field_1C4;\n            const float speedNorm = std::clamp(speed / 2.0f, 0.0f, 1.0f);\n            const float outputStrength = std::clamp(\n                static_cast<float>(Settings::WheelFFBGlobalStrength), 0.0f, 1.5f);\n''',
    'cache 0-150 percent model gain')

ffb = rep(
    ffb,
'''            float roadAmp =\n                textureRoughness * roadSpeedGate *\n                static_cast<float>(Settings::WheelFFBRoadTexture);\n''',
'''            float roadAmp =\n                textureRoughness * roadSpeedGate *\n                static_cast<float>(Settings::WheelFFBRoadTexture) * outputStrength;\n''',
    'master gain on road periodic/fallback')

ffb = rep(
    ffb,
'                slipAmp = driftAmt * static_cast<float>(Settings::WheelFFBTireSlip);\n',
'                slipAmp = driftAmt * static_cast<float>(Settings::WheelFFBTireSlip) * outputStrength;\n',
    'master gain on tire-slip periodic')

ffb = rep(
    ffb,
'''                slipAmp =\n                    static_cast<float>(Settings::WheelFFBEngineIdle) * throttleNorm;\n''',
'''                slipAmp =\n                    static_cast<float>(Settings::WheelFFBEngineIdle) * throttleNorm * outputStrength;\n''',
    'master gain on idle periodic')

ffb = rep(
    ffb,
'''                splashAmp_ =\n                    (roughness - 0.7f) * speedNorm * 0.75f * roadTextureScale;\n''',
'''                splashAmp_ =\n                    (roughness - 0.7f) * speedNorm * 0.75f * roadTextureScale * outputStrength;\n''',
    'master gain on splash detail')

ffb = rep(
    ffb,
'                        : springStrength * warmupScale * recreateScale);\n',
'                        : springStrength * warmupScale * recreateScale * outputStrength);\n',
    'master gain on hardware spring')

ffb = rep(
    ffb,
'                update_damper(dynamicDamperStrength * warmupScale * recreateScale);\n',
'                update_damper(dynamicDamperStrength * warmupScale * recreateScale * outputStrength);\n',
    'master gain on hardware damper')

ffb = rep(
    ffb,
'''            float total = structural + events;\n            if (Settings::WheelFFBInvertForce)\n''',
'''            // d-b-c-e toolkit v0.8.0 ordering: gain/invert are part of the\n            // signal before soft saturation and slew. This prevents high gain\n            // or inversion from disagreeing with the slew limiter's last value.\n            float total = (structural + events) * outputStrength;\n            if (Settings::WheelFFBInvertForce)\n''',
    'gain/invert before tanh and slew')

old_gain = '''effect.dwGain = static_cast<DWORD>(\n                std::clamp(static_cast<float>(Settings::WheelFFBGlobalStrength), 0.0f, 1.0f) *\n                static_cast<float>(DI_FFNOMINALMAX));'''
gain_count = ffb.count(old_gain)
if gain_count < 3:
    raise SystemExit(f'full DirectInput gain: expected at least 3 effect create paths, found {gain_count}')
ffb = ffb.replace(old_gain, 'effect.dwGain = DI_FFNOMINALMAX;')
print(f'community patch: DirectInput device gain fixed at nominal on {gain_count} create paths')

ffb = replace_between(
    ffb,
    '        DWORD configured_effect_gain() const\n',
    '        void apply_live_effect_gain()\n',
'''        DWORD configured_effect_gain() const\n        {\n            // Overall strength moved into the force model. The DirectInput\n            // device-side gain remains nominal, avoiding double scaling.\n            return DI_FFNOMINALMAX;\n        }\n\n''',
    'keep DirectInput dwGain nominal')


# ---------------------------------------------------------------------------
# 3. Broad wheel-base compatibility from the controlled community testing:
#    actual FFB actuator axes, two-axis POLAR -> one-axis CARTESIAN fallback,
#    and temporary rejection of a dead interface so a sibling endpoint wins.
# ---------------------------------------------------------------------------
if '#include <vector>' not in ffb:
    ffb = rep(ffb, '#include <string>\n', '#include <string>\n#include <vector>\n', 'vector for actuator axes')

ffb = rep(
    ffb,
'''            const std::string wantedGuid =\n                lower_copy(Settings::WheelFFBDeviceGuid.get().c_str());\n''',
'''            const std::string wantedGuid =\n                lower_copy(Settings::WheelFFBDeviceGuid.get().c_str());\n\n            if (ctx->self && GetTickCount() < ctx->self->failedInterfaceUntil_ &&\n                directinput_guid_key(instance->guidInstance) == ctx->self->failedInterfaceGuid_)\n            {\n                spdlog::warn(\n                    "WheelFFB: temporarily skipping rejected FFB interface '{}' [{}] to try a sibling",\n                    instance->tszProductName, directinput_guid_key(instance->guidInstance));\n                return DIENUM_CONTINUE;\n            }\n''',
    'skip recently rejected sibling interface')

if "saved DirectInput GUID '{}' not attached; falling back to DeviceName" in ffb:
    ffb = ffb.replace(
        "saved DirectInput GUID '{}' not attached; falling back to DeviceName",
        "saved DirectInput GUID '{}' unavailable/rejected; falling back to DeviceName",
        1)

helpers = r'''        static BOOL CALLBACK enum_actuator_axis_callback(
            LPCDIDEVICEOBJECTINSTANCEA object, LPVOID context)
        {
            auto* self = static_cast<WheelFFBEngine*>(context);
            if (!self || !object)
                return DIENUM_CONTINUE;
            if ((object->dwType & DIDFT_FFACTUATOR) != 0 && self->actuatorAxes_.size() < 2)
                self->actuatorAxes_.push_back(object->dwOfs);
            return DIENUM_CONTINUE;
        }

        DWORD primary_actuator_axis() const
        {
            return actuatorAxes_.empty() ? DIJOFS_X : actuatorAxes_.front();
        }

        void mark_selected_interface_failed(const char* reason, HRESULT hr)
        {
            failedInterfaceGuid_ = directinput_guid_key(selectedGuid_);
            failedInterfaceUntil_ = GetTickCount() + 5000;
            spdlog::warn(
                "WheelFFB: interface '{}' [{}] rejected {}; trying a sibling interface on retry (0x{:08X})",
                selectedName_, failedInterfaceGuid_, reason, (unsigned)hr);
        }

'''
ffb = rep(
    ffb,
    '        bool initialize()\n        {\n',
    helpers + '        bool initialize()\n        {\n',
    'actuator/sibling helper functions')

# Debug pass 1 found this generic marker occurs in both initialisation and the
# runtime recreate path. Insert before the FIRST one only; that one is the
# initial creation immediately after SETACTUATORSON.
initial_create = '''            if (!create_constant_effect())\n            {\n'''
axis_setup = '''            actuatorAxes_.clear();\n            const HRESULT axisEnumHr = device_->EnumObjects(\n                enum_actuator_axis_callback, this, DIDFT_AXIS);\n            if (FAILED(axisEnumHr) || actuatorAxes_.empty())\n            {\n                actuatorAxes_.clear();\n                actuatorAxes_.push_back(DIJOFS_X);\n                spdlog::warn(\n                    "WheelFFB: no explicit DIDFT_FFACTUATOR axis reported; using DIJOFS_X fallback");\n            }\n            else\n            {\n                spdlog::info(\n                    "WheelFFB: detected {} force actuator axis/axes; primary offset={}",\n                    actuatorAxes_.size(), (unsigned)actuatorAxes_.front());\n            }\n\n'''
ffb = first_insert(ffb, initial_create, axis_setup, 'enumerate actuator axes before initial ConstantForce')

constant_create = r'''        bool create_constant_effect()
        {
            if (!device_)
                return false;

            safe_release_effect(constantEffect_, "constant before create");
            constantEffectPolar_ = false;

            DWORD axes[2] = {
                primary_actuator_axis(),
                actuatorAxes_.size() > 1 ? actuatorAxes_[1] : primary_actuator_axis()
            };
            LONG directions[2] = { 9000L, 0L };
            DICONSTANTFORCE cf{};
            cf.lMagnitude = 0;

            DIEFFECT effect{};
            effect.dwSize = sizeof(effect);
            effect.dwDuration = INFINITE;
            effect.dwSamplePeriod = 0;
            effect.dwGain = DI_FFNOMINALMAX;
            effect.dwTriggerButton = DIEB_NOTRIGGER;
            effect.dwTriggerRepeatInterval = 0;
            effect.rgdwAxes = axes;
            effect.rglDirection = directions;
            effect.cbTypeSpecificParams = sizeof(cf);
            effect.lpvTypeSpecificParams = &cf;

            HRESULT hr = E_FAIL;
            if (actuatorAxes_.size() > 1)
            {
                effect.dwFlags = DIEFF_POLAR | DIEFF_OBJECTOFFSETS;
                effect.cAxes = 2;
                hr = device_->CreateEffect(GUID_ConstantForce, &effect, &constantEffect_, nullptr);
                if (SUCCEEDED(hr) && constantEffect_)
                {
                    constantEffectPolar_ = true;
                    spdlog::info("WheelFFB: ConstantForce created with 2-axis POLAR actuator encoding");
                    return true;
                }

                safe_release_effect(constantEffect_, "failed POLAR constant");
                spdlog::warn(
                    "WheelFFB: 2-axis POLAR ConstantForce rejected (0x{:08X}); trying one-axis CARTESIAN",
                    (unsigned)hr);
            }

            effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
            effect.cAxes = 1;
            directions[0] = 1;
            hr = device_->CreateEffect(GUID_ConstantForce, &effect, &constantEffect_, nullptr);
            if (FAILED(hr) || !constantEffect_)
            {
                mark_selected_interface_failed("ConstantForce creation", hr);
                spdlog::error("WheelFFB: CreateEffect(ConstantForce) failed (0x{:08X})", (unsigned)hr);
                return false;
            }

            constantEffectPolar_ = false;
            spdlog::info("WheelFFB: ConstantForce created with 1-axis CARTESIAN fallback");
            return true;
        }

'''
ffb = replace_between(
    ffb,
    '        bool create_constant_effect()\n',
    '        bool create_spring_effect()\n',
    constant_create,
    'POLAR/CARTESIAN ConstantForce create fallback')

fixed_axis = 'DWORD axes[1] = { DIJOFS_X };'
fixed_count = ffb.count(fixed_axis)
if fixed_count < 3:
    raise SystemExit(f'actuator-axis replacement expected >=3 spring/damper/periodic paths, found {fixed_count}')
ffb = ffb.replace(fixed_axis, 'DWORD axes[1] = { primary_actuator_axis() };')
print(f'community patch: selected actuator axis used by {fixed_count} condition/periodic create paths')

constant_set = r'''        void set_constant_force(LONG requestedLevel)
        {
            if (!device_ || panicStopped_)
                return;

            requestedLevel = std::clamp(
                requestedLevel,
                -static_cast<LONG>(DI_FFNOMINALMAX),
                static_cast<LONG>(DI_FFNOMINALMAX));

            DICONSTANTFORCE cf{};
            LONG directions[2] = { 1L, 0L };
            DWORD flags = DIEP_TYPESPECIFICPARAMS | DIEP_START;

            DIEFFECT params{};
            params.dwSize = sizeof(params);
            params.cbTypeSpecificParams = sizeof(cf);
            params.lpvTypeSpecificParams = &cf;

            if (constantEffectPolar_)
            {
                cf.lMagnitude = std::abs(requestedLevel);
                directions[0] = requestedLevel < 0 ? 27000L : 9000L;
                params.cAxes = 2;
                params.rglDirection = directions;
                flags |= DIEP_DIRECTION;
            }
            else
            {
                cf.lMagnitude = requestedLevel;
            }

            HRESULT hr = E_FAIL;
            if (constantEffect_)
                hr = constantEffect_->SetParameters(&params, flags);

            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
            {
                if (reacquire_after_input_loss("ConstantForce", hr) && constantEffect_)
                    hr = constantEffect_->SetParameters(&params, flags);
            }

            if (hr == E_HANDLE || hr == DIERR_NOTDOWNLOADED || !constantEffect_)
            {
                safe_release_effect(constantEffect_, "stale constant");
                const DWORD now = GetTickCount();
                if (now < recreateHoldoffUntil_)
                    return;

                if (!create_constant_effect())
                {
                    recreateHoldoffUntil_ = now + 500;
                    request_device_reinitialize("ConstantForce recreation rejected interface", hr);
                    return;
                }

                recreateRampFrames_ = RecreateRampFrames;
                prevConstantLevel_ = 0;
                prevStructuralLevel_ = 0;
                spdlog::info("WheelFFB: recreated ConstantForce after handle loss; ramping in");
                return;
            }

            if (FAILED(hr))
            {
                note_device_failure("ConstantForce", hr);
                spdlog::warn("WheelFFB: constant force update failed (0x{:08X})", (unsigned)hr);
                return;
            }

            clear_device_failure();
            prevConstantLevel_ = requestedLevel;
        }

'''
ffb = replace_between(
    ffb,
    '        void set_constant_force(LONG requestedLevel)\n',
    '        void update_crash_detection(float speed, uint32_t stateFlags)\n',
    constant_set,
    'POLAR-aware live ConstantForce updates')

ffb = rep(
    ffb,
'''        IDirectInputEffect* tireSlipEffect_ = nullptr;\n\n        GUID selectedGuid_{};\n''',
'''        IDirectInputEffect* tireSlipEffect_ = nullptr;\n        std::vector<DWORD> actuatorAxes_;\n        bool constantEffectPolar_ = false;\n\n        GUID selectedGuid_{};\n''',
    'actuator and ConstantForce encoding state')

ffb = rep(
    ffb,
'        DWORD retryAfter_ = 0;\n',
'''        DWORD retryAfter_ = 0;\n        std::string failedInterfaceGuid_;\n        DWORD failedInterfaceUntil_ = 0;\n''',
    'sibling-interface recovery state')

# Pin whichever interface actually succeeded, avoiding repeated attempts to jump
# back to a duplicate endpoint that cannot create force effects.
create_fail_block = '''            if (!create_constant_effect())\n            {\n                release_device();\n                release_directinput();\n                return false;\n            }\n'''
pin_working = create_fail_block + '''\n            {\n                const std::string actualGuid = directinput_guid_key(selectedGuid_);\n                if (lower_copy(Settings::WheelFFBDeviceGuid.get().c_str()) != actualGuid)\n                {\n                    Settings::WheelFFBDeviceGuid = actualGuid;\n                    selectedConfiguredGuid_ = actualGuid;\n                    spdlog::info(\n                        "WheelFFB: pinned working force interface GUID '{}' after capability validation",\n                        actualGuid);\n                }\n            }\n'''
# At this stage only initialisation still contains the full release-on-failure block.
ffb = rep(ffb, create_fail_block, pin_working, 'pin validated working sibling GUID')


# ---------------------------------------------------------------------------
# 4. Safe fixed-output direction test. It is hard capped at 20%, independent of
#    100-150% model gain, and temporarily silences Spring/Damper.
# ---------------------------------------------------------------------------
ffb = rep(
    ffb,
'        void check_watchdog()\n        {\n',
'''        void request_direction_test(int direction)\n        {\n            if (direction == 0)\n            {\n                manualTestFrames_ = 0;\n                if (initialized_ && !panicStopped_)\n                    set_constant_force(0);\n                return;\n            }\n            manualTestDirection_ = direction < 0 ? -1 : 1;\n            manualTestFrames_ = 18;\n            spdlog::info(\n                "WheelFFB: queued safe {} direction test at fixed 20% output",\n                manualTestDirection_ < 0 ? "left" : "right");\n        }\n\n        void check_watchdog()\n        {\n''',
    'safe direction-test request')

ffb = rep(
    ffb,
'''            apply_live_effect_gain();\n\n            float steer = read_game_steering();\n''',
'''            apply_live_effect_gain();\n\n            if (manualTestFrames_ > 0)\n            {\n                if (springEffect_)\n                    update_spring(0.0f);\n                if (damperEffect_)\n                    update_damper(0.0f);\n\n                LONG testLevel = manualTestDirection_ * 2000L;\n                if (Settings::WheelFFBInvertForce)\n                    testLevel = -testLevel;\n                set_constant_force(testLevel);\n                --manualTestFrames_;\n                if (manualTestFrames_ == 0)\n                {\n                    set_constant_force(0);\n                    warmupFrames_ = 0;\n                }\n                return;\n            }\n\n            float steer = read_game_steering();\n''',
    'execute fixed 20-percent direction test')

ffb = rep(
    ffb,
'        int recreateRampFrames_ = 0;\n',
'''        int recreateRampFrames_ = 0;\n        int manualTestFrames_ = 0;\n        int manualTestDirection_ = 1;\n''',
    'direction-test state')

if 'void WheelFFB_RequestDirectionTest(int direction)' not in ffb:
    ffb += r'''

void WheelFFB_RequestDirectionTest(int direction)
{
    gWheelFFB.request_direction_test(direction);
}
'''
    print('community patch: exported direction-test API')


# ---------------------------------------------------------------------------
# 5. F11 UI: versioned reference presets and matching 0-150% range.
# ---------------------------------------------------------------------------
uis = rep(
    uis,
'''    extern Setting<float> WheelFFBSteeringWeight;\n    extern Setting<float> WheelFFBGripLoss;\n''',
'''    extern Setting<float> WheelFFBSteeringWeight;\n    extern Setting<float> WheelFFBGripLoss;\n    extern Setting<float> WheelFFBLowSpeedSpring;\n    extern Setting<float> WheelFFBSpringLoadBoost;\n''',
    'declare preset-only simulation settings')

if 'void WheelFFB_RequestDirectionTest(int direction);' not in uis:
    uis = rep(
        uis,
        '    extern Setting<bool> WheelFFBDebugLog;\n}\n',
        '    extern Setting<bool> WheelFFBDebugLog;\n}\n\nvoid WheelFFB_RequestDirectionTest(int direction);\n',
        'declare safe direction-test API')

ui_anchor = '''\t\tImGui::TextWrapped("Advanced DirectInput COM FFB. Input bindings can come from any connected device; FFB remains attached to the selected force-feedback wheel.");\n\t\tImGui::Spacing();\n\n\t\tif (ImGui::Checkbox("Enable force feedback", Settings::WheelFFBEnable.ptr()))\n'''
ui_new = '''\t\tImGui::TextWrapped("Advanced DirectInput COM FFB. Input bindings can come from any connected device; FFB remains attached to the selected force-feedback wheel.");\n\t\tImGui::Spacing();\n\n\t\tauto applyPreset = [&](float overall, float spring, float damper, float lateral,\n\t\t\tfloat grip, float impact, float road, float tire, float lowSpeed, float loadBoost)\n\t\t{\n\t\t\tSettings::WheelFFBGlobalStrength = overall;\n\t\t\tSettings::WheelFFBSpringStrength = spring;\n\t\t\tSettings::WheelFFBDamperStrength = damper;\n\t\t\tSettings::WheelFFBSteeringWeight = lateral;\n\t\t\tSettings::WheelFFBGripLoss = grip;\n\t\t\tSettings::WheelFFBWallImpact = impact;\n\t\t\tSettings::WheelFFBRoadTexture = road;\n\t\t\tSettings::WheelFFBTireSlip = tire;\n\t\t\tSettings::WheelFFBLowSpeedSpring = lowSpeed;\n\t\t\tSettings::WheelFFBSpringLoadBoost = loadBoost;\n\t\t\tsetting_changed(Settings::WheelFFBGlobalStrength);\n\t\t\tsetting_changed(Settings::WheelFFBSpringStrength);\n\t\t\tsetting_changed(Settings::WheelFFBDamperStrength);\n\t\t\tsetting_changed(Settings::WheelFFBSteeringWeight);\n\t\t\tsetting_changed(Settings::WheelFFBGripLoss);\n\t\t\tsetting_changed(Settings::WheelFFBWallImpact);\n\t\t\tsetting_changed(Settings::WheelFFBRoadTexture);\n\t\t\tsetting_changed(Settings::WheelFFBTireSlip);\n\t\t\tsetting_changed(Settings::WheelFFBLowSpeedSpring);\n\t\t\tsetting_changed(Settings::WheelFFBSpringLoadBoost);\n\t\t};\n\n\t\tImGui::SeparatorText("Versioned baseline presets");\n\t\tif (ImGui::Button("Simulation Balanced v1"))\n\t\t\tapplyPreset(0.70f, 0.60f, 0.32f, 0.38f, 0.65f, 0.65f, 0.30f, 0.20f, 0.08f, 0.35f);\n\t\tImGui::SameLine();\n\t\tif (ImGui::Button("Arcade Light v1"))\n\t\t\tapplyPreset(0.55f, 0.45f, 0.15f, 0.30f, 0.55f, 0.55f, 0.30f, 0.18f, 0.12f, 0.20f);\n\t\tImGui::SameLine();\n\t\tif (ImGui::Button("Arcade Strong v1"))\n\t\t\tapplyPreset(0.90f, 0.65f, 0.30f, 0.45f, 0.60f, 0.75f, 0.35f, 0.22f, 0.08f, 0.40f);\n\t\tImGui::TextDisabled("Preset names are immutable test references; adjust sliders afterward for a custom tune.");\n\n\t\tif (ImGui::Checkbox("Enable force feedback", Settings::WheelFFBEnable.ptr()))\n'''
uis = rep(uis, ui_anchor, ui_new, 'versioned reference presets')

uis = rep(
    uis,
'''\t\tif (ImGui::SliderFloat("Overall strength", Settings::WheelFFBGlobalStrength.ptr(), 0.0f, 1.0f, "%.2f"))\n\t\t\tsetting_changed(Settings::WheelFFBGlobalStrength);\n''',
'''\t\tif (ImGui::SliderFloat("Overall strength", Settings::WheelFFBGlobalStrength.ptr(), 0.0f, 1.5f, "%.2f"))\n\t\t\tsetting_changed(Settings::WheelFFBGlobalStrength);\n\t\tif (Settings::WheelFFBGlobalStrength.get() > 1.0f)\n\t\t\tImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f),\n\t\t\t\t"Above 100% reduces headroom/contrast. Use only when the wheel is still too light.");\n''',
    '0-150 percent strength slider and warning')

uis = rep(
    uis,
'''\t\tif (ImGui::Checkbox("Diagnostic logging", Settings::WheelFFBDebugLog.ptr()))\n\t\t\tsetting_changed(Settings::WheelFFBDebugLog);\n\n\t\tImGui::Spacing();\n''',
'''\t\tif (ImGui::Checkbox("Diagnostic logging", Settings::WheelFFBDebugLog.ptr()))\n\t\t\tsetting_changed(Settings::WheelFFBDebugLog);\n\n\t\tImGui::SeparatorText("Safe direction test");\n\t\tif (ImGui::Button("Test Left (20%)"))\n\t\t\tWheelFFB_RequestDirectionTest(-1);\n\t\tImGui::SameLine();\n\t\tif (ImGui::Button("Test Right (20%)"))\n\t\t\tWheelFFB_RequestDirectionTest(1);\n\t\tImGui::SameLine();\n\t\tif (ImGui::Button("Stop Test"))\n\t\t\tWheelFFB_RequestDirectionTest(0);\n\t\tImGui::TextDisabled("Direction tests are hard-capped at 20% and ignore Overall Strength headroom.");\n\n\t\tImGui::Spacing();\n''',
    'safe direction-test UI')

# Keep the duplicate Wheel Setup tuning panel consistent with the advanced page.
setup = rep(
    setup,
'            ImGui::SliderFloat("Overall Strength", Settings::WheelFFBGlobalStrength.ptr(), 0.0f, 1.0f, "%.2f");\n',
'''            ImGui::SliderFloat("Overall Strength", Settings::WheelFFBGlobalStrength.ptr(), 0.0f, 1.5f, "%.2f");\n            if (Settings::WheelFFBGlobalStrength.get() > 1.0f)\n                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f),\n                    "Above 100% trades force-detail contrast for extra weight.");\n''',
    'Wheel Setup 0-150 percent range')

old_baseline = '''            if (ImGui::Button("Load simulation baseline"))\n            {\n                Settings::WheelFFBGlobalStrength = 0.50f;\n                Settings::WheelFFBSpringStrength = 0.45f;\n                Settings::WheelFFBLowSpeedSpring = 0.08f;\n                Settings::WheelFFBSpringLoadBoost = 0.35f;\n                Settings::WheelFFBDamperStrength = 0.35f;\n                Settings::WheelFFBSteeringWeight = 0.40f;\n                Settings::WheelFFBGripLoss = 0.65f;\n                Settings::WheelFFBRoadTexture = 0.12f;\n                Settings::WheelFFBTireSlip = 0.15f;\n                Settings::WheelFFBWallImpact = 0.50f;\n                Settings::WheelFFBUseHardwareSpring = true;\n                Settings::WheelFFBUseHardwareDamper = true;\n                status_ = "Loaded simulation baseline. Save the wheel profile after testing.";\n            }\n'''
new_baseline = '''            if (ImGui::Button("Load Simulation Balanced v1"))\n            {\n                Settings::WheelFFBGlobalStrength = 0.70f;\n                Settings::WheelFFBSpringStrength = 0.60f;\n                Settings::WheelFFBLowSpeedSpring = 0.08f;\n                Settings::WheelFFBSpringLoadBoost = 0.35f;\n                Settings::WheelFFBDamperStrength = 0.32f;\n                Settings::WheelFFBSteeringWeight = 0.38f;\n                Settings::WheelFFBGripLoss = 0.65f;\n                Settings::WheelFFBRoadTexture = 0.30f;\n                Settings::WheelFFBTireSlip = 0.20f;\n                Settings::WheelFFBWallImpact = 0.65f;\n                Settings::WheelFFBUseHardwareSpring = true;\n                Settings::WheelFFBUseHardwareDamper = true;\n                status_ = "Loaded Simulation Balanced v1. Test first, then save your preferred profile.";\n            }\n'''
setup = rep(setup, old_baseline, new_baseline, 'Wheel Setup baseline matches Simulation Balanced v1')


# Write and self-audit the effective source generated by all prior build patches.
ffb_path.write_text(ffb, encoding='utf-8')
input_path.write_text(ims, encoding='utf-8')
ui_path.write_text(uis, encoding='utf-8')
setup_path.write_text(setup, encoding='utf-8')
ini_path.write_text(inis, encoding='utf-8')

checks = {
    'src/input_manager.cpp': [
        'Setting<bool> WheelInputCompatibility',
        'has_attached_ffb_wheel()',
        'SDL_GetJoysticks',
    ],
    'src/hooks_wheel_ffb.cpp': [
        'Range<float>{ 0.0f, 1.5f }',
        'd-b-c-e toolkit v0.8.0 ordering',
        'DIDFT_FFACTUATOR',
        '2-axis POLAR actuator encoding',
        'one-axis CARTESIAN',
        'temporarily skipping rejected FFB interface',
        'pinned working force interface GUID',
        'queued safe {} direction test at fixed 20% output',
        'effect.dwGain = DI_FFNOMINALMAX;',
    ],
    'src/overlay/input_bindings_ui.cpp': [
        'Simulation Balanced v1', 'Arcade Light v1', 'Arcade Strong v1',
        'Test Left (20%)', 'Above 100% reduces headroom/contrast',
    ],
    'src/overlay/wheel_setup_ui.cpp': [
        'Load Simulation Balanced v1',
        '0.0f, 1.5f, "%.2f"',
    ],
    'OutRun2006Tweaks.ini': [
        'WheelInputCompatibility = false',
        'UseNewInput = true',
    ],
}
for path, needles in checks.items():
    data = Path(path).read_text(encoding='utf-8')
    for needle in needles:
        if needle not in data:
            raise SystemExit(f'community verify failed: {needle!r} missing from {path}')
        print(f'community verify OK: {path}: {needle}')

# Signal-order audit: the new master multiplication must be before tanh and slew.
signal = Path('src/hooks_wheel_ffb.cpp').read_text(encoding='utf-8')
pos_gain = signal.find('float total = (structural + events) * outputStrength;')
pos_invert = signal.find('if (Settings::WheelFFBInvertForce)', pos_gain)
pos_tanh = signal.find('std::tanh(total)', pos_gain)
pos_slew = signal.find('const LONG structuralDelta', pos_tanh)
if not (0 <= pos_gain < pos_invert < pos_tanh < pos_slew):
    raise SystemExit('community verify failed: gain/invert/tanh/slew ordering is wrong')
print('community verify OK: gain -> invert -> tanh -> slew ordering')

print('Applied d-b-c-e v0.8.0 + hyp36rmax/Discord refinements; debug-pass-1 ambiguities removed')
