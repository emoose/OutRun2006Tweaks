#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

#include <spdlog/spdlog.h>

#include "hook_mgr.hpp"
#include "vr/ipc/host_state_v3_reader.hpp"
#include "vr/ipc/protocol.hpp"
#include "vr/ipc/protocol_v3.hpp"
#include "vr/ipc/shadow_legacy_v2.hpp"
#include "vr/ipc/win32_channel.hpp"
#include "vr/ipc/shadow_lifetime_bridge.hpp"

namespace OutRunVR::IpcV3
{
    void RequestShadowBridgeStop() noexcept;

    namespace
    {
        constexpr DWORD ShadowIdlePollMs = 8;
        constexpr DWORD ShadowRetryMs = 250;

        enum class ProcessLiveness : std::uint8_t
        {
            Dead,
            Alive,
            Unknown
        };

        ProcessLiveness QueryProcessLiveness(DWORD pid) noexcept
        {
            if (!pid)
                return ProcessLiveness::Dead;
            HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process)
            {
                return GetLastError() == ERROR_INVALID_PARAMETER
                    ? ProcessLiveness::Dead
                    : ProcessLiveness::Unknown;
            }
            const DWORD wait = WaitForSingleObject(process, 0);
            CloseHandle(process);
            if (wait == WAIT_TIMEOUT)
                return ProcessLiveness::Alive;
            if (wait == WAIT_OBJECT_0)
                return ProcessLiveness::Dead;
            return ProcessLiveness::Unknown;
        }

        std::atomic<bool> ShadowBridgeStop{false};
        HANDLE ShadowBridgeStopEvent = nullptr;
        HANDLE ShadowBridgeThreadHandle = nullptr;

        bool ShadowBridgeWait(DWORD timeoutMs) noexcept
        {
            if (ShadowBridgeStop.load(std::memory_order_acquire))
                return true;
            if (!ShadowBridgeStopEvent)
            {
                Sleep(timeoutMs);
                return ShadowBridgeStop.load(std::memory_order_acquire);
            }
            return WaitForSingleObject(ShadowBridgeStopEvent, timeoutMs) == WAIT_OBJECT_0;
        }

