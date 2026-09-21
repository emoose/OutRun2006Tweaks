from pathlib import Path

# Hybrid integration layer.
# Input/binding architecture is adapted from hyp36rmax/multi-device-input (MIT),
# while this branch keeps its richer DirectInput COM multi-effect FFB engine.

im = Path('src/input_manager.cpp')
ui = Path('src/overlay/input_bindings_ui.cpp')
dll = Path('src/dllmain.cpp')

ims = im.read_text(encoding='utf-8')
uis = ui.read_text(encoding='utf-8')
dlls = dll.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected exactly one match, got {count}')
    print(f'patched: {label}')
    return text.replace(old, new, 1)


# --- Input manager: keep hyp36rmax raw SDL multi-device input, but do not start
# its separate ConstantForce FFB backend.  A tiny DirectInput probe is enough to
# choose SDL's DirectInput input backend automatically when an FFB wheel exists.
ims = rep(
    ims,
    '#include "wheel_force_feedback.hpp"\n',
    '''#ifndef DIRECTINPUT_VERSION\n#define DIRECTINPUT_VERSION 0x0800\n#endif\n#include <Windows.h>\n#include <dinput.h>\n\n#pragma comment(lib, "dinput8.lib")\n#pragma comment(lib, "dxguid.lib")\n''',
    'remove competing FFB backend dependency')

anchor = 'InputManager& InputManager::instance = *new InputManager;\n'
probe = r'''namespace
{
    BOOL CALLBACK detect_ffb_device(LPCDIDEVICEINSTANCEA, LPVOID context)
    {
        *static_cast<bool*>(context) = true;
        return DIENUM_STOP;
    }

    bool has_attached_ffb_wheel()
    {
        IDirectInput8A* di = nullptr;
        const HRESULT createHr = DirectInput8Create(
            GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
            reinterpret_cast<void**>(&di), nullptr);
        if (FAILED(createHr) || !di)
            return false;

        bool found = false;
        const HRESULT enumHr = di->EnumDevices(
            DI8DEVCLASS_GAMECTRL, detect_ffb_device, &found,
            DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
        di->Release();
        return SUCCEEDED(enumHr) && found;
    }
}

'''
ims = rep(ims, anchor, probe + anchor, 'DirectInput wheel auto-backend probe')
ims = rep(
    ims,
    'if (activeBackend == 0 && WheelForceFeedback::has_attached_device())',
    'if (activeBackend == 0 && has_attached_ffb_wheel())',
    'automatic DirectInput selection')
ims = rep(ims, '\n\tWheelForceFeedback::init(hwnd);\n', '\n', 'leave FFB ownership to custom engine')
ims = rep(
    ims,
    '''void InputManager_Update()\n{\n\tWheelForceFeedback::update();\n\tif (Settings::UseNewInput)\n\t\tInputManager::instance.update();\n}\n''',
    '''void InputManager_Update()\n{\n\tif (Settings::UseNewInput)\n\t\tInputManager::instance.update();\n}\n''',
    'remove competing per-frame FFB update')

# --- Binding UI: retain Quick Setup, calibration, hot-plug device list and raw
# device bindings. Replace hyp36rmax's simple FFB page with this branch's richer
# simulation controls; device identity/capability controls stay in Wheel Setup.
uis = rep(
    uis,
    '#include "wheel_force_feedback.hpp"\n',
    '',
    'binding UI uses custom FFB engine')

insert_after = '#include <cstring>\n'
settings_decl = r'''
namespace Settings
{
    extern Setting<bool> WheelFFBEnable;
    extern Setting<float> WheelFFBGlobalStrength;
    extern Setting<float> WheelFFBSpringStrength;
    extern Setting<float> WheelFFBDamperStrength;
    extern Setting<float> WheelFFBSteeringWeight;
    extern Setting<float> WheelFFBGripLoss;
    extern Setting<float> WheelFFBWallImpact;
    extern Setting<float> WheelFFBRoadTexture;
    extern Setting<float> WheelFFBTireSlip;
    extern Setting<bool> WheelFFBUseHardwareSpring;
    extern Setting<bool> WheelFFBUseHardwareDamper;
    extern Setting<bool> WheelFFBUsePeriodicEffects;
    extern Setting<bool> WheelFFBInvertForce;
    extern Setting<bool> WheelFFBInvertSpring;
    extern Setting<bool> WheelFFBDebugLog;
}
'''
uis = rep(uis, insert_after, insert_after + settings_decl, 'declare advanced FFB settings in binding UI')

ffb_start = uis.find('\tvoid draw_force_feedback()\n\t{')
ffb_end_marker = '\n\t// The prompt shown while an input is being waited on.'
ffb_end = uis.find(ffb_end_marker, ffb_start)
if ffb_start < 0 or ffb_end < 0:
    raise SystemExit('advanced FFB UI replacement markers not found')

