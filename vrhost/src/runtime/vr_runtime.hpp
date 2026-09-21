#pragma once

#include "vr/core/frame_types.hpp"
#include "vr/core/transport.hpp"

#include <cstdint>

namespace OutRunVR::Host
{
    struct RuntimeFrame
    {
        Core::RenderPose pose{};
        bool sessionRunning{};
        bool visible{};
        bool focused{};
    };

    class IVrRuntime
    {
    public:
        virtual ~IVrRuntime() = default;
        virtual Core::AdapterId requiredAdapter() const noexcept = 0;
        virtual bool beginFrame(RuntimeFrame& outFrame) noexcept = 0;
        virtual bool submitProjection(const Core::PresentedFrame& frame) noexcept = 0;
        virtual bool submitTheater(const Core::PresentedFrame& frame) noexcept = 0;
        virtual void endFrame() noexcept = 0;
        virtual void invalidateSessionResources() noexcept = 0;
    };
}
