#pragma once

#include <array>
#include <cstdint>

namespace OutRunVR::Core
{
    struct Fov
    {
        float angleLeft{};
        float angleRight{};
        float angleUp{};
        float angleDown{};
    };

    struct Quaternion
    {
        float x{};
        float y{};
        float z{};
        float w{1.0f};
    };

    struct Vector3
    {
        float x{};
        float y{};
        float z{};
    };

    struct EyeView
    {
        Quaternion orientation{};
        Vector3 positionMeters{};
        Fov fov{};
    };

    struct RenderPose
    {
        std::uint64_t poseId{};
        std::int64_t predictedDisplayTime{};
        std::int64_t sampleQpc{};
        Quaternion headOrientation{};
        Vector3 headPositionMeters{};
        std::array<EyeView, 2> eyes{};
        std::uint32_t referenceSpaceGeneration{};
        bool orientationValid{};
        bool positionValid{};
        bool viewsValid{};
        bool shouldRender{};
    };

    enum class PresentationMode : std::uint32_t
    {
        Unknown = 0,
        Gameplay = 1,
        Theater = 2,
    };

    enum class FrameFailure : std::uint32_t
    {
        None = 0,
        MissingPose,
        ResourceUnavailable,
        UnsafeMrt,
        ViewportUnavailable,
        LeftProjectionFailed,
        LeftDrawFailed,
        RightStateFailed,
        RightProjectionFailed,
        RightDrawFailed,
        RestoreFailed,
        ComposeFailed,
        PresentFailed,
        DepthStateChanged,
        PoseMismatch,
        DepthUnsynchronized,
        ClearFailed,
        WorldClassificationFailed,
        StencilUnsynchronized,
        OffscreenWorld,
        OcclusionQueryActive,
        TransportBackpressure,
        TransportSynchronizationFailed,
    };

    struct PresentedFrame
    {
        std::uint64_t frameId{};
        std::uint64_t renderPoseId{};
        std::int64_t presentQpc{};
        PresentationMode presentation{PresentationMode::Unknown};
        FrameFailure failure{FrameFailure::None};
        std::uint32_t width{};
        std::uint32_t height{};
        std::array<EyeView, 2> renderedEyes{};
        bool stereoComplete{};
        bool worldStereo{};
        bool drawDuplicated{};
    };
}
