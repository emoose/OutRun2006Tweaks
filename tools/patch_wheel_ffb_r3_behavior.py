from pathlib import Path

path = Path("src/hooks_wheel_ffb.cpp")
text = path.read_text(encoding="utf-8")


def replace_once(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    text = text.replace(old, new, 1)


replace_once(
'''            // Road texture and tire-slip envelopes.\n            float roadAmp =\n                roughness * speedNorm * static_cast<float>(Settings::WheelFFBRoadTexture);\n            const float roadFreq = 25.0f + 12.0f * speedNorm;\n''',
'''            // Road texture and tire-slip envelopes.  sub_1149C0 returns\n            // ~0.25 for ordinary asphalt; that is a material baseline, not a\n            // request to vibrate the wheel.  The Xbox routine only enters its\n            // stronger surface branch above roughly 0.30, so remove that\n            // baseline here and ramp rough surfaces from 0.30 -> 0.85.\n            const float textureRoughness =\n                std::clamp((roughness - 0.30f) / 0.55f, 0.0f, 1.0f);\n            const float roadSpeedGate =\n                std::clamp((speedNorm - 0.05f) / 0.20f, 0.0f, 1.0f);\n            float roadAmp =\n                textureRoughness * roadSpeedGate *\n                static_cast<float>(Settings::WheelFFBRoadTexture);\n            const float roadFreq = 25.0f + 12.0f * speedNorm;\n''',
"road texture baseline"
)

replace_once(
'''            if (springEffect_)\n            {\n                update_spring(\n                    suppressSpringForImpact\n                        ? 0.0f\n                        : springStrength * warmupScale * recreateScale);\n            }\n\n            const float softwareSpring =\n                springEffect_\n                    ? 0.0f\n                    : -steer * static_cast<float>(Settings::WheelFFBSpringStrength) * speedCurve;\n''',
'''            // Make UseHardwareSpring a real live F11 switch.  Previously\n            // changing it to false after startup left the already-created\n            // GUID_Spring running, which made direction testing misleading.\n            if (!Settings::WheelFFBUseHardwareSpring && springEffect_)\n            {\n                update_spring(0.0f);\n                springEffect_->Stop();\n                safe_release_effect(springEffect_, "hardware spring disabled");\n                prevSpringCoefficient_ = 0;\n                prevSpringSaturation_ = 0;\n                springStrategy_ = -1;\n                spdlog::info("WheelFFB: hardware spring disabled live; using software spring");\n            }\n\n            if (springEffect_)\n            {\n                update_spring(\n                    suppressSpringForImpact\n                        ? 0.0f\n                        : springStrength * warmupScale * recreateScale);\n            }\n\n            const float softwareSpring =\n                springEffect_\n                    ? 0.0f\n                    : -steer * static_cast<float>(Settings::WheelFFBSpringStrength) * speedCurve;\n''',
"live hardware spring switch"
)

replace_once(
'''            const DWORD mag = static_cast<DWORD>(\n                std::clamp(magnitude, 0.0f, 1.0f) *\n                static_cast<float>(DI_FFNOMINALMAX));\n            frequency = std::clamp(frequency, 1.0f, 100.0f);\n            const DWORD period = static_cast<DWORD>(1000000.0f / frequency);\n\n            const bool silence = mag == 0 && state.lastMagnitude != 0;\n            const bool magChanged =\n                std::abs(static_cast<long>(mag) - static_cast<long>(state.lastMagnitude)) > 300;\n            const bool periodChanged =\n''',
'''            // Very small hardware-sine magnitudes can remain audible on DD\n            // bases.  Snap them to zero, and update much more aggressively on\n            // falling magnitude so an off-road effect cannot remain latched\n            // after the car returns to asphalt.\n            const float magnitudeClamped = std::clamp(magnitude, 0.0f, 1.0f);\n            const DWORD mag = magnitudeClamped < 0.01f\n                ? 0u\n                : static_cast<DWORD>(\n                    magnitudeClamped * static_cast<float>(DI_FFNOMINALMAX));\n            frequency = std::clamp(frequency, 1.0f, 100.0f);\n            const DWORD period = static_cast<DWORD>(1000000.0f / frequency);\n\n            const long magDelta = std::abs(\n                static_cast<long>(mag) - static_cast<long>(state.lastMagnitude));\n            const bool silence = mag == 0 && state.lastMagnitude != 0;\n            const bool magChanged =\n                magDelta > 60 ||\n                (state.lastMagnitude > 0 &&\n                 magDelta * 5 > static_cast<long>(state.lastMagnitude));\n            const bool periodChanged =\n''',
"periodic stale magnitude"
)

replace_once(
'''                    crashImpulseForce_ =\n                        direction * 1.5f *\n                        static_cast<float>(Settings::WheelFFBWallImpact);\n''',
'''                    const float severity =\n                        std::clamp((speedDrop - 0.03f) / 0.12f, 0.0f, 1.0f);\n                    crashImpulseForce_ =\n                        direction * (1.7f + 0.8f * severity) *\n                        static_cast<float>(Settings::WheelFFBWallImpact);\n''',
"speed-drop impact strength"
)

replace_once(
'''                crashImpulseForce_ =\n                    direction * 1.2f *\n                    static_cast<float>(Settings::WheelFFBWallImpact);\n''',
'''                crashImpulseForce_ =\n                    direction * 1.9f *\n                    static_cast<float>(Settings::WheelFFBWallImpact);\n''',
"collision-flag impact strength"
)

replace_once(
'''                if (crashImpulseTimer_ > CrashCooldownFrames)\n                {\n                    float envelope = 1.0f;\n                    if (crashImpulseTimer_ <= 85)\n                    {\n                        envelope =\n                            static_cast<float>(crashImpulseTimer_ - CrashCooldownFrames) /\n                            5.0f;\n                    }\n                    result += crashImpulseForce_ * envelope;\n                }\n''',
'''                if (crashImpulseTimer_ > CrashCooldownFrames)\n                {\n                    // A short kick/rebound is much easier to feel on a DD wheel\n                    // than the old soft one-direction 10-frame push.\n                    const int impactFrame = CrashTimerFrames - crashImpulseTimer_;\n                    if (impactFrame < 3)\n                        result += crashImpulseForce_;\n                    else if (impactFrame < 6)\n                        result -= crashImpulseForce_ * 0.55f;\n                    else\n                        result += crashImpulseForce_ * 0.20f;\n                }\n''',
"impact pulse envelope"
)

path.write_text(text, encoding="utf-8")
print("Applied R3 FFB behavior patch")
