#pragma once

#include <cstddef>
#include <cstdint>

namespace OutRunVR
{
    inline constexpr wchar_t SharedMemoryName[] = L"Local\\OutRun2006Tweaks.VR.Pose.v2";
    inline constexpr std::uint32_t SharedMagic = 0x5256524Fu; // 'ORVR' in little endian
    inline constexpr std::uint32_t SharedProtocolVersion = 2;

    inline constexpr wchar_t RenderFrameMemoryName[] = L"Local\\OutRun2006Tweaks.VR.Frame.v2";
    inline constexpr std::uint32_t RenderFrameMagic = 0x4656524Fu; // 'ORVF'
    inline constexpr std::uint32_t RenderFrameProtocolVersion = 2;
    inline constexpr std::uint32_t RenderFrameRingSize = 4;
    inline constexpr std::size_t PackedEyeOrientationOffset = 48;
    inline constexpr std::size_t PackedEyeOrientationBytes = 16;

    enum SharedFlags : std::uint32_t
    {
        HostAlive = 1u << 0,
        OrientationValid = 1u << 1,
        PositionValid = 1u << 2,
        SessionVisible = 1u << 3,
        SessionFocused = 1u << 4,
        StereoViewsValid = 1u << 5,
        StereoEyeOrientationValid = 1u << 6,
        HostShouldRender = 1u << 7,
        HostDirectGpuTransport = 1u << 8,
        HostDirectGpuReady = 1u << 9,
        HostAdapterLuidValid = 1u << 10,
    };

    inline constexpr std::uint32_t ClientHeartbeatIndex = 0;
    inline constexpr std::uint32_t ClientFlagsIndex = 1;
    inline constexpr std::uint32_t ClientLastAngleBitsIndex = 2;
    inline constexpr std::uint32_t HostReferenceSpaceGenerationIndex = 3;
    inline constexpr std::uint32_t ClientPresentationModeIndex = 4;
    inline constexpr std::uint32_t ClientGameStateIndex = 5;
    inline constexpr std::uint32_t HostEyeOffsetLeftXIndex = 6;
    inline constexpr std::uint32_t HostEyeOffsetLeftYIndex = 7;
    inline constexpr std::uint32_t HostEyeOffsetLeftZIndex = 8;
    inline constexpr std::uint32_t HostEyeOffsetRightXIndex = 9;
    inline constexpr std::uint32_t HostEyeOffsetRightYIndex = 10;
    inline constexpr std::uint32_t HostEyeOffsetRightZIndex = 11;
    inline constexpr std::uint32_t ClientStereoStateIndex = 12;
    inline constexpr std::uint32_t ClientStereoFrameIndex = 13;
    inline constexpr std::uint32_t ClientStereoBackbufferWidthIndex = 14;
    inline constexpr std::uint32_t ClientStereoBackbufferHeightIndex = 15;
    // Source-compatibility alias only. Slot 14 remains backbuffer width;
    // exact presentation timing is transported by Frame.v1::presentQpc.
    inline constexpr std::uint32_t ClientStereoPresentQpcLowIndex = ClientStereoBackbufferWidthIndex;

    enum ClientPresentationMode : std::uint32_t
    {
        PresentationUnknown = 0,
        PresentationGameplay = 1,
        PresentationTheater = 2,
    };

    enum ClientStereoState : std::uint32_t
    {
        StereoDisabled = 0,
        StereoSbsActive = 1,
        StereoSbsFallbackMono = 2,
    };

    enum ClientTelemetryFlags : std::uint32_t
    {
        ClientHookAlive = 1u << 0,
        ClientHostPoseValid = 1u << 1,
        ClientPoseApplied = 1u << 2,
        ClientAutoEnabled = 1u << 3,
        ClientRendererWvpVerified = 1u << 4,
        ClientRendererPoseInjected = 1u << 5,
        ClientRendererMatrixPrepared = 1u << 6,
        ClientRendererUploadFailed = 1u << 7,
        ClientCullingCameraSynced = 1u << 8,
        ClientFrameCompleted = 1u << 9,
        ClientStereoActive = 1u << 10,
        ClientStereoWorldDraw = 1u << 11,
        ClientStereoDrawDuplicated = 1u << 12,
    };

