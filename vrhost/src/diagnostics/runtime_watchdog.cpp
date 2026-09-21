#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#include "vr/ipc/protocol.hpp"
#include "vr/ipc/protocol_v3.hpp"
#include "vr/ipc/shadow_legacy_v2.hpp"
#include "vr/ipc/win32_channel.hpp"

namespace OutRunVR::Host::Diagnostics
{
    namespace
    {
        std::string Timestamp()
        {
            SYSTEMTIME t{};
            GetSystemTime(&t);
            std::ostringstream s;
            s << std::setfill('0') << std::setw(4) << t.wYear << '-'
              << std::setw(2) << t.wMonth << '-' << std::setw(2) << t.wDay << 'T'
              << std::setw(2) << t.wHour << ':' << std::setw(2) << t.wMinute << ':'
              << std::setw(2) << t.wSecond << '.' << std::setw(3) << t.wMilliseconds << 'Z';
            return s.str();
        }

        std::string BoundedString(const char* text, std::size_t capacity)
        {
            if (!text || !capacity) return {};
            std::size_t length = 0;
            while (length < capacity && text[length] != '\0') ++length;
            return std::string(text, length);
        }

        class Log
        {
        public:
            Log() { stream_.open("outrun-vr-watchdog.log", std::ios::out | std::ios::trunc); }
            void Write(const char* level, const std::string& message)
            {
                if (!stream_.is_open()) return;
                stream_ << Timestamp() << ' ' << level << ' ' << message << '\n';
                stream_.flush();
            }
        private:
            std::ofstream stream_;
        };

        template <typename T>
        bool StableReadState(const Ipc::ReadOnlyMapping<T>& mapping, T& out) noexcept
        {
            return mapping.IsOpen() && Ipc::StableRead(mapping.Get(), out);
        }

        bool StableReadFrameRing(const IpcV3::FrameRing* shared, IpcV3::FrameRing& out) noexcept
        {
            if (!shared) return false;
            for (int attempt = 0; attempt < 6; ++attempt)
            {
                const std::uint32_t before = shared->publishSequence;
                if (before & 1u) continue;
                MemoryBarrier();
                std::memcpy(&out, shared, sizeof(out));
                MemoryBarrier();
                const std::uint32_t after = shared->publishSequence;
                if (before == after && !(after & 1u)) return true;
            }
            return false;
        }

        bool HeaderValid(const IpcV3::ClientState& s) noexcept
        {
            return s.magic == IpcV3::ClientMagic && s.version == IpcV3::ProtocolVersion &&
                s.structSize == sizeof(s);
        }

        bool HeaderValid(const IpcV3::AckState& s) noexcept
        {
            return s.magic == IpcV3::AckMagic && s.version == IpcV3::ProtocolVersion &&
                s.structSize == sizeof(s);
        }

        bool HeaderValid(const IpcV3::FrameRing& s) noexcept
        {
            return s.magic == IpcV3::FrameMagic && s.version == IpcV3::ProtocolVersion &&
                s.structSize == sizeof(s) && s.slotCount == IpcV3::RingSize;
        }

        class RuntimeWatchdog
        {
        public:
            RuntimeWatchdog() : thread_([this] { Run(); }) {}
            ~RuntimeWatchdog()
            {
                stop_.store(true, std::memory_order_release);
                if (thread_.joinable()) thread_.join();
            }

