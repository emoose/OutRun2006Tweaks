#pragma once

// OutRun vehicle-dynamics estimator for steering FFB.
//
// This class deliberately owns no DirectInput effects and computes no FFB
// torque. Its only job is to estimate reusable vehicle state from the game:
// body slip (beta), yaw rate, front-slip proxy and local motion. WheelFFBEngine
// decides how those signals affect SAT, damping and tire scrub.
class WheelVehicleDynamics
{
public:
    void reset()
    {
        positionValid_ = false;
        headingValid_ = false;
        calibrated_ = false;
        sampleValid_ = false;
        forwardAxis_ = 0;
        forwardSign_ = 1.0f;
        calibrationSamples_ = 0;
        calibrationScoreX_ = 0.0f;
        calibrationScoreZ_ = 0.0f;
        calibrationSignedX_ = 0.0f;
        calibrationSignedZ_ = 0.0f;
        calibrationConfidence_ = 0.0f;
        activationBlend_ = 0.0f;
        invalidTicks_ = 0;
        discontinuityCount_ = 0;
        prevPosition_ = D3DVECTOR{};
        prevHeading_ = 0.0f;
        prevSteerValid_ = false;
        prevSteer_ = 0.0f;
        steerRate_ = 0.0f;
        bodySlip_ = 0.0f;
        yawRate_ = 0.0f;
        frontSlip_ = 0.0f;
        rawBodySlip_ = 0.0f;
        rawYawRate_ = 0.0f;
        rawFrontSlip_ = 0.0f;
        bodySlipBlend_ = 0.18f;
        yawRateBlend_ = 0.20f;
        frontSlipBlend_ = 0.24f;
        vLong_ = 0.0f;
        vLat_ = 0.0f;
        positionStep_ = 0.0f;
        motionScaleEma_ = 0.0f;
        motionScaleSamples_ = 0;
        spdLen_ = 0.0f;
        spdCorrelation_ = 0.0f;
    }

    void reset_dynamic()
    {
        // Preserve the proven matrix basis while dropping every time-domain
        // sample, including the warp-detection motion scale. Menu/focus/F11
        // transitions must not compare a new segment against the old segment's
        // movement scale before Physics SAT resumes.
        positionValid_ = false;
        headingValid_ = false;
        prevPosition_ = D3DVECTOR{};
        positionStep_ = 0.0f;
        motionScaleEma_ = 0.0f;
        motionScaleSamples_ = 0;
        spdLen_ = 0.0f;
        spdCorrelation_ = 0.0f;
        clear_dynamic_state();
    }