    enum RenderFrameFlags : std::uint32_t
    {
        RenderFrameStereoComplete = 1u << 0,
        RenderFrameWorldStereo = 1u << 1,
        RenderFrameDrawDuplicated = 1u << 2,
        RenderFrameEffectivePoseValid = 1u << 3,
        RenderFramePresentInFlight = 1u << 4,
        RenderFrameDirectGpuTransport = 1u << 5,
    };

    // Frame.v1 reserved-word extension. The 256-byte ABI stays unchanged.
    inline constexpr std::uint32_t RenderFrameDirectLeftHandleIndex = 0;
    inline constexpr std::uint32_t RenderFrameDirectRightHandleIndex = 1;
    inline constexpr std::uint32_t RenderFrameDirectWidthIndex = 2;
    inline constexpr std::uint32_t RenderFrameDirectHeightIndex = 3;
    inline constexpr std::uint32_t RenderFrameDirectFormatIndex = 4;
    inline constexpr std::uint32_t RenderFrameDirectGenerationIndex = 5;
    inline constexpr std::uint32_t RenderFrameDirectSlotIndex = 6;
    // Three reserved words carry the producer DLL's 12-character git tag as
    // raw ASCII. This keeps the Frame.v2 ABI fixed while making mixed binary
    // test sessions obvious in host logs.
    inline constexpr std::uint32_t RenderFrameGameBuildTag0Index = 7;
    inline constexpr std::uint32_t RenderFrameGameBuildTag1Index = 8;
    inline constexpr std::uint32_t RenderFrameGameBuildTag2Index = 9;
    // R35 cadence identity. The game stamps the XR pacing request that released
    // this game frame. Word 10 was previously unused; Frame.v2 ABI stays fixed.
    inline constexpr std::uint32_t RenderFrameCadenceRequestIndex = 10;

    // P0 runtime identity. Word 11 carries a per-game-process generation that
    // is mirrored by SharedRenderFrameRing::reserved0. Reusing reserved storage
    // keeps the Frame.v2 ABI unchanged while letting every reader reject stale
    // slots left behind when the host mapping survives a fast game restart.
    inline constexpr std::uint32_t RenderFrameRunGenerationIndex = 11;

    enum StereoFailureReason : std::uint32_t
    {
        StereoFailureNone = 0,
        StereoFailureMissingLatchedPose = 1,
        StereoFailureResourceUnavailable = 2,
        StereoFailureMrtActive = 3,
        StereoFailureViewportUnavailable = 4,
        StereoFailureLeftWvpUploadFailed = 5,
        StereoFailureLeftDrawFailed = 6,
        StereoFailureRightStateFailed = 7,
        StereoFailureRightWvpUploadFailed = 8,
        StereoFailureRightDrawFailed = 9,
        StereoFailureRestoreFailed = 10,
        StereoFailureComposeFailed = 11,
        StereoFailurePresentFailed = 12,
        StereoFailureDepthStateChanged = 13,
        StereoFailurePoseSequenceMismatch = 14,
        StereoFailureDepthUnsynchronized = 15,
        StereoFailureClearFailed = 16,
        StereoFailureWorldClassificationFailed = 17,
        StereoFailureStencilUnsynchronized = 18,
        StereoFailureOffscreenWorld = 19,
        StereoFailureOcclusionQueryActive = 20,
    };

#pragma pack(push, 4)
    struct SharedFov
    {
        float angleLeft;
        float angleRight;
        float angleUp;
        float angleDown;
    };

