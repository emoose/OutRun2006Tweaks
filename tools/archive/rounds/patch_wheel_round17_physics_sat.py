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
        raise SystemExit(f'round17 {label}: expected exactly one match, got {count}')
    print(f'ROUND17 patched: {label}')
    return text.replace(old, new, 1)


ffb = rep(ffb,
'''#include "game_addrs.hpp"\n''',
'''#include "game_addrs.hpp"\n#include "hooks_wheel_physics_sat.hpp"\n''',
'include physics SAT helper')

ffb = rep(ffb,
'''    Setting<float> WheelFFBSteeringWeight{\n        "WheelFFB", "SteeringWeight", 1.45f,\n        "Self-aligning torque strength. Uses steering angle, speed and OutRun lateral load; unloads only in deeper drift.", Range<float>{ 0.0f, 2.0f }\n    };\n\n''',
'''    Setting<float> WheelFFBSteeringWeight{\n        "WheelFFB", "SteeringWeight", 1.45f,\n        "Self-aligning torque strength. Physics SAT uses body slip, yaw rate and lateral load; Natural SAT remains available for comparison.", Range<float>{ 0.0f, 2.0f }\n    };\n\n    Setting<bool> WheelFFBPhysicsSat{\n        "WheelFFB", "PhysicsSAT", true,\n        "Experimental body-slip/yaw SAT instead of steering-centre direction alone."\n    };\n\n''',
'physics SAT setting')

ffb = rep(ffb,
'''            const float selfAligningTorque =\n                (steer >= 0.0f ? -1.0f : 1.0f) *\n                steerForSat * satSpeed * satLoadBoost * satGrip *\n                satReturnRelief * satStrength;\n''',
'''            const float naturalSatTorque =\n                (steer >= 0.0f ? -1.0f : 1.0f) *\n                steerForSat * satSpeed * satLoadBoost * satGrip *\n                satReturnRelief * satStrength;\n\n            // Physics SAT derives force direction from estimated front slip,\n            // not merely from steering sign. During initial basis calibration\n            // retain only a small Natural SAT safety net, then crossfade over\n            // 24 valid physics ticks. Once active, a valid zero Physics SAT is\n            // truly zero; Natural SAT never leaks back in around wheel centre.\n            const float physicsSatTorque = physicsSat_.update(\n                car, steer, speedNorm, cornerLoadSmooth, returnRateSmooth,\n                static_cast<float>(Settings::WheelFFBGripLoss),\n                satStrength, satSpeed);\n            const float physicsFallback = naturalSatTorque * 0.15f;\n            const float physicsMix = physicsSat_.activationBlend();\n            const float selfAligningTorque = Settings::WheelFFBPhysicsSat\n                ? physicsFallback + (physicsSatTorque - physicsFallback) * physicsMix\n                : naturalSatTorque;\n''',
'front-slip physics SAT selection')

ffb = rep(ffb,
'''            smoothedLateral_ = 0.0f;\n            prevSteer_ = 0.0f;\n            prevStructuralLevel_ = 0;\n''',
'''            smoothedLateral_ = 0.0f;\n            prevSteer_ = 0.0f;\n            physicsSat_.reset();\n            prevStructuralLevel_ = 0;\n''',
'reset physics SAT state')

ffb = rep(ffb,
'''                "WheelFFB DIAG: spd={:.2f} steer={:.3f} rate={:.4f} lat={:.2f} drift={:.2f} rough={:.2f} sat={:.3f} steerSrc={} out={} invCF={} spring={} invSpring={} coeff={} damper={} dcoeff={} periodic={}",\n''',
'''                "WheelFFB DIAG: spd={:.2f} steer={:.3f} rate={:.4f} lat={:.2f} drift={:.2f} rough={:.2f} sat={:.3f} phys={} basis=M70r{} cal={:.2f} mix={:.2f} beta={:.3f} yaw={:.3f} fslip={:.3f} vLat={:.5f} vLong={:.5f} step={:.5f} spdLen={:.5f} spdCorr={:.2f} steerSrc={} out={} invCF={} spring={} invSpring={} coeff={} damper={} dcoeff={} periodic={}",\n''',
'physics diagnostic format')
ffb = rep(ffb,
'''                satTorque,\n                Settings::UseNewInput ? "SDL" : "legacy",\n''',
'''                satTorque,\n                Settings::WheelFFBPhysicsSat\n                    ? (physicsSat_.calibrated()\n                        ? (physicsSat_.sampleValid() ? "ACTIVE" : "HOLD")\n                        : "CAL")\n                    : "OFF",\n                physicsSat_.forwardAxis(),\n                physicsSat_.calibrationConfidence(),\n                physicsSat_.activationBlend(),\n                physicsSat_.bodySlip(),\n                physicsSat_.yawRate(),\n                physicsSat_.frontSlip(),\n                physicsSat_.vLat(),\n                physicsSat_.vLong(),\n                physicsSat_.positionStep(),\n                physicsSat_.spdLen(),\n                physicsSat_.spdCorrelation(),\n                Settings::UseNewInput ? "SDL" : "legacy",\n''',
'physics diagnostic values')

