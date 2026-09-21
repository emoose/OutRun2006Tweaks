#pragma once

#include "vr/core/frame_types.hpp"
#include "vr/core/transport.hpp"
#include "vr/game/game_adapter.hpp"

namespace OutRunVR::D3D9
{
    enum class DrawClass
    {
        Ignore,
        World,
        ScreenSpace,
        Unsafe,
    };

    struct FrameContext
    {
        Core::RenderPose pose{};
        Game::StereoMatrices matrices{};
        Core::PresentedFrame result{};
    };

    class IStereoBackend
    {
    public:
        virtual ~IStereoBackend() = default;
        virtual bool beginFrame(FrameContext& frame) noexcept = 0;
        virtual DrawClass classifyCurrentDraw() const noexcept = 0;
        virtual bool drawWorldStereo(FrameContext& frame) noexcept = 0;
        virtual bool drawScreenSpaceStereo(FrameContext& frame) noexcept = 0;
        virtual bool finishFrame(FrameContext& frame) noexcept = 0;
        virtual void invalidate() noexcept = 0;
    };
}