new_ffb = r'''	void draw_force_feedback()
	{
		ImGui::TextWrapped("Advanced DirectInput COM FFB. Input bindings can come from any connected device; FFB remains attached to the selected force-feedback wheel.");
		ImGui::Spacing();

		if (ImGui::Checkbox("Enable force feedback", Settings::WheelFFBEnable.ptr()))
			setting_changed(Settings::WheelFFBEnable);

		ImGui::BeginDisabled(!Settings::WheelFFBEnable);
		if (ImGui::SliderFloat("Overall strength", Settings::WheelFFBGlobalStrength.ptr(), 0.0f, 1.0f, "%.2f"))
			setting_changed(Settings::WheelFFBGlobalStrength);
		if (ImGui::SliderFloat("Aligning / centering", Settings::WheelFFBSpringStrength.ptr(), 0.0f, 1.5f, "%.2f"))
			setting_changed(Settings::WheelFFBSpringStrength);
		if (ImGui::SliderFloat("Dynamic damping", Settings::WheelFFBDamperStrength.ptr(), 0.0f, 1.0f, "%.2f"))
			setting_changed(Settings::WheelFFBDamperStrength);
		if (ImGui::SliderFloat("Cornering load", Settings::WheelFFBSteeringWeight.ptr(), 0.0f, 1.5f, "%.2f"))
			setting_changed(Settings::WheelFFBSteeringWeight);
		if (ImGui::SliderFloat("Grip-loss unloading", Settings::WheelFFBGripLoss.ptr(), 0.0f, 1.0f, "%.2f"))
			setting_changed(Settings::WheelFFBGripLoss);
		if (ImGui::SliderFloat("Collision", Settings::WheelFFBWallImpact.ptr(), 0.0f, 1.0f, "%.2f"))
			setting_changed(Settings::WheelFFBWallImpact);
		if (ImGui::SliderFloat("Road detail", Settings::WheelFFBRoadTexture.ptr(), 0.0f, 1.0f, "%.2f"))
			setting_changed(Settings::WheelFFBRoadTexture);
		if (ImGui::SliderFloat("Tire slip", Settings::WheelFFBTireSlip.ptr(), 0.0f, 1.0f, "%.2f"))
			setting_changed(Settings::WheelFFBTireSlip);

		ImGui::SeparatorText("Backends / direction");
		if (ImGui::Checkbox("Hardware GUID_Spring", Settings::WheelFFBUseHardwareSpring.ptr()))
			setting_changed(Settings::WheelFFBUseHardwareSpring);
		ImGui::SameLine();
		if (ImGui::Checkbox("Hardware GUID_Damper", Settings::WheelFFBUseHardwareDamper.ptr()))
			setting_changed(Settings::WheelFFBUseHardwareDamper);
		if (ImGui::Checkbox("Hardware road/slip sine effects", Settings::WheelFFBUsePeriodicEffects.ptr()))
			setting_changed(Settings::WheelFFBUsePeriodicEffects);
		if (ImGui::Checkbox("Reverse ConstantForce", Settings::WheelFFBInvertForce.ptr()))
			setting_changed(Settings::WheelFFBInvertForce);
		ImGui::SameLine();
		if (ImGui::Checkbox("Reverse Spring", Settings::WheelFFBInvertSpring.ptr()))
			setting_changed(Settings::WheelFFBInvertSpring);
		if (ImGui::Checkbox("Diagnostic logging", Settings::WheelFFBDebugLog.ptr()))
			setting_changed(Settings::WheelFFBDebugLog);

		ImGui::Spacing();
		ImGui::TextDisabled("Select/refresh the exact DirectInput FFB interface and inspect capabilities in the Wheel Setup tab.");
		ImGui::EndDisabled();
	}
'''
uis = uis[:ffb_start] + new_ffb + uis[ffb_end:]
print('patched: replace simple ConstantForce FFB page with advanced controls')

uis = rep(
    uis,
    'ImGui::SliderInt("Steering Deadzone", &deadzonePercent, 5, 20, "%d%%")',
    'ImGui::SliderInt("Steering Deadzone", &deadzonePercent, 0, 20, "%d%%")',
    'allow zero steering deadzone')

old_attribution = 'ImGui::TextDisabled("Multi-device input by hyp36rmax");'
new_attribution = 'ImGui::TextDisabled("Multi-device input architecture adapted from hyp36rmax (MIT)");'
attribution_count = uis.count(old_attribution)
if attribution_count < 1:
    raise SystemExit('runtime attribution: no hyp36rmax attribution labels found')
uis = uis.replace(old_attribution, new_attribution)
print(f'patched: runtime attribution x{attribution_count}')

# --- Startup policy: no longer force the legacy Sumo DirectInput path.  The new
# raw-device InputManager is now the preferred path; legacy helpers remain a
# deliberate fallback whenever UseNewInput=false.
legacy_start = dlls.find('\t// The last official release used the game\'s own DirectInput path.')
legacy_end_marker = '\n\tSettings::to_log();'
legacy_end = dlls.find(legacy_end_marker, legacy_start)
if legacy_start < 0 or legacy_end < 0:
    raise SystemExit('legacy startup override block markers not found')
replacement = '''\tif (Settings::UseNewInput)\n\t{\n\t\tspdlog::info("Wheel input: multi-device SDL raw input enabled; legacy DirectInput helpers remain inactive fallback");\n\t}\n\telse if (Settings::WheelInputCompatibility)\n\t{\n\t\tspdlog::info("Wheel input: legacy DirectInput compatibility fallback enabled");\n\t}\n'''
dlls = dlls[:legacy_start] + replacement + dlls[legacy_end:]
print('patched: prefer multi-device input without deleting legacy fallback')

im.write_text(ims, encoding='utf-8')
ui.write_text(uis, encoding='utf-8')
dll.write_text(dlls, encoding='utf-8')
print('Applied hyp36rmax multi-device / custom DirectInput COM FFB hybrid')