ffb = rep(ffb,
'''        float smoothedLateral_ = 0.0f;\n        float prevSteer_ = 0.0f;\n        float crashImpulseForce_ = 0.0f;\n''',
'''        float smoothedLateral_ = 0.0f;\n        float prevSteer_ = 0.0f;\n        WheelPhysicsSatV1 physicsSat_{};\n        float crashImpulseForce_ = 0.0f;\n''',
'physics SAT state member')

# The previous implementation hooked CalcVibrationValues. In Tweaks that
# function is explicitly called BEFORE GamePlCar_Ctrl.call(car), so Physics SAT
# was always looking at the previous simulation state. Keep one GamePlCar_Ctrl
# owner and let hooks_forcefeedback call WheelFFB only after original car
# physics returns.
ffb = rep(ffb,
'''        inline static SafetyHookInline CalcVibrationHook_ = {};\n\n        static void __cdecl calc_vibration_hook(EVWORK_CAR* car)\n        {\n            CalcVibrationHook_.ccall<void>(car);\n            gWheelFFB.update(car);\n        }\n\n''',
'''        // FFB update ownership lives in the existing GamePlCar_Ctrl wrapper.\n        // Do not install another inline hook on the physics/vibration path.\n\n''',
'remove pre-physics CalcVibration hook')

ffb = rep(ffb,
'''        bool validate() override\n        {\n            // Always install the lightweight update hook. The engine itself\n            // gates on WheelFFBEnable, which makes F11 enable/disable genuinely\n            // live even when the game was launched with Enable=false.\n            return true;\n        }\n\n        bool apply() override\n        {\n            // Hook Tweaks' Xbox vibration calculation instead of installing a\n            // second GamePlCar_Ctrl hook. This gives one deterministic 60 Hz\n            // update after the game's car physics has been calculated.\n            CalcVibrationHook_ = safetyhook::create_inline(\n                reinterpret_cast<void*>(&CalcVibrationValues),\n                calc_vibration_hook);\n\n            if (!CalcVibrationHook_)\n            {\n                spdlog::error("WheelFFB: failed to hook CalcVibrationValues");\n                return false;\n            }\n\n            spdlog::info(\n                "WheelFFB: DirectInput COM hook installed; device init deferred to first gameplay tick");\n            return true;\n        }\n''',
'''        bool validate() override\n        {\n            // The engine itself gates on WheelFFBEnable; the existing Vibration\n            // GamePlCar_Ctrl wrapper invokes us after each 60 Hz physics tick.\n            return true;\n        }\n\n        bool apply() override\n        {\n            spdlog::info(\n                "WheelFFB: DirectInput COM engine registered; update runs after GamePlCar_Ctrl physics");\n            return true;\n        }\n''',
'post-physics WheelFFB hook ownership')

ffb = rep(ffb,
'''    WheelFFBHook WheelFFBHook::instance;\n}\n\n\nvoid WheelFFB_RequestDirectionTest(int direction)\n''',
'''    WheelFFBHook WheelFFBHook::instance;\n}\n\nvoid __cdecl WheelFFB_UpdateAfterPhysics(EVWORK_CAR* car)\n{\n    gWheelFFB.update(car);\n}\n\nvoid WheelFFB_RequestDirectionTest(int direction)\n''',
'export post-physics WheelFFB update')

vibration = rep(vibration,
'''extern "C"\n{\n    void __cdecl CalcVibrationValues(EVWORK_CAR* car);\n    long _ftol2(double);\n}\n''',
'''extern "C"\n{\n    void __cdecl CalcVibrationValues(EVWORK_CAR* car);\n    long _ftol2(double);\n}\n\nvoid __cdecl WheelFFB_UpdateAfterPhysics(EVWORK_CAR* car);\n''',
'declare post-physics WheelFFB update')

