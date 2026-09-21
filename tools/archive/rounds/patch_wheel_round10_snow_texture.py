from pathlib import Path
import runpy

ffb_path = Path('src/hooks_wheel_ffb.cpp')
ffb = ffb_path.read_text(encoding='utf-8')


def rep(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'round10 {label}: expected exactly one match, got {count}')
    print(f'ROUND10 patched: {label}')
    return text.replace(old, new, 1)

# Snowy Mountain (stage 4) and Ice Scape (stage 19), plus their reverse
# variants (+30), use a high steady surface roughness value. That value was
# designed for gamepad rumble and becomes a continuous 25-37 Hz tone on a DD
# wheel. Keep the general surface model unchanged elsewhere, but heavily
# attenuate only the road-texture periodic on these snow/ice stages. With the
# shipped v0.1 RoadTexture/GlobalStrength values, the resulting magnitude stays
# below the existing 1% periodic snap-to-zero threshold during normal snow-road
# driving. Steering spring/damper, cornering load, tyre slip and impacts are not
# changed.
old = '''            const float textureRoughness =
                std::clamp((roughness - 0.30f) / 0.55f, 0.0f, 1.0f);
            const float roadSpeedGate =
                std::clamp((speedNorm - 0.05f) / 0.20f, 0.0f, 1.0f);
            float roadAmp =
                textureRoughness * roadSpeedGate *
                static_cast<float>(Settings::WheelFFBRoadTexture) * outputStrength;
            const float roadFreq = 25.0f + 12.0f * speedNorm;
'''
new = '''            const float textureRoughness =
                std::clamp((roughness - 0.30f) / 0.55f, 0.0f, 1.0f);
            const float roadSpeedGate =
                std::clamp((speedNorm - 0.05f) / 0.20f, 0.0f, 1.0f);

            // Xbox gamepad rumble treats snow/ice as a continuously rough
            // material. On a DD wheel that becomes an unpleasant constant
            // high-frequency sine. Stage IDs follow Game::StageNames: 4/19
            // are Snowy Mountain/Ice Scape and +30 are their reverse variants.
            const int stageNumber = Game::GetNowStageNum(8);
            const int uniqueStage = Game::GetStageUniqueNum(stageNumber);
            const bool snowOrIceStage =
                uniqueStage == 4 || uniqueStage == 19 ||
                uniqueStage == 34 || uniqueStage == 49;
            constexpr float SnowIceRoadTextureScale = 0.04f;
            const float stageRoadTextureScale =
                snowOrIceStage ? SnowIceRoadTextureScale : 1.0f;

            float roadAmp =
                textureRoughness * roadSpeedGate *
                static_cast<float>(Settings::WheelFFBRoadTexture) * outputStrength *
                stageRoadTextureScale;
            const float roadFreq = 25.0f + 12.0f * speedNorm;
'''
ffb = rep(ffb, old, new, 'suppress continuous snow/ice road sine')

old_splash = '''                splashAmp_ =
                    (roughness - 0.7f) * speedNorm * 0.75f * roadTextureScale * outputStrength;
'''
new_splash = '''                splashAmp_ =
                    (roughness - 0.7f) * speedNorm * 0.75f * roadTextureScale *
                    outputStrength * stageRoadTextureScale;
'''
ffb = rep(ffb, old_splash, new_splash, 'apply snow/ice attenuation to road splash periodic')

ffb_path.write_text(ffb, encoding='utf-8')
print('Applied round-10 snow/ice continuous road-vibration suppression')

# Round-11: replace the old signed lateral ConstantForce with a deliberately
# stronger sim-style pseudo self-aligning torque model.
runpy.run_path('tools/patch_wheel_round11_strong_sat.py', run_name='__main__')

# Round-12: feed SAT from the actual SDL multi-device steering state and remove
# the duplicate FFB page from the Controls dialog.
runpy.run_path('tools/patch_wheel_round12_sat_input_ui.py', run_name='__main__')

# Round-13: retain every important FFB control and safe direction test on the
# single main Force Feedback page.
runpy.run_path('tools/patch_wheel_round13_ffb_ui_complete.py', run_name='__main__')

# Round-14: make the default SDL input ownership explicit. Input Bindings owns
# steering/pedals/buttons/calibration; the main page owns force feedback only.
runpy.run_path('tools/patch_wheel_round14_input_ui_roles.py', run_name='__main__')

# Round-15: R3 hardware test correction. Reverse the ConstantForce/SAT actuator
# direction and substantially strengthen ordinary-corner SAT and centering.
runpy.run_path('tools/patch_wheel_round15_strong_r3_sat.py', run_name='__main__')

# Round-16: hardware feedback showed the strong power curve could snap around
# centre. Replace it with a progressive SAT shape, fade generic Spring at speed,
# hide the duplicate generic WheelFFB settings and suppress SDL rumble while the
# DirectInput COM wheel engine owns FFB output.
runpy.run_path('tools/patch_wheel_round16_natural_single_owner.py', run_name='__main__')

# Round-17: experimental vehicle-dynamics SAT. Keep Round-16 Natural SAT as an
# A/B fallback, but derive the active SAT direction from body slip + yaw/front
# slip so counter-steer can stay continuous through wheel centre.
runpy.run_path('tools/patch_wheel_round17_physics_sat.py', run_name='__main__')
