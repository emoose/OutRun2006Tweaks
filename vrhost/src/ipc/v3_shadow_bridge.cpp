#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "ipc/host_state_v3_writer.hpp"
#include "vr/ipc/protocol.hpp"
#include "vr/ipc/protocol_v3.hpp"
#include "vr/ipc/shadow_legacy_v2.hpp"
#include "vr/ipc/win32_channel.hpp"

namespace OutRunVR::Host
{
    namespace
    {
        constexpr DWORD ShadowIdlePollMs = 8;
        constexpr DWORD ShadowWriterRetryMs = 250;

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
            HANDLE process = OpenProcess(
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
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

        class JsonLog
        {
        public:
            JsonLog()
            {
                stream_.open("outrun-vr-host-v3.log", std::ios::out | std::ios::trunc);
            }

            void Write(const char* level, const char* event,
                const std::string& message) noexcept
            {
                try
                {
                    if (!stream_.is_open())
                        return;
                    SYSTEMTIME now{};
                    GetSystemTime(&now);
                    stream_ << "{\"time\":\""
                        << now.wYear << '-' << now.wMonth << '-' << now.wDay << 'T'
                        << now.wHour << ':' << now.wMinute << ':' << now.wSecond << '.'
                        << now.wMilliseconds << "Z\",\"level\":\"" << Escape(level)
                        << "\",\"event\":\"" << Escape(event)
                        << "\",\"message\":\"" << Escape(message) << "\"}\n";
                    stream_.flush();
                }
                catch (...)
                {
                }
            }

        private:
            static std::string Escape(const std::string& value)
            {
                std::string out;
                out.reserve(value.size() + 8);
                for (const char c : value)
                {
                    switch (c)
                    {
                    case '\\': out += "\\\\"; break;
                    case '"': out += "\\\""; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default: out += c; break;
                    }
                }
                return out;
            }

            std::ofstream stream_;
        };

        class AckStateWriter
        {
        public:
            AckStateWriter()
            {
                mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                    PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(IpcV3::AckState)),
                    IpcV3::AckStateName);
                if (!mapping_)
                    return;
                const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
                state_ = static_cast<IpcV3::AckState*>(MapViewOfFile(
                    mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IpcV3::AckState)));
                if (!state_)
                {
                    Reset();
                    return;
                }
                if (!existed)
                {
                    std::memset(state_, 0, sizeof(*state_));
                    state_->version = IpcV3::ProtocolVersion;
                    state_->structSize = sizeof(*state_);
                    MemoryBarrier();
                    state_->magic = IpcV3::AckMagic;
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
                owns_ = AcquireOwner();
            }

            ~AckStateWriter()
            {
                if (state_ && owns_ &&
                    state_->consumerPid == GetCurrentProcessId())
                {
                    const std::uint32_t odd =
                        Ipc::BeginSeqlockWrite(state_->sequence);
                    state_->sequence = odd;
                    state_->flags = 0;
                    state_->consumerPid = 0;
                    Ipc::EndSeqlockWrite(state_->sequence);
                }
                Reset();
            }

            bool Ready() const noexcept { return state_ && owns_; }

            std::uint32_t Publish(IpcV3::AckState next) noexcept
            {
                if (!Ready())
                    return 0;
                next.magic = IpcV3::AckMagic;
                next.version = IpcV3::ProtocolVersion;
                next.structSize = sizeof(IpcV3::AckState);
                next.consumerPid = GetCurrentProcessId();
                const std::uint32_t odd =
                    Ipc::BeginSeqlockWrite(state_->sequence);
                next.sequence = odd;
                std::memcpy(state_, &next, sizeof(next));
                MemoryBarrier();
                return Ipc::EndSeqlockWrite(state_->sequence);
            }

        private:
            bool HeaderValid() const noexcept
            {
                return state_ && state_->magic == IpcV3::AckMagic &&
                    state_->version == IpcV3::ProtocolVersion &&
                    state_->structSize == sizeof(IpcV3::AckState);
            }