vibration = rep(vibration,
'''        CalcVibrationValues(car);\n        SetVibration(0, VibrationLeftMotor, VibrationRightMotor);\n\n        GamePlCar_Ctrl.call(car);\n''',
'''        // Keep legacy/gamepad vibration timing unchanged. Wheel FFB is\n        // intentionally different: sample the car only AFTER its physics Ctrl\n        // returns so body motion/yaw belong to the current simulation tick.\n        CalcVibrationValues(car);\n        SetVibration(0, VibrationLeftMotor, VibrationRightMotor);\n\n        GamePlCar_Ctrl.call(car);\n        WheelFFB_UpdateAfterPhysics(car);\n''',
'run WheelFFB after car physics')

ui = rep(ui,
'''    extern Setting<float> WheelFFBSteeringWeight;\n    extern Setting<float> WheelFFBGripLoss;\n''',
'''    extern Setting<float> WheelFFBSteeringWeight;\n    extern Setting<bool> WheelFFBPhysicsSat;\n    extern Setting<float> WheelFFBGripLoss;\n''',
'F11 physics SAT setting')

ui = rep(ui,
'''            ImGui::SliderFloat("Self-aligning Torque (SAT)", Settings::WheelFFBSteeringWeight.ptr(), 0.0f, 2.00f, "%.2f");\n            ImGui::SliderFloat("Grip-loss Unload", Settings::WheelFFBGripLoss.ptr(), 0.0f, 1.0f, "%.2f");\n''',
'''            ImGui::SliderFloat("Self-aligning Torque (SAT)", Settings::WheelFFBSteeringWeight.ptr(), 0.0f, 2.00f, "%.2f");\n            ImGui::Checkbox("Physics SAT v1 (body slip + yaw)", Settings::WheelFFBPhysicsSat.ptr());\n            if (ImGui::IsItemHovered())\n                ImGui::SetTooltip("Uses post-physics OutRun car motion/body heading to estimate front slip. Disable for the Round-16 Natural SAT comparison.");\n            ImGui::SliderFloat("Grip-loss Unload", Settings::WheelFFBGripLoss.ptr(), 0.0f, 1.0f, "%.2f");\n''',
'F11 physics SAT toggle')

ui = rep(ui,
'''            if (ImGui::Button("Load MOZA R3 Natural SAT"))\n            {\n''',
'''            if (ImGui::Button("Load MOZA R3 Physics SAT v1"))\n            {\n                Settings::WheelFFBEnable = true;\n                Settings::WheelFFBPhysicsSat = true;\n                Settings::WheelFFBGlobalStrength = 0.70f;\n                Settings::WheelFFBSpringStrength = 0.65f;\n                Settings::WheelFFBSpringSaturation = 0.95f;\n                Settings::WheelFFBDamperStrength = 0.28f;\n                Settings::WheelFFBSteeringWeight = 1.45f;\n                Settings::WheelFFBGripLoss = 0.65f;\n                Settings::WheelFFBWeightTransfer = 0.15f;\n                Settings::WheelFFBSlewRate = 0.040f;\n                Settings::WheelFFBRoadTexture = 0.30f;\n                Settings::WheelFFBTireSlip = 0.20f;\n                Settings::WheelFFBWallImpact = 0.38f;\n                Settings::WheelFFBUseHardwareSpring = true;\n                Settings::WheelFFBUseHardwareDamper = true;\n                Settings::WheelFFBInvertForce = true;\n                Settings::WheelFFBInvertSpring = false;\n                Settings::WheelFFBDebugLog = true;\n                Settings::VibrationMode = 0;\n                Settings::write(Module::UserIniPath);\n                status_ = "Loaded MOZA R3 Physics SAT v1: post-physics body-slip/yaw SAT with diagnostic logging. Saved to user.ini.";\n            }\n            ImGui::SameLine();\n\n            if (ImGui::Button("Load MOZA R3 Natural SAT"))\n            {\n                Settings::WheelFFBPhysicsSat = false;\n''',
'Physics SAT preset plus Natural A/B fallback')

ffb_path.write_text(ffb, encoding='utf-8')
ui_path.write_text(ui, encoding='utf-8')
vibration_path.write_text(vibration, encoding='utf-8')
print('Applied round-17 physics SAT: post-physics sampling, robust activation state and Natural A/B fallback')
