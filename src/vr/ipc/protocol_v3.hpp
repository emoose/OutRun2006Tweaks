#pragma once

#include "vr/core/frame_types.hpp"
#include "vr/core/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace OutRunVR::IpcV3
{
    inline constexpr std::uint32_t ProtocolVersion = 3;
    inline constexpr std::uint32_t RingSize = 4;
    using WireHandle = std::uint64_t;

    inline constexpr wchar_t HostStateName[] = L"Local\\OutRun2006Tweaks.VR.v3.HostState";
    inline constexpr wchar_t ClientStateName[] = L"Local\\OutRun2006Tweaks.VR.v3.ClientState";
    inline constexpr wchar_t FrameRingName[] = L"Local\\OutRun2006Tweaks.VR.v3.FrameRing";
    inline constexpr wchar_t AckStateName[] = L"Local\\OutRun2006Tweaks.VR.v3.AckState";

    inline constexpr std::uint32_t HostMagic = 0x3348524Fu;   // ORH3
    inline constexpr std::uint32_t ClientMagic = 0x3343524Fu; // ORC3
    inline constexpr std::uint32_t FrameMagic = 0x3346524Fu;  // ORF3
    inline constexpr std::uint32_t AckMagic = 0x3341524Fu;    // ORA3

    enum HostFlags : std::uint32_t
    {
        HostAlive = 1u << 0,
        OrientationValid = 1u << 1,
        PositionValid = 1u << 2,
        SessionVisible = 1u << 3,
        SessionFocused = 1u << 4,
        StereoViewsValid = 1u << 5,
        HostShouldRender = 1u << 6,
        DirectGpuTransportSupported = 1u << 7,
        DirectGpuTransportReady = 1u << 8,
        AdapterLuidValid = 1u << 9,
    };

#pragma pack(push, 4)
    struct WireFov
    {
        float angleLeft{};
        float angleRight{};
        float angleUp{};
        float angleDown{};
    };

    struct WireEyeView
    {
        float orientation[4]{};
        float positionMeters[3]{};
        float reserved{};
        WireFov fov{};
    };

    struct HostState
    {
        std::uint32_t magic{HostMagic};
        std::uint32_t version{ProtocolVersion};
        std::uint32_t structSize{};
        volatile std::uint32_t sequence{};
        std::uint32_t hostPid{};
        std::uint32_t flags{};
        std::uint32_t referenceSpaceGeneration{};
        std::uint32_t directTransportGeneration{};
        std::uint64_t poseId{};
        std::int64_t predictedDisplayTime{};
        std::int64_t sampleQpc{};
        float headOrientation[4]{};
        float headPositionMeters[3]{};
        std::uint32_t recommendedWidth[2]{};
        std::uint32_t recommendedHeight[2]{};
        std::uint32_t adapterLuidLow{};
        std::int32_t adapterLuidHigh{};
        WireEyeView eyes[2]{};
        char runtimeName[64]{};
    };

    struct ClientState
    {
        std::uint32_t magic{ClientMagic};
        std::uint32_t version{ProtocolVersion};
        std::uint32_t structSize{};
        volatile std::uint32_t sequence{};
        std::uint32_t clientPid{};
        std::uint32_t flags{};
        std::uint32_t presentationMode{};
        std::uint32_t gameState{};
        std::uint32_t stereoState{};
        std::uint32_t lastFailure{};
        std::uint32_t adapterLuidLow{};
        std::int32_t adapterLuidHigh{};
        std::uint32_t interopProbeGeneration{};
        std::uint32_t interopProbeToken{};
        WireHandle interopProbeHandle{};
        std::uint32_t backbufferWidth{};
        std::uint32_t backbufferHeight{};
        std::uint64_t lastPresentedFrameId{};
        std::uint64_t lastRenderedPoseId{};
    };

    struct FrameDescriptor
    {
        std::uint64_t frameId{};
        std::uint64_t renderPoseId{};
        std::int64_t presentQpc{};
        std::uint32_t presentationMode{};
        std::uint32_t flags{};
        std::uint32_t failureReason{};
        std::uint32_t transportKind{};
        std::uint32_t transportGeneration{};
        std::uint32_t transportSlot{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t format{};
        WireHandle leftHandle{};
        WireHandle rightHandle{};
        WireEyeView renderedEyes[2]{};
    };

    struct FrameRing
    {
        std::uint32_t magic{FrameMagic};
        std::uint32_t version{ProtocolVersion};
        std::uint32_t structSize{};
        std::uint32_t slotCount{RingSize};
        volatile std::uint32_t publishSequence{};
        volatile std::uint32_t latestSlot{};
        std::uint32_t producerPid{};
        std::uint32_t generation{};
        FrameDescriptor slots[RingSize]{};
    };

    struct AckState
    {
        std::uint32_t magic{AckMagic};
        std::uint32_t version{ProtocolVersion};
        std::uint32_t structSize{};
        volatile std::uint32_t sequence{};
        std::uint32_t consumerPid{};
        std::uint32_t acceptedProbeGeneration{};
        std::uint32_t acceptedProbeToken{};
        std::uint32_t transportGeneration{};
        std::uint64_t consumedFrameId{};
        std::uint32_t consumedSlot{};
        std::uint32_t flags{};
    };
#pragma pack(pop)

    template <typename T>
    constexpr void InitializeWireState(T& state) noexcept
    {
        state.structSize = static_cast<std::uint32_t>(sizeof(T));
    }

    static_assert(sizeof(WireHandle) == 8);
    static_assert(sizeof(WireFov) == 16);
    static_assert(sizeof(WireEyeView) == 48);
    static_assert(sizeof(HostState) == 268);
    static_assert(sizeof(ClientState) == 88);
    static_assert(sizeof(FrameDescriptor) == 172);
    static_assert(sizeof(FrameRing) == 720);
    static_assert(sizeof(AckState) == 48);
    static_assert(std::is_standard_layout_v<HostState> && std::is_trivially_copyable_v<HostState>);
    static_assert(std::is_standard_layout_v<ClientState> && std::is_trivially_copyable_v<ClientState>);
    static_assert(std::is_standard_layout_v<FrameDescriptor> && std::is_trivially_copyable_v<FrameDescriptor>);
    static_assert(std::is_standard_layout_v<FrameRing> && std::is_trivially_copyable_v<FrameRing>);
    static_assert(std::is_standard_layout_v<AckState> && std::is_trivially_copyable_v<AckState>);
}
