#pragma once

#include <cstddef>
#include <cstdint>

namespace OutRunVR::CadenceV1
{
    inline constexpr std::uint32_t ProtocolVersion = 1;
    inline constexpr std::uint32_t HostMagic = 0x3148434Fu;   // OCH1
    inline constexpr std::uint32_t ClientMagic = 0x3143434Fu; // OCC1

    inline constexpr wchar_t HostStateName[] =
        L"Local\\OutRun2006Tweaks.VR.Cadence.v1.Host";
    inline constexpr wchar_t ClientStateName[] =
        L"Local\\OutRun2006Tweaks.VR.Cadence.v1.Client";
    inline constexpr wchar_t RequestEventName[] =
        L"Local\\OutRun2006Tweaks.VR.Cadence.v1.Request";
    inline constexpr wchar_t PresentedEventName[] =
        L"Local\\OutRun2006Tweaks.VR.Cadence.v1.Presented";

    enum HostFlags : std::uint32_t
    {
        HostEnabled = 1u << 0,
        HostRunning = 1u << 1,
        HostSerializedProbe = 1u << 2,
    };

    enum ClientFlags : std::uint32_t
    {
        ClientEnabled = 1u << 0,
        ClientWaiting = 1u << 1,
        ClientLastWaitTimedOut = 1u << 2,
    };

#pragma pack(push, 4)
    struct HostState
    {
        std::uint32_t magic{HostMagic};
        std::uint32_t version{ProtocolVersion};
        std::uint32_t structSize{sizeof(HostState)};
        volatile std::uint32_t sequence{};
        std::uint32_t hostPid{};
        std::uint32_t flags{};
        std::uint32_t requestId{};
        std::uint32_t poseSequence{};
        std::uint32_t targetHzMilli{};
        std::uint32_t timeoutMs{};
        std::int64_t requestQpc{};
        std::int64_t predictedDisplayTime{};
        std::int64_t predictedDisplayPeriod{};
    };

    struct ClientState
    {
        std::uint32_t magic{ClientMagic};
        std::uint32_t version{ProtocolVersion};
        std::uint32_t structSize{sizeof(ClientState)};
        volatile std::uint32_t sequence{};
        std::uint32_t clientPid{};
        std::uint32_t flags{};
        std::uint32_t acceptedRequestId{};
        std::uint32_t presentedRequestId{};
        std::uint32_t timeoutCount{};
        std::uint32_t lastWaitUs{};
        std::int64_t acceptedQpc{};
        std::int64_t presentedQpc{};
    };
#pragma pack(pop)

    static_assert(sizeof(HostState) == 64);
    static_assert(sizeof(ClientState) == 56);
    static_assert(offsetof(HostState, requestQpc) == 40);
    static_assert(offsetof(ClientState, acceptedQpc) == 40);
}
