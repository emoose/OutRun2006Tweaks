#pragma once

#include "vr/ipc/protocol.hpp"
#include "vr/ipc/protocol_v3.hpp"
#include "vr/ipc/win32_channel.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace OutRunVR::IpcV3::ShadowV2
{
    inline bool LegacyPoseHeaderValid(const SharedPoseState& state) noexcept
    {
        return state.magic == SharedMagic &&
            state.protocolVersion == SharedProtocolVersion &&
            state.structSize == sizeof(SharedPoseState);
    }

    inline bool LegacyFrameHeaderValid(const SharedRenderFrameRing& ring) noexcept
    {
        return ring.magic == RenderFrameMagic &&
            ring.protocolVersion == RenderFrameProtocolVersion &&
            ring.structSize == sizeof(SharedRenderFrameRing) &&
            ring.slotCount == RenderFrameRingSize;
    }

    inline float FloatFromBits(std::uint32_t bits) noexcept
    {
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    inline std::int32_t SignedBits(std::uint32_t bits) noexcept
    {
        std::int32_t value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    inline bool FiniteQuaternion(const float q[4]) noexcept
    {
        if (!q)
            return false;
        float lengthSq = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            if (!std::isfinite(q[i]))
                return false;
            lengthSq += q[i] * q[i];
        }
        return std::isfinite(lengthSq) && lengthSq > 0.25f && lengthSq < 4.0f;
    }

    inline void NormalizeQuaternion(float q[4]) noexcept
    {
        float lengthSq = 0.0f;
        for (int i = 0; i < 4; ++i)
            lengthSq += q[i] * q[i];
        if (!std::isfinite(lengthSq) || lengthSq <= 1.0e-12f)
        {
            q[0] = q[1] = q[2] = 0.0f;
            q[3] = 1.0f;
            return;
        }
        const float invLength = 1.0f / std::sqrt(lengthSq);
        for (int i = 0; i < 4; ++i)
            q[i] *= invLength;
    }

    inline bool DecodePackedEyeOrientations(const SharedPoseState& snapshot, float out[2][4]) noexcept
    {
        std::int16_t packed[8]{};
        static_assert(sizeof(packed) == PackedEyeOrientationBytes);
        std::memcpy(packed, snapshot.runtimeName + PackedEyeOrientationOffset, sizeof(packed));
        for (int eye = 0; eye < 2; ++eye)
        {
            for (int c = 0; c < 4; ++c)
                out[eye][c] = static_cast<float>(packed[eye * 4 + c]) / 32767.0f;
            if (!FiniteQuaternion(out[eye]))
                return false;
            NormalizeQuaternion(out[eye]);
        }
        return true;
    }

    inline bool StableReadPose(const SharedPoseState* shared, SharedPoseState& out, int attempts = 8) noexcept
    {
        if (!shared || attempts <= 0)
            return false;
        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            const std::uint32_t before = shared->sequence;
            if (before & 1u)
                continue;
            MemoryBarrier();
            std::memcpy(&out, shared, sizeof(out));
            MemoryBarrier();
            const std::uint32_t after = shared->sequence;
            if (before == after && !(after & 1u) && LegacyPoseHeaderValid(out))
                return true;
        }
        return false;
    }

    inline bool StableReadFrameRing(const SharedRenderFrameRing* shared, SharedRenderFrameRing& out,
        int attempts = 8) noexcept
    {
        if (!shared || attempts <= 0)
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
            if (before == after && !(after & 1u) && LegacyFrameHeaderValid(out))
                return true;
        }
        return false;
    }

    inline bool LatestFrame(const SharedRenderFrameRing& ring, SharedRenderFrameState& out) noexcept
    {
        if (!LegacyFrameHeaderValid(ring))
            return false;
        const std::uint32_t slot = ring.latestSlot;
        if (slot >= RenderFrameRingSize)
            return false;
        out = ring.slots[slot];
        return out.magic == RenderFrameMagic &&
            out.protocolVersion == RenderFrameProtocolVersion &&
            out.structSize == sizeof(SharedRenderFrameState) &&
            !(out.sequence & 1u) &&
            RenderFrameRunIdentityMatches(ring, out) &&
            out.frameId != 0;
    }

    inline std::uint32_t HostFlagsFromV2(std::uint32_t flags) noexcept
    {
        std::uint32_t out = 0;
        if (flags & OutRunVR::HostAlive) out |= IpcV3::HostAlive;
        if (flags & OutRunVR::OrientationValid) out |= IpcV3::OrientationValid;
        if (flags & OutRunVR::PositionValid) out |= IpcV3::PositionValid;
        if (flags & OutRunVR::SessionVisible) out |= IpcV3::SessionVisible;
        if (flags & OutRunVR::SessionFocused) out |= IpcV3::SessionFocused;
        if (flags & OutRunVR::StereoViewsValid) out |= IpcV3::StereoViewsValid;
        if (flags & OutRunVR::HostShouldRender) out |= IpcV3::HostShouldRender;
        if (flags & OutRunVR::HostDirectGpuTransport) out |= IpcV3::DirectGpuTransportSupported;
        if (flags & OutRunVR::HostDirectGpuReady) out |= IpcV3::DirectGpuTransportReady;
        if (flags & OutRunVR::HostAdapterLuidValid) out |= IpcV3::AdapterLuidValid;
        return out;
    }

    inline HostState HostStateFromV2(const SharedPoseState& source,
        std::uint32_t directTransportGeneration = 0) noexcept
    {
        HostState out{};
        InitializeWireState(out);
        out.hostPid = source.hostPid;
        out.flags = HostFlagsFromV2(source.flags);
        out.referenceSpaceGeneration = source.reserved[HostReferenceSpaceGenerationIndex];
        out.directTransportGeneration = directTransportGeneration;
        out.poseId = source.sequence;
        out.predictedDisplayTime = 0;
        out.sampleQpc = source.sampleQpc;
        std::memcpy(out.headOrientation, source.orientation, sizeof(out.headOrientation));
        std::memcpy(out.headPositionMeters, source.position, sizeof(out.headPositionMeters));
        for (int eye = 0; eye < 2; ++eye)
        {
            out.recommendedWidth[eye] = source.recommendedWidth[eye];
            out.recommendedHeight[eye] = source.recommendedHeight[eye];
            out.eyes[eye].fov.angleLeft = source.eyeFov[eye].angleLeft;
            out.eyes[eye].fov.angleRight = source.eyeFov[eye].angleRight;
            out.eyes[eye].fov.angleUp = source.eyeFov[eye].angleUp;
            out.eyes[eye].fov.angleDown = source.eyeFov[eye].angleDown;
        }
        out.adapterLuidLow = source.hostAdapterLuidLow;
        out.adapterLuidHigh = SignedBits(source.hostAdapterLuidHigh);

        out.eyes[0].positionMeters[0] = FloatFromBits(source.reserved[HostEyeOffsetLeftXIndex]);
        out.eyes[0].positionMeters[1] = FloatFromBits(source.reserved[HostEyeOffsetLeftYIndex]);
        out.eyes[0].positionMeters[2] = FloatFromBits(source.reserved[HostEyeOffsetLeftZIndex]);
        out.eyes[1].positionMeters[0] = FloatFromBits(source.reserved[HostEyeOffsetRightXIndex]);
        out.eyes[1].positionMeters[1] = FloatFromBits(source.reserved[HostEyeOffsetRightYIndex]);
        out.eyes[1].positionMeters[2] = FloatFromBits(source.reserved[HostEyeOffsetRightZIndex]);

        float eyeOrientation[2][4]{};
        if ((source.flags & StereoEyeOrientationValid) && DecodePackedEyeOrientations(source, eyeOrientation))
        {
            std::memcpy(out.eyes[0].orientation, eyeOrientation[0], sizeof(out.eyes[0].orientation));
            std::memcpy(out.eyes[1].orientation, eyeOrientation[1], sizeof(out.eyes[1].orientation));
        }
        else
        {
            out.eyes[0].orientation[3] = 1.0f;
            out.eyes[1].orientation[3] = 1.0f;
        }

        constexpr std::size_t RuntimeTextBytes = PackedEyeOrientationOffset;
        const std::size_t copyBytes = (std::min)(RuntimeTextBytes, sizeof(out.runtimeName) - 1u);
        std::memcpy(out.runtimeName, source.runtimeName, copyBytes);
        out.runtimeName[copyBytes] = '\0';
        return out;
    }

    inline FrameDescriptor FrameDescriptorFromV2(const SharedRenderFrameState& source) noexcept
    {
        FrameDescriptor out{};
        out.frameId = source.frameId;
        out.renderPoseId = source.sourcePoseSequence;
        out.presentQpc = source.presentQpc;
        out.presentationMode = source.presentationMode;
        out.flags = source.flags;
        out.failureReason = source.failureReason;

        const bool direct = (source.flags & RenderFrameDirectGpuTransport) != 0;
        out.transportKind = static_cast<std::uint32_t>(direct
            ? Core::TransportKind::D3D9ExShared
            : Core::TransportKind::DesktopDuplication);
        out.transportGeneration = direct ? source.reserved[RenderFrameDirectGenerationIndex] : 0;
        out.transportSlot = direct ? source.reserved[RenderFrameDirectSlotIndex] : 0;
        out.width = direct && source.reserved[RenderFrameDirectWidthIndex]
            ? source.reserved[RenderFrameDirectWidthIndex] : source.backbufferWidth;
        out.height = direct && source.reserved[RenderFrameDirectHeightIndex]
            ? source.reserved[RenderFrameDirectHeightIndex] : source.backbufferHeight;
        out.format = direct ? source.reserved[RenderFrameDirectFormatIndex] : 0;
        out.leftHandle = direct ? static_cast<WireHandle>(source.reserved[RenderFrameDirectLeftHandleIndex]) : 0;
        out.rightHandle = direct ? static_cast<WireHandle>(source.reserved[RenderFrameDirectRightHandleIndex]) : 0;

        for (int eye = 0; eye < 2; ++eye)
        {
            std::memcpy(out.renderedEyes[eye].orientation, source.eye[eye].orientation,
                sizeof(out.renderedEyes[eye].orientation));
            std::memcpy(out.renderedEyes[eye].positionMeters, source.eye[eye].position,
                sizeof(out.renderedEyes[eye].positionMeters));
            out.renderedEyes[eye].fov.angleLeft = source.eye[eye].fov.angleLeft;
            out.renderedEyes[eye].fov.angleRight = source.eye[eye].fov.angleRight;
            out.renderedEyes[eye].fov.angleUp = source.eye[eye].fov.angleUp;
            out.renderedEyes[eye].fov.angleDown = source.eye[eye].fov.angleDown;
        }
        return out;
    }

    inline ClientState ClientStateFromV2(const SharedPoseState& pose,
        const SharedRenderFrameState* latestFrame) noexcept
    {
        ClientState out{};
        InitializeWireState(out);
        out.clientPid = pose.clientPid;
        out.flags = pose.reserved[ClientFlagsIndex];
        out.presentationMode = pose.reserved[ClientPresentationModeIndex];
        out.gameState = pose.reserved[ClientGameStateIndex];
        out.stereoState = pose.reserved[ClientStereoStateIndex];
        out.adapterLuidLow = pose.clientAdapterLuidLow;
        out.adapterLuidHigh = SignedBits(pose.clientAdapterLuidHigh);
        out.interopProbeGeneration = 0;
        out.interopProbeToken = pose.clientInteropProbeToken;
        out.interopProbeHandle = static_cast<WireHandle>(pose.clientInteropProbeHandle);
        out.backbufferWidth = pose.reserved[ClientStereoBackbufferWidthIndex];
        out.backbufferHeight = pose.reserved[ClientStereoBackbufferHeightIndex];
        if (latestFrame)
        {
            out.lastFailure = latestFrame->failureReason;
            out.lastPresentedFrameId = latestFrame->frameId;
            out.lastRenderedPoseId = latestFrame->sourcePoseSequence;
        }
        return out;
    }

    inline AckState AckStateFromV2(const SharedPoseState& pose,
        const SharedRenderFrameState* latestFrame, std::uint32_t consumerPid) noexcept
    {
        AckState out{};
        InitializeWireState(out);
        out.consumerPid = consumerPid;
        out.acceptedProbeGeneration = 0;
        out.acceptedProbeToken = pose.hostInteropProbeAckToken;
        out.consumedFrameId = pose.hostDirectConsumedFrameId;
        if (latestFrame && latestFrame->frameId == pose.hostDirectConsumedFrameId)
        {
            out.transportGeneration = latestFrame->reserved[RenderFrameDirectGenerationIndex];
            out.consumedSlot = latestFrame->reserved[RenderFrameDirectSlotIndex];
        }
        return out;
    }
}