    void update(
        EVWORK_CAR* car,
        float steer,
        float speedNorm,
        float lateralLoadSmooth)
    {
        sampleValid_ = false;
        vLong_ = 0.0f;
        vLat_ = 0.0f;
        positionStep_ = 0.0f;
        spdLen_ = 0.0f;
        spdCorrelation_ = 0.0f;

        if (!car)
        {
            decay_invalid_sample();
            return;
        }

        const D3DVECTOR current = car->position_14;
        if (!std::isfinite(current.x) || !std::isfinite(current.z))
        {
            decay_invalid_sample();
            return;
        }

        if (!positionValid_)
        {
            prevPosition_ = current;
            positionValid_ = true;
            return;
        }

        const float dx = current.x - prevPosition_.x;
        const float dz = current.z - prevPosition_.z;
        prevPosition_ = current;
        const float motionLen = std::sqrt(dx * dx + dz * dz);
        positionStep_ = std::isfinite(motionLen) ? motionLen : 0.0f;

        // A stop/restart must not inherit the previous corner's beta/yaw/front
        // slip. Keep the proven matrix basis but restart only dynamic state.
        if (speedNorm <= 0.04f)
        {
            clear_dynamic_state();
            return;
        }

        if (!std::isfinite(motionLen) || motionLen <= 0.00001f)
        {
            decay_invalid_sample();
            return;
        }

        // Detect restart/warp discontinuities without assuming OutRun world
        // units. Compare normalized step length with its rolling scale.
        const float motionScale = motionLen / std::max(speedNorm, 0.05f);
        if (motionScaleSamples_ >= 12 &&
            motionScaleEma_ > 0.00001f &&
            motionScale > motionScaleEma_ * 5.0f)
        {
            ++discontinuityCount_;
            clear_dynamic_state();
            return;
        }
        if (std::isfinite(motionScale))
        {
            if (motionScaleSamples_ == 0)
                motionScaleEma_ = motionScale;
            else
                motionScaleEma_ += (motionScale - motionScaleEma_) * 0.05f;
            ++motionScaleSamples_;
        }

        const float motionX = dx / motionLen;
        const float motionZ = dz / motionLen;
        const D3DMATRIX& body = car->matrix_70;

        // OutRun uses row-vector transforms, but whether local X or Z is the
        // vehicle-forward basis is verified from several straight rolling ticks
        // rather than guessed from one frame.
        if (!calibrated_ &&
            speedNorm > 0.12f &&
            std::abs(steer) < 0.08f &&
            lateralLoadSmooth < 0.12f)
        {
            const float xLen = std::sqrt(body._11 * body._11 + body._13 * body._13);
            const float zLen = std::sqrt(body._31 * body._31 + body._33 * body._33);
            if (std::isfinite(xLen) && std::isfinite(zLen) &&
                xLen > 0.0001f && zLen > 0.0001f)
            {
                const float xDot =
                    (body._11 / xLen) * motionX + (body._13 / xLen) * motionZ;
                const float zDot =
                    (body._31 / zLen) * motionX + (body._33 / zLen) * motionZ;
                calibrationScoreX_ += std::abs(xDot);
                calibrationScoreZ_ += std::abs(zDot);
                calibrationSignedX_ += xDot;
                calibrationSignedZ_ += zDot;
                ++calibrationSamples_;

                constexpr int CalibrationSamplesRequired = 12;
                if (calibrationSamples_ >= CalibrationSamplesRequired)
                {
                    const float invSamples = 1.0f / static_cast<float>(calibrationSamples_);
                    const float xScore = calibrationScoreX_ * invSamples;
                    const float zScore = calibrationScoreZ_ * invSamples;
                    const float bestScore = std::max(xScore, zScore);
                    const float secondScore = std::min(xScore, zScore);
                    calibrationConfidence_ = bestScore - secondScore;

                    if (bestScore >= 0.85f && calibrationConfidence_ >= 0.25f)
                    {
                        forwardAxis_ = zScore >= xScore ? 3 : 1;
                        const float signedScore = forwardAxis_ == 3
                            ? calibrationSignedZ_
                            : calibrationSignedX_;
                        forwardSign_ = signedScore >= 0.0f ? 1.0f : -1.0f;
                        calibrated_ = true;
                        clear_dynamic_state();
                    }
                    else if (calibrationSamples_ >= CalibrationSamplesRequired * 4)
                    {
                        // Do not let a noisy launch poison the basis average for
                        // the rest of the race. Retry from a fresh straight-line
                        // window while Natural SAT remains fully available.
                        calibrationSamples_ = 0;
                        calibrationScoreX_ = 0.0f;
                        calibrationScoreZ_ = 0.0f;
                        calibrationSignedX_ = 0.0f;
                        calibrationSignedZ_ = 0.0f;
                        calibrationConfidence_ = 0.0f;
                    }
                }
            }
        }

        if (!calibrated_)
            return;

        float forwardX = forwardAxis_ == 3 ? body._31 : body._11;
        float forwardZ = forwardAxis_ == 3 ? body._33 : body._13;
        forwardX *= forwardSign_;
        forwardZ *= forwardSign_;
        const float forwardLen = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
        if (!std::isfinite(forwardLen) || forwardLen <= 0.0001f)
        {
            decay_invalid_sample();
            return;
        }
        forwardX /= forwardLen;
        forwardZ /= forwardLen;

        // Right-positive local motion. Position delta remains authoritative until
        // spd_mb_20's coordinate space/scale is proven by real-lap telemetry.
        const float rightX = forwardZ;
        const float rightZ = -forwardX;
        vLong_ = dx * forwardX + dz * forwardZ;
        vLat_ = dx * rightX + dz * rightZ;
        if (vLong_ <= 0.0f)
        {
            clear_dynamic_state();
            return;
        }
        rawBodySlip_ = std::clamp(
            std::atan2(vLat_, std::max(std::abs(vLong_), 0.00001f)),
            -0.70f, 0.70f);

        // Tyre transient response is fundamentally distance-based (relaxation
        // length), so a fixed time-domain low-pass becomes increasingly late as
        // speed rises. OutRun does not expose a physical metres/second scale,
        // therefore use a conservative speed-adaptive blend while retaining
        // smoothing at parking/launch speeds.
        const float transientT0 = std::clamp(
            (speedNorm - 0.08f) / 0.72f, 0.0f, 1.0f);
        const float transientT =
            transientT0 * transientT0 * (3.0f - 2.0f * transientT0);
        bodySlipBlend_ = 0.18f + (0.34f - 0.18f) * transientT;
        yawRateBlend_ = 0.20f + (0.38f - 0.20f) * transientT;
        frontSlipBlend_ = 0.24f + (0.58f - 0.24f) * transientT;
        bodySlip_ += (rawBodySlip_ - bodySlip_) * bodySlipBlend_;

        const float heading = std::atan2(forwardX, forwardZ);
        if (!headingValid_)
        {
            prevHeading_ = heading;
            headingValid_ = true;
            return;
        }

        constexpr float Pi = 3.14159265359f;
        constexpr float TwoPi = 6.28318530718f;
        float headingDelta = heading - prevHeading_;
        prevHeading_ = heading;
        while (headingDelta > Pi)
            headingDelta -= TwoPi;
        while (headingDelta < -Pi)
            headingDelta += TwoPi;
        if (std::abs(headingDelta) >= 0.35f)
        {
            ++discontinuityCount_;
            clear_dynamic_state();
            return;
        }

        // Called once per fixed OutRun simulation tick, independent of render FPS.
        rawYawRate_ = std::clamp(headingDelta * 60.0f, -3.5f, 3.5f);
        yawRate_ += (rawYawRate_ - yawRate_) * yawRateBlend_;

        // Track steering velocity separately from body/yaw filtering. Physics SAT
        // used to wait for several filtered front-slip ticks before reacting to a
        // fast counter-steer, which made a DD wheel feel strangely passive even
        // though the steady-state torque was correct. A bounded one-to-two tick
        // steering lead gives the tyre proxy the phase response it needs without
        // changing the eventual steady-state slip angle.
        float rawSteerRate = 0.0f;
        if (prevSteerValid_)
            rawSteerRate = std::clamp((steer - prevSteer_) * 60.0f, -12.0f, 12.0f);
        else
            prevSteerValid_ = true;
        prevSteer_ = steer;
        steerRate_ += (rawSteerRate - steerRate_) * 0.55f;

        // Bicycle-model-inspired front-slip proxy:
        // alpha_f ~= road-wheel-angle - beta - a*r/v.
        constexpr float RoadWheelLockRad = 0.52f;
        const float steeringLeadSeconds = 0.018f + 0.012f * transientT;
        const float steeringLead = std::clamp(
            steerRate_ * steeringLeadSeconds, -0.10f, 0.10f);
        const float roadWheelAngle = std::clamp(
            steer + steeringLead, -1.0f, 1.0f) * RoadWheelLockRad;
        const float yawLeadSeconds = 0.10f - 0.045f * speedNorm;
        const float baseFrontSlip =
            roadWheelAngle - bodySlip_ - yawRate_ * yawLeadSeconds;
        rawFrontSlip_ = std::clamp(baseFrontSlip, -0.70f, 0.70f);

        // When the desired tyre torque changes side, do not let the old filtered
        // slip linger for several extra physics ticks. The normal speed-adaptive
        // filter remains untouched for ordinary force build; only a genuine sign
        // reversal gets this fast release/cross-over path.
        const bool frontSlipReversing =
            rawFrontSlip_ * frontSlip_ < -0.0004f;
        const float effectiveFrontSlipBlend = frontSlipReversing
            ? std::max(frontSlipBlend_, 0.82f)
            : frontSlipBlend_;
        frontSlip_ += (rawFrontSlip_ - frontSlip_) * effectiveFrontSlipBlend;

        // Telemetry-only validation for the game's native speed vector.
        const float spdX = car->spd_mb_20.x;
        const float spdZ = car->spd_mb_20.z;
        const float spdLen = std::sqrt(spdX * spdX + spdZ * spdZ);
        if (std::isfinite(spdLen) && spdLen > 0.0001f)
        {
            spdLen_ = spdLen;
            spdCorrelation_ =
                (spdX / spdLen) * motionX + (spdZ / spdLen) * motionZ;
        }

        sampleValid_ = true;
        invalidTicks_ = 0;
        activationBlend_ = std::min(1.0f, activationBlend_ + (1.0f / 24.0f));
    }