        class ClientStateWriter
        {
        public:
            ClientStateWriter()
            {
                mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                    static_cast<DWORD>(sizeof(ClientState)), ClientStateName);
                if (!mapping_)
                    return;
                const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
                state_ = static_cast<ClientState*>(MapViewOfFile(
                    mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ClientState)));
                if (!state_)
                {
                    Reset();
                    return;
                }
                if (!existed)
                {
                    std::memset(state_, 0, sizeof(*state_));
                    state_->version = ProtocolVersion;
                    state_->structSize = sizeof(*state_);
                    MemoryBarrier();
                    state_->magic = ClientMagic;
                }
                else
                {
                    for (int i = 0; i < 100 && !HeaderValid(); ++i)
                        Sleep(2);
                    if (!HeaderValid())
                    {
                        Reset();
                        return;
                    }
                }
                owns_ = AcquireOwner(state_->clientPid);
            }

            ~ClientStateWriter()
            {
                if (state_ && owns_ && state_->clientPid == GetCurrentProcessId())
                {
                    const std::uint32_t odd = Ipc::BeginSeqlockWrite(state_->sequence);
                    state_->sequence = odd;
                    state_->flags = 0;
                    state_->clientPid = 0;
                    Ipc::EndSeqlockWrite(state_->sequence);
                }
                Reset();
            }

            bool Ready() const noexcept { return state_ && owns_; }

            std::uint32_t Publish(ClientState next) noexcept
            {
                if (!Ready())
                    return 0;
                next.magic = ClientMagic;
                next.version = ProtocolVersion;
                next.structSize = sizeof(ClientState);
                next.clientPid = GetCurrentProcessId();
                const std::uint32_t odd = Ipc::BeginSeqlockWrite(state_->sequence);
                next.sequence = odd;
                std::memcpy(state_, &next, sizeof(next));
                MemoryBarrier();
                return Ipc::EndSeqlockWrite(state_->sequence);
            }

        private:
            bool HeaderValid() const noexcept
            {
                return state_ && state_->magic == ClientMagic &&
                    state_->version == ProtocolVersion &&
                    state_->structSize == sizeof(ClientState);
            }

            bool AcquireOwner(volatile std::uint32_t& pid) noexcept
            {
                const LONG self = static_cast<LONG>(GetCurrentProcessId());
                for (int i = 0; i < 100; ++i)
                {
                    const LONG observed = static_cast<LONG>(pid);
                    if (observed == self)
                        return true;
                    if (observed != 0 &&
                        QueryProcessLiveness(static_cast<DWORD>(observed)) != ProcessLiveness::Dead)
                        return false;
                    if (InterlockedCompareExchange(
                        reinterpret_cast<volatile LONG*>(&pid), self, observed) == observed)
                        return true;
                    Sleep(1);
                }
                return false;
            }

            void Reset() noexcept
            {
                if (state_)
                {
                    UnmapViewOfFile(state_);
                    state_ = nullptr;
                }
                if (mapping_)
                {
                    CloseHandle(mapping_);
                    mapping_ = nullptr;
                }
                owns_ = false;
            }

            HANDLE mapping_ = nullptr;
            ClientState* state_ = nullptr;
            bool owns_ = false;
        };

        class FrameRingWriter
        {
        public:
            FrameRingWriter()
            {
                mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                    static_cast<DWORD>(sizeof(FrameRing)), FrameRingName);
                if (!mapping_)
                    return;
                const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
                ring_ = static_cast<FrameRing*>(MapViewOfFile(
                    mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FrameRing)));
                if (!ring_)
                {
                    Reset();
                    return;
                }
                if (!existed)
                {
                    std::memset(ring_, 0, sizeof(*ring_));
                    ring_->version = ProtocolVersion;
                    ring_->structSize = sizeof(*ring_);
                    ring_->slotCount = RingSize;
                    ring_->generation = 1;
                    MemoryBarrier();
                    ring_->magic = FrameMagic;
                }
                else
                {
                    for (int i = 0; i < 100 && !HeaderValid(); ++i)
                        Sleep(2);
                    if (!HeaderValid())
                    {
                        Reset();
                        return;
                    }
                }
                owns_ = AcquireOwner(ring_->producerPid);
            }

            ~FrameRingWriter()
            {
                if (ring_ && owns_ && ring_->producerPid == GetCurrentProcessId())
                {
                    const std::uint32_t odd = Ipc::BeginSeqlockWrite(ring_->publishSequence);
                    ring_->publishSequence = odd;
                    ring_->producerPid = 0;
                    Ipc::EndSeqlockWrite(ring_->publishSequence);
                }
                Reset();
            }

            bool Ready() const noexcept { return ring_ && owns_; }

            std::uint32_t Publish(const FrameDescriptor& frame) noexcept
            {
                if (!Ready() || frame.frameId == 0)
                    return 0;
                const std::uint32_t slot = static_cast<std::uint32_t>(frame.frameId % RingSize);
                const std::uint32_t odd = Ipc::BeginSeqlockWrite(ring_->publishSequence);
                std::memcpy(&ring_->slots[slot], &frame, sizeof(frame));
                ring_->latestSlot = slot;
                ring_->producerPid = GetCurrentProcessId();
                if (frame.transportGeneration != 0)
                    ring_->generation = frame.transportGeneration;
                else if (ring_->generation == 0)
                    ring_->generation = 1;
                MemoryBarrier();
                return Ipc::EndSeqlockWrite(ring_->publishSequence);
            }

        private:
            bool HeaderValid() const noexcept
            {
                return ring_ && ring_->magic == FrameMagic &&
                    ring_->version == ProtocolVersion &&
                    ring_->structSize == sizeof(FrameRing) && ring_->slotCount == RingSize;
            }

            bool AcquireOwner(std::uint32_t& pid) noexcept
            {
                const LONG self = static_cast<LONG>(GetCurrentProcessId());
                auto* atomicPid = reinterpret_cast<volatile LONG*>(&pid);
                for (int i = 0; i < 100; ++i)
                {
                    const LONG observed = *atomicPid;
                    if (observed == self)
                        return true;
                    if (observed != 0 &&
                        QueryProcessLiveness(static_cast<DWORD>(observed)) != ProcessLiveness::Dead)
                        return false;
                    if (InterlockedCompareExchange(atomicPid, self, observed) == observed)
                        return true;
                    Sleep(1);
                }
                return false;
            }

            void Reset() noexcept
            {
                if (ring_)
                {
                    UnmapViewOfFile(ring_);
                    ring_ = nullptr;
                }
                if (mapping_)
                {
                    CloseHandle(mapping_);
                    mapping_ = nullptr;
                }
                owns_ = false;
            }

            HANDLE mapping_ = nullptr;
            FrameRing* ring_ = nullptr;
            bool owns_ = false;
        };

        bool ReadAck(Ipc::ReadOnlyMapping<AckState>& mapping, AckState& out) noexcept
        {
            if (!mapping.EnsureOpen(AckStateName) || !Ipc::StableRead(mapping.Get(), out))
                return false;
            return out.magic == AckMagic && out.version == ProtocolVersion &&
                out.structSize == sizeof(AckState);
        }

        float MaxAbs(const float* a, const float* b, std::size_t count) noexcept
        {
            float result = 0.0f;
            for (std::size_t i = 0; i < count; ++i)
                result = (std::max)(result, std::fabs(a[i] - b[i]));
            return result;
        }

        float HostStateDelta(const HostState& expected, const HostState& actual) noexcept
        {
            float delta = 0.0f;
            delta = (std::max)(delta, MaxAbs(expected.headOrientation, actual.headOrientation, 4));
            delta = (std::max)(delta, MaxAbs(expected.headPositionMeters, actual.headPositionMeters, 3));
            for (int eye = 0; eye < 2; ++eye)
            {
                delta = (std::max)(delta,
                    MaxAbs(expected.eyes[eye].orientation, actual.eyes[eye].orientation, 4));
                delta = (std::max)(delta,
                    MaxAbs(expected.eyes[eye].positionMeters, actual.eyes[eye].positionMeters, 3));
                const float ef[4]{
                    expected.eyes[eye].fov.angleLeft, expected.eyes[eye].fov.angleRight,
                    expected.eyes[eye].fov.angleUp, expected.eyes[eye].fov.angleDown
                };
                const float af[4]{
                    actual.eyes[eye].fov.angleLeft, actual.eyes[eye].fov.angleRight,
                    actual.eyes[eye].fov.angleUp, actual.eyes[eye].fov.angleDown
                };
                delta = (std::max)(delta, MaxAbs(ef, af, 4));
            }
            return delta;
        }

        const char* FailureName(std::uint32_t reason) noexcept
        {
            switch (reason)
            {
            case StereoFailureNone: return "none";
            case StereoFailureMissingLatchedPose: return "missing_latched_pose";
            case StereoFailureResourceUnavailable: return "resource_unavailable";
            case StereoFailureMrtActive: return "mrt_active";
            case StereoFailureViewportUnavailable: return "viewport_unavailable";
            case StereoFailureLeftWvpUploadFailed: return "left_wvp_upload";
            case StereoFailureLeftDrawFailed: return "left_draw";
            case StereoFailureRightStateFailed: return "right_state";
            case StereoFailureRightWvpUploadFailed: return "right_wvp_upload";
            case StereoFailureRightDrawFailed: return "right_draw";
            case StereoFailureRestoreFailed: return "restore";
            case StereoFailureComposeFailed: return "compose";
            case StereoFailurePresentFailed: return "present";
            case StereoFailureDepthStateChanged: return "depth_state_changed";
            case StereoFailurePoseSequenceMismatch: return "pose_sequence_mismatch";
            case StereoFailureDepthUnsynchronized: return "depth_unsynchronized";
            case StereoFailureClearFailed: return "clear";
            case StereoFailureWorldClassificationFailed: return "world_classification";
            case StereoFailureStencilUnsynchronized: return "stencil_unsynchronized";
            case StereoFailureOffscreenWorld: return "offscreen_world";
            case StereoFailureOcclusionQueryActive: return "occlusion_query";
            default: return "unknown";
            }
        }

        DWORD WINAPI ShadowBridgeThread(void*)
        {
            Ipc::ReadOnlyMapping<SharedPoseState> legacyPoseMapping;
            Ipc::ReadOnlyMapping<SharedRenderFrameRing> legacyFrameMapping;
            Ipc::ReadOnlyMapping<AckState> ackMapping;
            HostStateReader hostReader;
            std::unique_ptr<ClientStateWriter> clientWriter;
            std::unique_ptr<FrameRingWriter> frameWriter;

            std::uint32_t lastLegacyPoseSequence = 0;
            std::uint32_t lastLegacyHeartbeat = 0;
            std::uint64_t lastLegacyFrameId = 0;
            std::uint64_t lastClientPublishedFrameId = 0;
            std::uint32_t lastClientPresentation = UINT32_MAX;
            std::uint32_t lastClientStereoState = UINT32_MAX;
            std::uint32_t lastFailureReason = StereoFailureNone;
            ULONGLONG nextWriterRetryMs = 0;
            ULONGLONG lastSummaryMs = 0;
            std::uint64_t poseParityOk = 0;
            std::uint64_t poseParityLag = 0;
            std::uint64_t poseParityMismatch = 0;
            std::uint64_t hostMissing = 0;
            std::uint64_t clientPublishes = 0;
            std::uint64_t framePublishes = 0;
            std::uint64_t ackParityOk = 0;
            std::uint64_t ackParityLag = 0;
            bool readyLogged = false;

            while (!ShadowBridgeStop.load(std::memory_order_acquire))
            {
                if (!legacyPoseMapping.EnsureOpen(SharedMemoryName))
                {
                    if (ShadowBridgeWait(100))
                        break;
                    continue;
                }

                SharedPoseState pose{};
                if (!ShadowV2::StableReadPose(legacyPoseMapping.Get(), pose))
                {
                    if (ShadowBridgeWait(ShadowIdlePollMs))
                        break;
                    continue;
                }

                const ULONGLONG loopNow = GetTickCount64();
                if ((!clientWriter || !clientWriter->Ready() ||
                     !frameWriter || !frameWriter->Ready()) && loopNow >= nextWriterRetryMs)
                {
                    if (clientWriter && !clientWriter->Ready())
                        clientWriter.reset();
                    if (frameWriter && !frameWriter->Ready())
                        frameWriter.reset();
                    if (!clientWriter)
                        clientWriter = std::make_unique<ClientStateWriter>();
                    if (!frameWriter)
                        frameWriter = std::make_unique<FrameRingWriter>();
                    nextWriterRetryMs = loopNow + ShadowRetryMs;
                }

                SharedRenderFrameState latestFrame{};
                bool haveLatestFrame = false;
                if (legacyFrameMapping.EnsureOpen(RenderFrameMemoryName))
                {
                    SharedRenderFrameRing ring{};
                    if (ShadowV2::StableReadFrameRing(legacyFrameMapping.Get(), ring))
                        haveLatestFrame = ShadowV2::LatestFrame(ring, latestFrame);
                }

                const bool frameChanged = haveLatestFrame &&
                    latestFrame.frameId != lastLegacyFrameId;
                if (frameChanged)
                {
                    lastLegacyFrameId = latestFrame.frameId;
                    if (frameWriter && frameWriter->Ready())
                    {
                        frameWriter->Publish(ShadowV2::FrameDescriptorFromV2(latestFrame));
                        ++framePublishes;
                    }
                    if (latestFrame.failureReason != StereoFailureNone &&
                        latestFrame.failureReason != lastFailureReason)
                    {
                        spdlog::warn(
                            "VR v3 shadow: stereo failure={} ({}) frame={} poseSeq={} flags=0x{:X} size={}x{} direct={}",
                            latestFrame.failureReason, FailureName(latestFrame.failureReason),
                            latestFrame.frameId, latestFrame.sourcePoseSequence, latestFrame.flags,
                            latestFrame.backbufferWidth, latestFrame.backbufferHeight,
                            (latestFrame.flags & RenderFrameDirectGpuTransport) ? 1 : 0);
                    }
                    lastFailureReason = latestFrame.failureReason;
                }

                const std::uint32_t heartbeat = pose.reserved[ClientHeartbeatIndex];
                const std::uint32_t presentation = pose.reserved[ClientPresentationModeIndex];
                const std::uint32_t stereoState = pose.reserved[ClientStereoStateIndex];
                const std::uint64_t currentFrameId = haveLatestFrame ? latestFrame.frameId : 0;
                const bool clientChanged = heartbeat != lastLegacyHeartbeat ||
                    currentFrameId != lastClientPublishedFrameId ||
                    presentation != lastClientPresentation ||
                    stereoState != lastClientStereoState;
                if (clientChanged && clientWriter && clientWriter->Ready())
                {
                    ClientState client = ShadowV2::ClientStateFromV2(
                        pose, haveLatestFrame ? &latestFrame : nullptr);
                    clientWriter->Publish(client);
                    ++clientPublishes;
                    lastLegacyHeartbeat = heartbeat;
                    lastClientPublishedFrameId = currentFrameId;
                    lastClientPresentation = presentation;
                    lastClientStereoState = stereoState;
                }

                const std::uint32_t directGeneration = haveLatestFrame &&
                    (latestFrame.flags & RenderFrameDirectGpuTransport)
                    ? latestFrame.reserved[RenderFrameDirectGenerationIndex] : 0;

                if (pose.sequence != lastLegacyPoseSequence)
                {
                    HostState v3{};
                    if (hostReader.Read(v3))
                    {
                        if (v3.poseId == pose.sequence)
                        {
                            const HostState expected = ShadowV2::HostStateFromV2(pose, directGeneration);
                            const bool metadataMatch =
                                v3.hostPid == expected.hostPid &&
                                v3.flags == expected.flags &&
                                v3.referenceSpaceGeneration == expected.referenceSpaceGeneration &&
                                v3.adapterLuidLow == expected.adapterLuidLow &&
                                v3.adapterLuidHigh == expected.adapterLuidHigh &&
                                v3.recommendedWidth[0] == expected.recommendedWidth[0] &&
                                v3.recommendedWidth[1] == expected.recommendedWidth[1] &&
                                v3.recommendedHeight[0] == expected.recommendedHeight[0] &&
                                v3.recommendedHeight[1] == expected.recommendedHeight[1];
                            const float maxDelta = HostStateDelta(expected, v3);
                            if (metadataMatch && maxDelta <= 1.0e-5f)
                                ++poseParityOk;
                            else
                            {
                                ++poseParityMismatch;
                                if (poseParityMismatch <= 4 || poseParityMismatch % 300 == 0)
                                {
                                    spdlog::warn(
                                        "VR v3 shadow parity mismatch: pose={} metadata={} maxDelta={:.8f} v2flags=0x{:X} v3flags=0x{:X} v2ref={} v3ref={}",
                                        pose.sequence, metadataMatch ? 1 : 0, maxDelta,
                                        ShadowV2::HostFlagsFromV2(pose.flags), v3.flags,
                                        pose.reserved[HostReferenceSpaceGenerationIndex],
                                        v3.referenceSpaceGeneration);
                                }
                            }
                        }
                        else
                        {
                            ++poseParityLag;
                        }
                    }
                    else
                    {
                        ++hostMissing;
                    }
                    lastLegacyPoseSequence = pose.sequence;
                }

                AckState ack{};
                if (ReadAck(ackMapping, ack))
                {
                    const AckState expected = ShadowV2::AckStateFromV2(
                        pose, haveLatestFrame ? &latestFrame : nullptr, ack.consumerPid);
                    if (ack.acceptedProbeToken == expected.acceptedProbeToken &&
                        ack.consumedFrameId == expected.consumedFrameId)
                        ++ackParityOk;
                    else
                        ++ackParityLag;
                }

                if (!readyLogged)
                {
                    readyLogged = true;
                    spdlog::info(
                        "VR v3 shadow: live dual-protocol diagnostics active; change-driven publication + bounded polling; v2 remains render authority until parity is proven");
                }

                const ULONGLONG now = GetTickCount64();
                if (now - lastSummaryMs >= 5000)
                {
                    lastSummaryMs = now;
                    const bool direct = haveLatestFrame &&
                        (latestFrame.flags & RenderFrameDirectGpuTransport) != 0;
                    spdlog::info(
                        "VR v3 shadow summary: poseOk={} poseLag={} poseMismatch={} hostMissing={} clientPub={} framePub={} ackOk={} ackLag={} latestPose={} latestFrame={} stereoFailure={}({}) direct={} directReady={} presentation={} stereoState={}",
                        poseParityOk, poseParityLag, poseParityMismatch, hostMissing,
                        clientPublishes, framePublishes, ackParityOk, ackParityLag,
                        pose.sequence, haveLatestFrame ? latestFrame.frameId : 0,
                        haveLatestFrame ? latestFrame.failureReason : 0,
                        haveLatestFrame ? FailureName(latestFrame.failureReason) : "none",
                        direct ? 1 : 0,
                        (pose.flags & HostDirectGpuReady) ? 1 : 0,
                        pose.reserved[ClientPresentationModeIndex],
                        pose.reserved[ClientStereoStateIndex]);
                }

                if (ShadowBridgeWait(ShadowIdlePollMs))
                    break;
            }
            return 0;
        }

        class VRV3ShadowHook final : public Hook
        {
        public:
            std::string_view description() override { return "OpenXRVRV3ShadowBridge"; }
            bool validate() override { return true; }

            bool apply() override
            {
                if (ShadowBridgeThreadHandle)
                    return true;
                if (!ShadowBridgeStopEvent)
                    ShadowBridgeStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!ShadowBridgeStopEvent)
                {
                    spdlog::error("VR v3 shadow: failed to create stop event: {}", GetLastError());
                    return false;
                }
                ShadowBridgeStop.store(false, std::memory_order_release);
                ResetEvent(ShadowBridgeStopEvent);

                HMODULE pinnedModule = nullptr;
                if (!GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                        reinterpret_cast<LPCWSTR>(&ShadowBridgeStop), &pinnedModule))
                {
                    spdlog::error("VR v3 shadow: failed to pin plugin module: {}", GetLastError());
                    return false;
                }

                HANDLE thread = CreateThread(nullptr, 0, ShadowBridgeThread, nullptr, 0, nullptr);
                if (!thread)
                {
                    spdlog::error("VR v3 shadow: failed to create bridge thread: {}", GetLastError());
                    return false;
                }
                ShadowBridgeThreadHandle = thread;
                RegisterShadowBridgeStopCallback(&RequestShadowBridgeStop);
                spdlog::info("VR v3 shadow: process-lifetime worker tracked with stop event; plugin module pinned; detach callback registered");
                return true;
            }

            static VRV3ShadowHook instance;
        };

        VRV3ShadowHook VRV3ShadowHook::instance;
    }

    void RequestShadowBridgeStop() noexcept
    {
        RegisterShadowBridgeStopCallback(nullptr);
        ShadowBridgeStop.store(true, std::memory_order_release);
        if (ShadowBridgeStopEvent)
            SetEvent(ShadowBridgeStopEvent);
        if (ShadowBridgeThreadHandle)
        {
            CloseHandle(ShadowBridgeThreadHandle);
            ShadowBridgeThreadHandle = nullptr;
        }
    }
}
