#pragma once

#include "vr/core/frame_types.hpp"

#include <cstdint>

namespace OutRunVR::Game
{
    struct StereoMatrices
    {
        float leftWvp[16]{};
        float rightWvp[16]{};
        std::uint64_t poseId{};
    };

    class IGameAdapter
    {
    public:
        virtual ~IGameAdapter() = default;

        // Called once for a presented game frame. Implementations may latch an
        // already-published host pose, but must never advance simulation/input/FFB.
        virtual bool latchRenderPose(Core::RenderPose& outPose) noexcept = 0;

        // Converts the verified game camera boundary into eye-specific matrices.
        // This is the only layer allowed to know OutRun's c64 WorldViewProjection facts.
        virtual bool buildStereoMatrices(const Core::RenderPose& pose,
            StereoMatrices& outMatrices) noexcept = 0;

        virtual void onPresent() noexcept = 0;
        virtual void onReset() noexcept = 0;
    };
}