    bool calibrated() const { return calibrated_; }
    bool sampleValid() const { return sampleValid_; }
    int forwardAxis() const { return forwardAxis_; }
    float forwardSign() const { return forwardSign_; }
    int discontinuityCount() const { return discontinuityCount_; }
    float calibrationConfidence() const { return calibrationConfidence_; }
    float activationBlend() const { return activationBlend_; }
    float bodySlip() const { return bodySlip_; }
    float yawRate() const { return yawRate_; }
    float frontSlip() const { return frontSlip_; }
    float rawBodySlip() const { return rawBodySlip_; }
    float rawYawRate() const { return rawYawRate_; }
    float rawFrontSlip() const { return rawFrontSlip_; }
    float steerRate() const { return steerRate_; }
    float bodySlipBlend() const { return bodySlipBlend_; }
    float yawRateBlend() const { return yawRateBlend_; }
    float frontSlipBlend() const { return frontSlipBlend_; }
    float vLong() const { return vLong_; }
    float vLat() const { return vLat_; }
    float positionStep() const { return positionStep_; }
    float motionScale() const { return motionScaleEma_; }
    float spdLen() const { return spdLen_; }
    float spdCorrelation() const { return spdCorrelation_; }

private:
    void clear_dynamic_state()
    {
        headingValid_ = false;
        sampleValid_ = false;
        prevHeading_ = 0.0f;
        prevSteerValid_ = false;
        prevSteer_ = 0.0f;
        steerRate_ = 0.0f;
        bodySlip_ = 0.0f;
        yawRate_ = 0.0f;
        frontSlip_ = 0.0f;
        rawBodySlip_ = 0.0f;
        rawYawRate_ = 0.0f;
        rawFrontSlip_ = 0.0f;
        bodySlipBlend_ = 0.18f;
        yawRateBlend_ = 0.20f;
        frontSlipBlend_ = 0.24f;
        vLong_ = 0.0f;
        vLat_ = 0.0f;
        activationBlend_ = 0.0f;
        invalidTicks_ = 0;
    }

