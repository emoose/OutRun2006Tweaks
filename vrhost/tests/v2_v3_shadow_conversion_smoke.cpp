#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "vr/ipc/shadow_legacy_v2.hpp"

namespace
{
    bool Near(float a, float b, float epsilon = 1.0e-4f)
    {
        return std::fabs(a - b) <= epsilon;
    }

    std::uint32_t FloatBits(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    std::uint32_t SignedBits(std::int32_t value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    std::int16_t PackSnorm16(float value)
    {
        if (value > 1.0f) value = 1.0f;
        if (value < -1.0f) value = -1.0f;
        return static_cast<std::int16_t>(std::lround(value * 32767.0f));
    }

    void PackEyeOrientations(OutRunVR::SharedPoseState& pose)
    {
        const float eyes[2][4]{
            {0.0f, 0.05f, 0.0f, 0.9987492f},
            {0.0f, -0.05f, 0.0f, 0.9987492f}
        };
        std::int16_t packed[8]{};
        for (int eye = 0; eye < 2; ++eye)
            for (int c = 0; c < 4; ++c)
                packed[eye * 4 + c] = PackSnorm16(eyes[eye][c]);
        std::memcpy(pose.runtimeName + OutRunVR::PackedEyeOrientationOffset,
            packed, sizeof(packed));
    }
}

int main()
{
    using namespace OutRunVR;
    using namespace OutRunVR::IpcV3;

    SharedPoseState pose{};
    pose.magic = SharedMagic;
    pose.protocolVersion = SharedProtocolVersion;
    pose.structSize = sizeof(pose);
    pose.sequence = 42;
    pose.hostPid = 1234;
    pose.clientPid = 5678;
    pose.flags = OutRunVR::HostAlive | OutRunVR::OrientationValid |
        OutRunVR::PositionValid | OutRunVR::SessionVisible |
        OutRunVR::SessionFocused | OutRunVR::StereoViewsValid |
        OutRunVR::StereoEyeOrientationValid | OutRunVR::HostShouldRender |
        OutRunVR::HostDirectGpuTransport | OutRunVR::HostDirectGpuReady |
        OutRunVR::HostAdapterLuidValid;
    pose.sampleQpc = 0x1122334455667788LL;
    pose.orientation[0] = 0.1f;
    pose.orientation[1] = -0.2f;
    pose.orientation[2] = 0.3f;
    pose.orientation[3] = 0.92736185f;
    pose.position[0] = 0.25f;
    pose.position[1] = 1.61f;
    pose.position[2] = -0.4f;
    pose.eyeFov[0] = {-0.80f, 0.72f, 0.84f, -0.77f};
    pose.eyeFov[1] = {-0.73f, 0.81f, 0.83f, -0.78f};
    pose.recommendedWidth[0] = pose.recommendedWidth[1] = 2016;
    pose.recommendedHeight[0] = pose.recommendedHeight[1] = 2208;
    pose.hostAdapterLuidLow = 0x89ABCDEFu;
    pose.hostAdapterLuidHigh = SignedBits(-17);
    pose.clientAdapterLuidLow = pose.hostAdapterLuidLow;
    pose.clientAdapterLuidHigh = pose.hostAdapterLuidHigh;
    pose.clientInteropProbeHandle = 0x12345678u;
    pose.clientInteropProbeToken = 91;
    pose.hostInteropProbeAckToken = 91;
    pose.hostDirectConsumedFrameId = 77;
    std::memcpy(pose.runtimeName, "VirtualDesktopXR test runtime", 29);
    pose.reserved[HostReferenceSpaceGenerationIndex] = 9;
    pose.reserved[HostEyeOffsetLeftXIndex] = FloatBits(-0.032f);
    pose.reserved[HostEyeOffsetLeftYIndex] = FloatBits(0.001f);
    pose.reserved[HostEyeOffsetLeftZIndex] = FloatBits(0.002f);
    pose.reserved[HostEyeOffsetRightXIndex] = FloatBits(0.032f);
    pose.reserved[HostEyeOffsetRightYIndex] = FloatBits(0.001f);
    pose.reserved[HostEyeOffsetRightZIndex] = FloatBits(0.002f);
    pose.reserved[ClientHeartbeatIndex] = 321;
    pose.reserved[ClientFlagsIndex] = ClientHookAlive | ClientHostPoseValid |
        ClientRendererWvpVerified | ClientRendererPoseInjected | ClientFrameCompleted |
        ClientStereoActive | ClientStereoWorldDraw | ClientStereoDrawDuplicated;
    pose.reserved[ClientPresentationModeIndex] = PresentationGameplay;
    pose.reserved[ClientGameStateIndex] = 6;
    pose.reserved[ClientStereoStateIndex] = StereoSbsActive;
    pose.reserved[ClientStereoBackbufferWidthIndex] = 3440;
    pose.reserved[ClientStereoBackbufferHeightIndex] = 1440;
    PackEyeOrientations(pose);

    const HostState host = ShadowV2::HostStateFromV2(pose, 15);
    if (host.poseId != 42 || host.hostPid != 1234 || host.referenceSpaceGeneration != 9 ||
        host.directTransportGeneration != 15 || host.sampleQpc != pose.sampleQpc ||
        host.adapterLuidLow != pose.hostAdapterLuidLow || host.adapterLuidHigh != -17 ||
        host.recommendedWidth[0] != 2016 || host.recommendedHeight[1] != 2208 ||
        !(host.flags & DirectGpuTransportSupported) || !(host.flags & DirectGpuTransportReady) ||
        !(host.flags & AdapterLuidValid) || !Near(host.eyes[0].positionMeters[0], -0.032f) ||
        !Near(host.eyes[1].positionMeters[0], 0.032f) || !Near(host.eyes[0].fov.angleLeft, -0.80f) ||
        host.eyes[0].orientation[3] < 0.99f || host.eyes[1].orientation[3] < 0.99f)
    {
        std::cerr << "HostState v2->v3 conversion failed\n";
        return 1;
    }

    SharedRenderFrameState frame{};
    frame.magic = RenderFrameMagic;
    frame.protocolVersion = RenderFrameProtocolVersion;
    frame.structSize = sizeof(frame);
    frame.sequence = 10;
    frame.clientPid = pose.clientPid;
    frame.state = StereoSbsActive;
    frame.frameId = 77;
    frame.sourcePoseSequence = pose.sequence;
    frame.presentationMode = PresentationGameplay;
    frame.flags = RenderFrameStereoComplete | RenderFrameWorldStereo |
        RenderFrameDrawDuplicated | RenderFrameEffectivePoseValid |
        RenderFrameDirectGpuTransport;
    frame.failureReason = StereoFailureNone;
    frame.backbufferWidth = 3440;
    frame.backbufferHeight = 1440;
    frame.presentQpc = 0x0102030405060708LL;
    frame.reserved[RenderFrameDirectLeftHandleIndex] = 0x11112222u;
    frame.reserved[RenderFrameDirectRightHandleIndex] = 0x33334444u;
    frame.reserved[RenderFrameDirectWidthIndex] = 2016;
    frame.reserved[RenderFrameDirectHeightIndex] = 2208;
    frame.reserved[RenderFrameDirectFormatIndex] = 21;
    frame.reserved[RenderFrameDirectGenerationIndex] = 15;
    frame.reserved[RenderFrameDirectSlotIndex] = 3;

    for (int eye = 0; eye < 2; ++eye)
    {
        frame.eye[eye].orientation[3] = 1.0f;
        frame.eye[eye].position[0] = eye == 0 ? -0.032f : 0.032f;
        frame.eye[eye].fov = pose.eyeFov[eye];
    }

    const FrameDescriptor descriptor = ShadowV2::FrameDescriptorFromV2(frame);
    if (descriptor.frameId != 77 || descriptor.renderPoseId != 42 ||
        descriptor.transportKind != static_cast<std::uint32_t>(Core::TransportKind::D3D9ExShared) ||
        descriptor.transportGeneration != 15 || descriptor.transportSlot != 3 ||
        descriptor.width != 2016 || descriptor.height != 2208 || descriptor.format != 21 ||
        descriptor.leftHandle != 0x11112222ULL || descriptor.rightHandle != 0x33334444ULL ||
        descriptor.presentQpc != frame.presentQpc || descriptor.flags != frame.flags)
    {
        std::cerr << "FrameDescriptor v2->v3 conversion failed\n";
        return 2;
    }

    const ClientState client = ShadowV2::ClientStateFromV2(pose, &frame);
    if (client.clientPid != 5678 || client.flags != pose.reserved[ClientFlagsIndex] ||
        client.presentationMode != PresentationGameplay || client.stereoState != StereoSbsActive ||
        client.interopProbeHandle != 0x12345678ULL || client.interopProbeToken != 91 ||
        client.backbufferWidth != 3440 || client.backbufferHeight != 1440 ||
        client.lastPresentedFrameId != 77 || client.lastRenderedPoseId != 42 ||
        client.lastFailure != StereoFailureNone)
    {
        std::cerr << "ClientState v2->v3 conversion failed\n";
        return 3;
    }

    const AckState ack = ShadowV2::AckStateFromV2(pose, &frame, 9999);
    if (ack.consumerPid != 9999 || ack.acceptedProbeToken != 91 ||
        ack.consumedFrameId != 77 || ack.transportGeneration != 15 || ack.consumedSlot != 3)
    {
        std::cerr << "AckState v2->v3 conversion failed\n";
        return 4;
    }

    frame.flags &= ~RenderFrameDirectGpuTransport;
    const FrameDescriptor fallback = ShadowV2::FrameDescriptorFromV2(frame);
    if (fallback.transportKind != static_cast<std::uint32_t>(Core::TransportKind::DesktopDuplication) ||
        fallback.leftHandle != 0 || fallback.rightHandle != 0 ||
        fallback.width != 3440 || fallback.height != 1440)
    {
        std::cerr << "Desktop Duplication fallback conversion failed\n";
        return 5;
    }

    std::cout << "v2->v3 host/client/frame/ack conversion smoke passed.\n";
    return 0;
}