    struct SharedPoseState
    {
        std::uint32_t magic;
        std::uint32_t protocolVersion;
        std::uint32_t structSize;
        volatile std::uint32_t sequence;
        volatile std::uint32_t hostPid;
        volatile std::uint32_t clientPid;
        volatile std::uint32_t flags;
        volatile std::uint32_t heartbeat;
        std::int64_t sampleQpc;
        float orientation[4];
        float position[3];
        volatile std::uint32_t clientStereoPoseSequence;
        SharedFov eyeFov[2];
        std::uint32_t recommendedWidth[2];
        std::uint32_t recommendedHeight[2];
        std::uint32_t hostAdapterLuidLow;
        std::uint32_t hostAdapterLuidHigh;
        volatile std::uint32_t clientAdapterLuidLow;
        volatile std::uint32_t clientAdapterLuidHigh;
        volatile std::uint32_t clientInteropProbeHandle;
        volatile std::uint32_t clientInteropProbeToken;
        volatile std::uint32_t hostInteropProbeAckToken;
        volatile std::uint32_t hostDirectConsumedFrameId;
        char runtimeName[64];
        std::uint32_t reserved[16];
    };

    struct SharedRenderEye
    {
        float orientation[4];
        float position[3];
        float reserved0;
        SharedFov fov;
    };

    struct SharedRenderFrameState
    {
        std::uint32_t magic;
        std::uint32_t protocolVersion;
        std::uint32_t structSize;
        volatile std::uint32_t sequence;
        volatile std::uint32_t clientPid;
        std::uint32_t state;
        std::uint32_t frameId;
        std::uint32_t sourcePoseSequence;
        std::uint32_t presentationMode;
        std::uint32_t flags;
        std::uint32_t failureReason;
        std::uint32_t backbufferWidth;
        std::uint32_t backbufferHeight;
        std::int64_t presentQpc;
        SharedRenderEye eye[2];
        std::uint32_t reserved[25];
    };

    struct SharedRenderFrameRing
    {
        std::uint32_t magic;
        std::uint32_t protocolVersion;
        std::uint32_t structSize;
        std::uint32_t slotCount;
        volatile std::uint32_t publishSequence;
        volatile std::uint32_t latestSlot;
        volatile std::uint32_t clientPid;
        // P0: per-game-process run generation. Each valid slot mirrors this in
        // reserved[RenderFrameRunGenerationIndex].
        std::uint32_t reserved0;
        SharedRenderFrameState slots[RenderFrameRingSize];
    };
#pragma pack(pop)

    inline bool RenderFrameRunIdentityMatches(
        const SharedRenderFrameRing& ring,
        const SharedRenderFrameState& frame) noexcept
    {
        return ring.clientPid != 0 &&
            ring.reserved0 != 0 &&
            frame.clientPid == ring.clientPid &&
            frame.reserved[RenderFrameRunGenerationIndex] == ring.reserved0;
    }

    static_assert(sizeof(SharedFov) == 16);
    static_assert(sizeof(SharedPoseState) == 280);
    static_assert(offsetof(SharedPoseState, recommendedWidth) == 104);
    static_assert(offsetof(SharedPoseState, recommendedHeight) == 112);
    static_assert(offsetof(SharedPoseState, hostAdapterLuidLow) == 120);
    static_assert(offsetof(SharedPoseState, runtimeName) == 152);
    static_assert(offsetof(SharedPoseState, reserved) == 216);
    static_assert(PackedEyeOrientationOffset + PackedEyeOrientationBytes == 64);
    static_assert(sizeof(SharedRenderEye) == 48);
    static_assert(sizeof(SharedRenderFrameState) == 256);
    static_assert(sizeof(SharedRenderFrameRing) == 1056);
}