    void decay_invalid_sample()
    {
        sampleValid_ = false;
        headingValid_ = false; // The next heading is a baseline, not a one-tick derivative.
        positionValid_ = false; // Never span a missing position with a one-tick velocity.
        prevSteerValid_ = false; // Never derive steering velocity across a missing sample.
        steerRate_ = 0.0f;
        if (!calibrated_)
            return;

        ++invalidTicks_;
        bodySlip_ *= 0.55f;
        yawRate_ *= 0.55f;
        frontSlip_ *= 0.55f;
        rawBodySlip_ *= 0.55f;
        rawYawRate_ *= 0.55f;
        rawFrontSlip_ *= 0.55f;
        activationBlend_ *= 0.85f;

        // Once telemetry has been invalid for several ticks, clear all dynamic
        // slip/yaw state. Calibration/basis remains valid so recovery is quick.
        if (invalidTicks_ > 4)
            clear_dynamic_state();
    }

    bool positionValid_ = false;
    bool headingValid_ = false;
    bool calibrated_ = false;
    bool sampleValid_ = false;
    int forwardAxis_ = 0;
    float forwardSign_ = 1.0f;
    int calibrationSamples_ = 0;
    float calibrationScoreX_ = 0.0f;
    float calibrationScoreZ_ = 0.0f;
    float calibrationSignedX_ = 0.0f;
    float calibrationSignedZ_ = 0.0f;
    float calibrationConfidence_ = 0.0f;
    float activationBlend_ = 0.0f;
    int invalidTicks_ = 0;
    int discontinuityCount_ = 0;
    D3DVECTOR prevPosition_{};
    float prevHeading_ = 0.0f;
    bool prevSteerValid_ = false;
    float prevSteer_ = 0.0f;
    float steerRate_ = 0.0f;
    float bodySlip_ = 0.0f;
    float yawRate_ = 0.0f;
    float frontSlip_ = 0.0f;
    float rawBodySlip_ = 0.0f;
    float rawYawRate_ = 0.0f;
    float rawFrontSlip_ = 0.0f;
    float bodySlipBlend_ = 0.18f;
    float yawRateBlend_ = 0.20f;
    float frontSlipBlend_ = 0.24f;
    float vLong_ = 0.0f;
    float vLat_ = 0.0f;
    float positionStep_ = 0.0f;
    float motionScaleEma_ = 0.0f;
    int motionScaleSamples_ = 0;
    float spdLen_ = 0.0f;
    float spdCorrelation_ = 0.0f;
};