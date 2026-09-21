#pragma once

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "vr/ipc/host_state_v3_reader.hpp"
#include "vr/ipc/protocol.hpp"

namespace OutRunVR::IpcV3
{
    struct HostPoseSnapshot
    {
        std::uint64_t poseId{};
        std::uint32_t hostPid{};
        std::uint32_t referenceSpaceGeneration{};
        bool positionValid{};
        bool stereoValid{};
        float headOrientation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        float headPositionMeters[3]{};
        WireEyeView eyes[2]{};
    };

    inline bool QuaternionSane(const float q[4]) noexcept
    {
        if (!q)
            return false;
        const float lengthSq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        return std::isfinite(q[0]) && std::isfinite(q[1]) &&
            std::isfinite(q[2]) && std::isfinite(q[3]) &&
            std::isfinite(lengthSq) && lengthSq > 0.25f && lengthSq < 4.0f;
    }

    inline bool Vector3Finite(const float v[3]) noexcept
    {
        return v && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    }

    inline bool WireFovValid(const WireFov& fov) noexcept
    {
        constexpr float limit = 1.56f;
        return std::isfinite(fov.angleLeft) && std::isfinite(fov.angleRight) &&
            std::isfinite(fov.angleUp) && std::isfinite(fov.angleDown) &&
            fov.angleLeft > -limit && fov.angleRight < limit &&
            fov.angleDown > -limit && fov.angleUp < limit &&
            fov.angleRight > fov.angleLeft + 0.05f &&
            fov.angleUp > fov.angleDown + 0.05f;
    }

    inline bool PoseSequenceMatchesLegacy(
        std::uint64_t poseId, std::uint32_t legacySequence) noexcept
    {
        return poseId != 0 && legacySequence != 0 &&
            static_cast<std::uint32_t>(poseId & 0xFFFFFFFFull) == legacySequence;
    }

    inline bool HostStateUsable(
        const HostState& state,
        std::int64_t nowQpc,
        std::int64_t qpcFrequency,
        std::int64_t staleMilliseconds = 250) noexcept
    {
        const std::uint32_t required = HostAlive | OrientationValid |
            SessionVisible | HostShouldRender;
        if ((state.flags & required) != required || state.hostPid == 0 || state.poseId == 0)
            return false;
        if (!QuaternionSane(state.headOrientation))
            return false;
        if (qpcFrequency <= 0 || state.sampleQpc <= 0 || staleMilliseconds <= 0)
            return false;
        const std::int64_t age = nowQpc - state.sampleQpc;
        const std::int64_t maxAge = (qpcFrequency * staleMilliseconds) / 1000;
        if (age < 0 || age > maxAge)
            return false;
        if ((state.flags & PositionValid) != 0 && !Vector3Finite(state.headPositionMeters))
            return false;

        if ((state.flags & StereoViewsValid) != 0)
        {
            for (const auto& eye : state.eyes)
            {
                if (!QuaternionSane(eye.orientation) || !Vector3Finite(eye.positionMeters) ||
                    !WireFovValid(eye.fov))
                    return false;
                if (std::fabs(eye.positionMeters[0]) > 0.25f ||
                    std::fabs(eye.positionMeters[1]) > 0.25f ||
                    std::fabs(eye.positionMeters[2]) > 0.25f)
                    return false;
            }
        }
        return true;
    }

    class HostPoseV3Source
    {
    public:
        bool Read(HostPoseSnapshot& out) noexcept
        {
            HostState state{};
            if (!reader_.Read(state))
                return false;

            LARGE_INTEGER now{};
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency))
                return false;
            if (!HostStateUsable(state, now.QuadPart, frequency.QuadPart))
                return false;

            // The current v3 HostState publisher shadows the same host sample that
            // still keys Frame.v2. Never accept a shadow pose that is one host frame
            // behind: fall back to the legacy pose for that game frame instead. This
            // keeps v3 primary when caught up while guaranteeing no added head latency.
            if (legacy_.EnsureOpen(OutRunVR::SharedMemoryName))
            {
                OutRunVR::SharedPoseState legacy{};
                if (OutRunVR::Ipc::StableRead(legacy_.Get(), legacy) &&
                    legacy.magic == OutRunVR::SharedMagic &&
                    legacy.protocolVersion == OutRunVR::SharedProtocolVersion &&
                    legacy.structSize == sizeof(legacy) &&
                    legacy.hostPid == state.hostPid &&
                    !PoseSequenceMatchesLegacy(state.poseId, legacy.sequence))
                    return false;
            }

            out = {};
            out.poseId = state.poseId;
            out.hostPid = state.hostPid;
            out.referenceSpaceGeneration = state.referenceSpaceGeneration;
            out.positionValid = (state.flags & PositionValid) != 0;
            out.stereoValid = (state.flags & StereoViewsValid) != 0;
            std::copy_n(state.headOrientation, 4, out.headOrientation);
            std::copy_n(state.headPositionMeters, 3, out.headPositionMeters);
            out.eyes[0] = state.eyes[0];
            out.eyes[1] = state.eyes[1];
            return true;
        }

        void Reset() noexcept
        {
            reader_.Reset();
            legacy_.Reset();
        }
        bool IsOpen() const noexcept { return reader_.IsOpen(); }

    private:
        HostStateReader reader_{};
        OutRunVR::Ipc::ReadOnlyMapping<OutRunVR::SharedPoseState> legacy_{};
    };
}