namespace OutRunVRRenderer
{
    using OutRunVR::SharedMemoryName;
    using OutRunVR::SharedMagic;
    using OutRunVR::SharedProtocolVersion;
    using OutRunVR::SharedPoseState;
    using OutRunVR::SharedFov;
    using OutRunVR::HostAlive;
    using OutRunVR::OrientationValid;
    using OutRunVR::PositionValid;
    using OutRunVR::StereoViewsValid;
    using OutRunVR::StereoEyeOrientationValid;
    using OutRunVR::SessionVisible;
    using OutRunVR::HostShouldRender;
    using OutRunVR::ClientHeartbeatIndex;
    using OutRunVR::ClientFlagsIndex;
    using OutRunVR::ClientLastAngleBitsIndex;
    using OutRunVR::HostReferenceSpaceGenerationIndex;
    using OutRunVR::ClientPresentationModeIndex;
    using OutRunVR::ClientGameStateIndex;
    using OutRunVR::HostEyeOffsetLeftXIndex;
    using OutRunVR::HostEyeOffsetLeftYIndex;
    using OutRunVR::HostEyeOffsetLeftZIndex;
    using OutRunVR::HostEyeOffsetRightXIndex;
    using OutRunVR::HostEyeOffsetRightYIndex;
    using OutRunVR::HostEyeOffsetRightZIndex;
    using OutRunVR::ClientStereoStateIndex;
    using OutRunVR::ClientStereoFrameIndex;
    using OutRunVR::ClientStereoPresentQpcLowIndex;
    using OutRunVR::ClientStereoBackbufferWidthIndex;
    using OutRunVR::ClientStereoBackbufferHeightIndex;
    using OutRunVR::ClientPresentationMode;
    using OutRunVR::PresentationUnknown;
    using OutRunVR::PresentationGameplay;
    using OutRunVR::PresentationTheater;
    using OutRunVR::StereoDisabled;
    using OutRunVR::StereoSbsActive;
    using OutRunVR::StereoSbsFallbackMono;
    using OutRunVR::ClientHookAlive;
    using OutRunVR::ClientHostPoseValid;
    using OutRunVR::ClientPoseApplied;
    using OutRunVR::ClientAutoEnabled;
    using OutRunVR::ClientRendererWvpVerified;
    using OutRunVR::ClientRendererPoseInjected;
    using OutRunVR::ClientRendererMatrixPrepared;
    using OutRunVR::ClientRendererUploadFailed;
    using OutRunVR::ClientCullingCameraSynced;
    using OutRunVR::ClientFrameCompleted;
    using OutRunVR::ClientStereoActive;
    using OutRunVR::ClientStereoWorldDraw;
    using OutRunVR::ClientStereoDrawDuplicated;

    struct LatchedStereoFrame
    {
        bool valid = false;
        std::uint32_t poseSequence = 0;
        SharedFov eyeFov[2]{};
        float eyeOffset[2][3]{};
        float eyeOrientation[2][4]{};
        float effectiveEyeOrientation[2][4]{};
        float effectiveEyePosition[2][3]{};
    };

    bool GetLatchedStereoFrame(LatchedStereoFrame& out);
    bool GetLatchedHeadInverse(float outMatrix[16],
        std::uint32_t& poseSequence) noexcept;
    bool GetRendererBaseProjection(float outMatrix[16]);
    bool GetLastVerifiedWvp(float outConstants[16], std::uint32_t& generation,
        std::uint32_t& poseSequence, std::uintptr_t& shaderIdentity,
        std::uint64_t& shaderSerial);
    bool GetLastGameWvpWrite(float outConstants[16], std::uint64_t& writeSerial,
        std::uint64_t& topLevelDrawSerial, std::uintptr_t& shaderIdentity,
        std::uint64_t& shaderSerial) noexcept;
    std::uint64_t GetBeginSceneCallCount();
    void NotifyGamePresent();
    std::uint32_t GetActiveCadenceRequestId() noexcept;
    bool IsCadencePacingActive() noexcept;
    void NotifyGameReset();
}

namespace OutRunVRStereo
{
    bool IsInternalStereoPassActive();
    bool IsGameStateBlockRecording() noexcept;
    bool IsStateBlockTrackingReliable() noexcept;
    bool GetCurrentShaderEpoch(std::uintptr_t& shaderIdentity, std::uint64_t& serial);
    std::uint64_t GetTopLevelDrawSerial() noexcept;
}