            bool AcquireOwner() noexcept
            {
                const LONG self = static_cast<LONG>(GetCurrentProcessId());
                auto* pid = reinterpret_cast<volatile LONG*>(&state_->consumerPid);
                for (int i = 0; i < 100; ++i)
                {
                    const LONG observed = *pid;
                    if (observed == self)
                        return true;
                    if (observed != 0 &&
                        QueryProcessLiveness(static_cast<DWORD>(observed)) !=
                            ProcessLiveness::Dead)
                        return false;
                    if (InterlockedCompareExchange(pid, self, observed) == observed)
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
            IpcV3::AckState* state_ = nullptr;
            bool owns_ = false;
        };

        bool ReadClient(Ipc::ReadOnlyMapping<IpcV3::ClientState>& mapping,
            IpcV3::ClientState& out) noexcept
        {
            if (!mapping.EnsureOpen(IpcV3::ClientStateName) ||
                !Ipc::StableRead(mapping.Get(), out))
                return false;
            return out.magic == IpcV3::ClientMagic &&
                out.version == IpcV3::ProtocolVersion &&
                out.structSize == sizeof(IpcV3::ClientState);
        }

        bool StableReadV3FrameRing(const IpcV3::FrameRing* shared,
            IpcV3::FrameRing& out, int attempts = 8) noexcept
        {
            if (!shared)
                return false;
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                const std::uint32_t before = shared->publishSequence;
                if (before & 1u)
                    continue;
                MemoryBarrier();
                std::memcpy(&out, shared, sizeof(out));
                MemoryBarrier();
                const std::uint32_t after = shared->publishSequence;
                if (before == after && !(after & 1u) &&
                    out.magic == IpcV3::FrameMagic &&
                    out.version == IpcV3::ProtocolVersion &&
                    out.structSize == sizeof(IpcV3::FrameRing) &&
                    out.slotCount == IpcV3::RingSize)
                    return true;
            }
            return false;
        }

        bool LatestV3Frame(const IpcV3::FrameRing& ring,
            IpcV3::FrameDescriptor& out) noexcept
        {
            if (ring.latestSlot >= IpcV3::RingSize)
                return false;
            out = ring.slots[ring.latestSlot];
            return out.frameId != 0;
        }

        bool FrameParity(const SharedRenderFrameState& v2,
            const IpcV3::FrameDescriptor& v3) noexcept
        {
            const auto expected = IpcV3::ShadowV2::FrameDescriptorFromV2(v2);
            return std::memcmp(&expected, &v3, sizeof(expected)) == 0;
        }

        class ShadowBridgeRuntime
        {
        public:
            ShadowBridgeRuntime() : worker_([this] { Run(); }) {}

            ~ShadowBridgeRuntime()
            {
                stop_.store(true, std::memory_order_release);
                if (worker_.joinable())
                    worker_.join();
            }