        private:
            void Run()
            {
                Log log;
                log.Write("INFO", "watchdog_start pid=" + std::to_string(GetCurrentProcessId()));

                Ipc::ReadOnlyMapping<SharedPoseState> v2Pose;
                Ipc::ReadOnlyMapping<SharedRenderFrameRing> v2Frames;
                Ipc::ReadOnlyMapping<IpcV3::ClientState> v3Client;
                Ipc::ReadOnlyMapping<IpcV3::FrameRing> v3Frames;
                Ipc::ReadOnlyMapping<IpcV3::AckState> v3Ack;

                ULONGLONG activeSince = 0;
                ULONGLONG lastSummary = 0;
                ULONGLONG lastPoseChange = 0;
                ULONGLONG lastFrameChange = 0;
                std::uint32_t lastPoseSequence = 0;
                std::uint32_t lastHeartbeat = 0;
                std::uint32_t lastFrameId = 0;
                bool noClientWarned = false;
                bool noStereoWarned = false;
                bool poseStallWarned = false;
                bool frameStallWarned = false;

                while (!stop_.load(std::memory_order_acquire))
                {
                    v2Pose.EnsureOpen(SharedMemoryName);
                    v2Frames.EnsureOpen(RenderFrameMemoryName);
                    v3Client.EnsureOpen(IpcV3::ClientStateName);
                    v3Frames.EnsureOpen(IpcV3::FrameRingName);
                    v3Ack.EnsureOpen(IpcV3::AckStateName);

                    SharedPoseState pose{};
                    const bool havePose = v2Pose.IsOpen() &&
                        IpcV3::ShadowV2::StableReadPose(v2Pose.Get(), pose) &&
                        pose.magic == SharedMagic && pose.protocolVersion == SharedProtocolVersion;

                    SharedRenderFrameState frame{};
                    bool haveFrame = false;
                    if (v2Frames.IsOpen())
                    {
                        SharedRenderFrameRing ring{};
                        if (IpcV3::ShadowV2::StableReadFrameRing(v2Frames.Get(), ring))
                            haveFrame = IpcV3::ShadowV2::LatestFrame(ring, frame);
                    }

                    IpcV3::ClientState client{};
                    const bool haveClient = v3Client.IsOpen() && StableReadState(v3Client, client) && HeaderValid(client);
                    IpcV3::FrameRing frame3{};
                    const bool haveFrame3 = v3Frames.IsOpen() && StableReadFrameRing(v3Frames.Get(), frame3) && HeaderValid(frame3);
                    IpcV3::AckState ack{};
                    const bool haveAck = v3Ack.IsOpen() && StableReadState(v3Ack, ack) && HeaderValid(ack);

                    const ULONGLONG now = GetTickCount64();
                    if (havePose && pose.hostPid == GetCurrentProcessId() && !activeSince)
                    {
                        activeSince = now;
                        lastPoseChange = now;
                        lastFrameChange = now;
                        log.Write("INFO", "host_pose_active runtime=" +
                            BoundedString(pose.runtimeName, sizeof(pose.runtimeName)));
                    }

                    if (havePose)
                    {
                        if (pose.sequence != lastPoseSequence)
                        {
                            lastPoseSequence = pose.sequence;
                            lastPoseChange = now;
                            poseStallWarned = false;
                        }
                        const std::uint32_t heartbeat = pose.reserved[ClientHeartbeatIndex];
                        if (heartbeat != lastHeartbeat)
                            lastHeartbeat = heartbeat;
                    }
                    if (haveFrame && frame.frameId != lastFrameId)
                    {
                        lastFrameId = frame.frameId;
                        lastFrameChange = now;
                        frameStallWarned = false;
                    }

                    if (activeSince)
                    {
                        if (!noClientWarned && now - activeSince > 7000 && (!havePose || pose.clientPid == 0 || lastHeartbeat == 0))
                        {
                            noClientWarned = true;
                            log.Write("WARN", "game_client_missing: dinput8 VR hook heartbeat not observed after 7s");
                        }
                        if (!noStereoWarned && now - activeSince > 12000 && lastFrameId == 0)
                        {
                            noStereoWarned = true;
                            log.Write("WARN", "stereo_frame_missing: no rendered stereo frame observed after 12s; enter gameplay if still in menus");
                        }
                        if (!poseStallWarned && lastPoseSequence && now - lastPoseChange > 2000)
                        {
                            poseStallWarned = true;
                            log.Write("WARN", "pose_stalled: OpenXR host pose sequence has not advanced for 2s");
                        }
                        if (!frameStallWarned && lastFrameId && now - lastFrameChange > 3000 &&
                            haveClient && client.presentationMode == PresentationGameplay)
                        {
                            frameStallWarned = true;
                            log.Write("WARN", "frame_stalled: game reports gameplay but stereo frame id has not advanced for 3s");
                        }
                    }

                    if (now - lastSummary >= 5000)
                    {
                        lastSummary = now;
                        std::ostringstream s;
                        s << "summary hostPose=" << (havePose ? 1 : 0)
                          << " hostSeq=" << (havePose ? pose.sequence : 0)
                          << " hostFlags=0x" << std::hex << (havePose ? pose.flags : 0) << std::dec
                          << " clientPid=" << (havePose ? pose.clientPid : 0)
                          << " heartbeat=" << lastHeartbeat
                          << " stereoFrame=" << (haveFrame ? frame.frameId : 0)
                          << " stereoState=" << (haveFrame ? frame.state : 0)
                          << " failure=" << (haveFrame ? frame.failureReason : 0)
                          << " poseSeq=" << (haveFrame ? frame.sourcePoseSequence : 0)
                          << " direct=" << ((haveFrame && (frame.flags & RenderFrameDirectGpuTransport)) ? 1 : 0)
                          << " v3Client=" << (haveClient ? 1 : 0)
                          << " v3ClientSeq=" << (haveClient ? client.sequence : 0)
                          << " v3Frame=" << (haveFrame3 ? 1 : 0)
                          << " v3LatestSlot=" << (haveFrame3 ? frame3.latestSlot : 0)
                          << " v3Ack=" << (haveAck ? 1 : 0)
                          << " ackFrame=" << (haveAck ? ack.consumedFrameId : 0)
                          << " ackSlot=" << (haveAck ? ack.consumedSlot : 0);
                        log.Write("INFO", s.str());
                    }

                    Sleep(100);
                }

                log.Write("INFO", "watchdog_stop");
            }

            std::atomic<bool> stop_{false};
            std::thread thread_;
        };

        RuntimeWatchdog g_runtimeWatchdog;
    }
}