        private:
            void Run()
            {
                JsonLog log;
                log.Write("INFO", "shadow_start",
                    "v3 shadow bridge started; bounded polling and retryable writers active; v2 remains runtime authority until parity is proven");

                Ipc::ReadOnlyMapping<SharedPoseState> legacyPoseMapping;
                Ipc::ReadOnlyMapping<SharedRenderFrameRing> legacyFrameMapping;
                Ipc::ReadOnlyMapping<IpcV3::ClientState> clientMapping;
                Ipc::ReadOnlyMapping<IpcV3::FrameRing> v3FrameMapping;
                std::unique_ptr<HostStateV3Writer> hostWriter;
                std::unique_ptr<AckStateWriter> ackWriter;

                std::uint32_t lastPoseSequence = 0;
                std::uint32_t lastAckToken = 0;
                std::uint32_t lastConsumedFrame = 0;
                std::uint64_t hostPublishes = 0;
                std::uint64_t ackPublishes = 0;
                std::uint64_t clientParityOk = 0;
                std::uint64_t clientParityLag = 0;
                std::uint64_t frameParityOk = 0;
                std::uint64_t frameParityLag = 0;
                ULONGLONG lastSummaryMs = 0;
                ULONGLONG firstActiveMs = 0;
                ULONGLONG nextWriterRetryMs = 0;
                bool fallbackHintLogged = false;
                bool readyLogged = false;

                while (!stop_.load(std::memory_order_acquire))
                {
                    if (!legacyPoseMapping.EnsureOpen(SharedMemoryName))
                    {
                        Sleep(50);
                        continue;
                    }

                    SharedPoseState pose{};
                    if (!IpcV3::ShadowV2::StableReadPose(
                            legacyPoseMapping.Get(), pose))
                    {
                        Sleep(ShadowIdlePollMs);
                        continue;
                    }
                    if (pose.hostPid != GetCurrentProcessId())
                    {
                        Sleep(ShadowIdlePollMs);
                        continue;
                    }

                    if (!firstActiveMs)
                        firstActiveMs = GetTickCount64();

                    SharedRenderFrameState latestFrame{};
                    bool haveLatestFrame = false;
                    if (legacyFrameMapping.EnsureOpen(RenderFrameMemoryName))
                    {
                        SharedRenderFrameRing ring{};
                        if (IpcV3::ShadowV2::StableReadFrameRing(
                                legacyFrameMapping.Get(), ring))
                            haveLatestFrame =
                                IpcV3::ShadowV2::LatestFrame(ring, latestFrame);
                    }

                    const std::uint32_t directGeneration = haveLatestFrame &&
                        (latestFrame.flags & RenderFrameDirectGpuTransport)
                        ? latestFrame.reserved[RenderFrameDirectGenerationIndex] : 0;

                    const ULONGLONG now = GetTickCount64();
                    const bool writerMissing = !hostWriter || !ackWriter ||
                        (ackWriter && !ackWriter->Ready());
                    if (writerMissing && now >= nextWriterRetryMs)
                    {
                        if (ackWriter && !ackWriter->Ready())
                            ackWriter.reset();
                        if (!hostWriter)
                        {
                            try
                            {
                                hostWriter = std::make_unique<HostStateV3Writer>(
                                    pose.hostAdapterLuidLow,
                                    IpcV3::ShadowV2::SignedBits(
                                        pose.hostAdapterLuidHigh));
                            }
                            catch (const std::exception& error)
                            {
                                log.Write("ERROR", "host_v3_create_failed",
                                    error.what());
                                hostWriter.reset();
                            }
                        }
                        if (hostWriter && !ackWriter)
                            ackWriter = std::make_unique<AckStateWriter>();
                        nextWriterRetryMs = now + ShadowWriterRetryMs;

                        if (hostWriter && ackWriter && ackWriter->Ready() &&
                            !readyLogged)
                        {
                            readyLogged = true;
                            std::ostringstream message;
                            message << "host v3 writer ready adapter="
                                << pose.hostAdapterLuidHigh << ':'
                                << pose.hostAdapterLuidLow;
                            log.Write("INFO", "host_v3_ready", message.str());
                            std::cout
                                << "[v3] live shadow HostState/AckState bridge ready.\n";
                        }
                    }

                    if (pose.sequence != lastPoseSequence && hostWriter)
                    {
                        hostWriter->SyncReferenceSpaceGeneration(
                            pose.reserved[HostReferenceSpaceGenerationIndex]);
                        auto state = IpcV3::ShadowV2::HostStateFromV2(
                            pose, directGeneration);
                        hostWriter->Publish(state);
                        lastPoseSequence = pose.sequence;
                        ++hostPublishes;
                    }

                    if (ackWriter && ackWriter->Ready() &&
                        (pose.hostInteropProbeAckToken != lastAckToken ||
                         pose.hostDirectConsumedFrameId != lastConsumedFrame))
                    {
                        auto ack = IpcV3::ShadowV2::AckStateFromV2(
                            pose, haveLatestFrame ? &latestFrame : nullptr,
                            GetCurrentProcessId());
                        ackWriter->Publish(ack);
                        lastAckToken = pose.hostInteropProbeAckToken;
                        lastConsumedFrame = pose.hostDirectConsumedFrameId;
                        ++ackPublishes;
                    }

                    IpcV3::ClientState client{};
                    if (ReadClient(clientMapping, client))
                    {
                        const auto expected =
                            IpcV3::ShadowV2::ClientStateFromV2(
                                pose, haveLatestFrame ? &latestFrame : nullptr);
                        if (client.clientPid == pose.clientPid &&
                            client.flags == expected.flags &&
                            client.presentationMode == expected.presentationMode &&
                            client.gameState == expected.gameState &&
                            client.stereoState == expected.stereoState &&
                            client.lastFailure == expected.lastFailure &&
                            client.lastPresentedFrameId == expected.lastPresentedFrameId &&
                            client.lastRenderedPoseId == expected.lastRenderedPoseId)
                            ++clientParityOk;
                        else
                            ++clientParityLag;
                    }

                    if (haveLatestFrame &&
                        v3FrameMapping.EnsureOpen(IpcV3::FrameRingName))
                    {
                        IpcV3::FrameRing ring{};
                        IpcV3::FrameDescriptor frame{};
                        if (StableReadV3FrameRing(v3FrameMapping.Get(), ring) &&
                            LatestV3Frame(ring, frame))
                        {
                            if (frame.frameId == latestFrame.frameId &&
                                FrameParity(latestFrame, frame))
                                ++frameParityOk;
                            else
                                ++frameParityLag;
                        }
                    }

                    if (!fallbackHintLogged && firstActiveMs &&
                        now - firstActiveMs > 5000 &&
                        (pose.flags & HostDirectGpuTransport) &&
                        !(pose.flags & HostDirectGpuReady))
                    {
                        fallbackHintLogged = true;
                        log.Write("WARN", "direct_gpu_not_ready",
                            "D3D9Ex direct sharing is not ready after 5s; Desktop Duplication fallback remains active.");
                    }

                    if (now - lastSummaryMs >= 5000)
                    {
                        lastSummaryMs = now;
                        std::ostringstream message;
                        message << "hostPub=" << hostPublishes
                            << " ackPub=" << ackPublishes
                            << " clientOk=" << clientParityOk
                            << " clientLag=" << clientParityLag
                            << " frameOk=" << frameParityOk
                            << " frameLag=" << frameParityLag
                            << " pose=" << pose.sequence
                            << " frame=" << (haveLatestFrame ? latestFrame.frameId : 0)
                            << " failure=" << (haveLatestFrame ? latestFrame.failureReason : 0)
                            << " hostFlags=0x" << std::hex << pose.flags << std::dec
                            << " directSupported="
                            << ((pose.flags & HostDirectGpuTransport) ? 1 : 0)
                            << " directReady="
                            << ((pose.flags & HostDirectGpuReady) ? 1 : 0)
                            << " probeHandle=" << pose.clientInteropProbeHandle
                            << " probeToken=" << pose.clientInteropProbeToken
                            << " probeAck=" << pose.hostInteropProbeAckToken
                            << " consumed=" << pose.hostDirectConsumedFrameId;
                        log.Write("INFO", "shadow_summary", message.str());
                        std::cout << "[v3] " << message.str() << '\n';
                    }

                    Sleep(ShadowIdlePollMs);
                }

                log.Write("INFO", "shadow_stop", "v3 shadow bridge stopped");
            }

            std::atomic<bool> stop_{false};
            std::thread worker_;
        };

        ShadowBridgeRuntime g_shadowBridgeRuntime;
    }
}
