#pragma once

// R23 final OpenXR presentation authority.
// A gameplay layer is allowed only when the host loop has committed one stable
// Frame.v2 + image-source + pose/FOV bundle. Once committed, that host-owned
// bundle remains authoritative for its short grace lifetime even if the game
// publishes the next Frame.v2 before xrEndFrame is called.

#include "r22_runtime_hardening.hpp"
#include "r23_verified_bundle.hpp"

#ifdef xrEndFrame
#undef xrEndFrame
#endif
#ifdef xrDestroySession
#undef xrDestroySession
#endif

#include <d3d9types.h>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>

namespace OutRunVrR23RuntimeHardening
{
    inline constexpr const char* BuildId = "R23-owned-verified-bundle-submit-20260916";
    inline bool FirstActiveLogged = false;
    inline bool FirstBundleBlockLogged = false;
    inline bool FirstFallbackLogged = false;
    inline bool FirstFallbackSourceMismatchLogged = false;
    inline bool FirstDirectSafeReuseLogged = false;
    inline bool FirstDirectFormatMismatchLogged = false;
    inline bool FirstMixedLayerBlockLogged = false;

    inline std::atomic<std::uint32_t> LastSubmittedFrameId{ 0 };
    inline std::atomic<std::uint32_t> LastSubmittedKind{
        static_cast<std::uint32_t>(OutRunVrR23VerifiedBundle::SourceKind::None) };
    inline std::atomic<bool> LastSubmittedLayer{ false };
    inline std::atomic<std::uint64_t> SubmissionSerial{ 0 };
    inline std::atomic<std::uint64_t> LastSubmissionLogMs{ 0 };
    inline constexpr std::uint64_t SubmissionLogIntervalMs = 2000;

    inline void RecordFinalSubmission(std::uint32_t frameId,
        OutRunVrR23VerifiedBundle::SourceKind kind, bool hasLayer) noexcept
    {
        LastSubmittedFrameId.store(frameId, std::memory_order_relaxed);
        LastSubmittedKind.store(static_cast<std::uint32_t>(kind),
            std::memory_order_relaxed);
        LastSubmittedLayer.store(hasLayer, std::memory_order_relaxed);
        const std::uint64_t serial =
            SubmissionSerial.fetch_add(1, std::memory_order_release) + 1;

        const std::uint64_t now = GetTickCount64();
        std::uint64_t previous =
            LastSubmissionLogMs.load(std::memory_order_relaxed);
        if (now >= previous + SubmissionLogIntervalMs &&
            LastSubmissionLogMs.compare_exchange_strong(
                previous, now, std::memory_order_relaxed))
        {
            std::cerr
                << "[R23 final-submit] serial=" << serial
                << " frameId=" << frameId
                << " sourceKind=" << static_cast<std::uint32_t>(kind)
                << " layer=" << (hasLayer ? 1 : 0)
                << "\n";
        }
    }

    inline XrResult SubmitNoLayer(XrSession session,
        const XrFrameEndInfo* endInfo) noexcept
    {
        if (!endInfo)
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        XrFrameEndInfo safe = *endInfo;
        safe.layerCount = 0;
        safe.layers = nullptr;
        return OutRunVrFinalTest::EndFrame(session, &safe);
    }

    inline bool IncomingContainsProjectionLayer(
        const XrFrameEndInfo* endInfo) noexcept
    {
        if (!endInfo || !endInfo->layers)
            return false;
        for (std::uint32_t i = 0; i < endInfo->layerCount; ++i)
        {
            const auto* layer = endInfo->layers[i];
            if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                return true;
        }
        return false;
    }

    inline XrResult SubmitNonProjectionOnly(XrSession session,
        const XrFrameEndInfo* endInfo) noexcept
    {
        if (!endInfo || !endInfo->layers)
            return SubmitNoLayer(session, endInfo);

        constexpr std::size_t MaxPreservedLayers = 16;
        std::array<const XrCompositionLayerBaseHeader*, MaxPreservedLayers> layers{};
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < endInfo->layerCount; ++i)
        {
            const auto* layer = endInfo->layers[i];
            if (!layer || layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                continue;
            if (count >= layers.size())
                return SubmitNoLayer(session, endInfo);
            layers[count++] = layer;
        }

        XrFrameEndInfo safe = *endInfo;
        safe.layerCount = count;
        safe.layers = count ? layers.data() : nullptr;
        return OutRunVrFinalTest::EndFrame(session, &safe);
    }

    inline bool Near(float a, float b, float epsilon = 1.0e-4f) noexcept
    {
        return std::isfinite(a) && std::isfinite(b) &&
            std::fabs(a - b) <= epsilon;
    }

    inline bool IncomingPoseMatchesCommittedFrame(
        const XrFrameEndInfo* endInfo,
        const OutRunVR::SharedRenderFrameState& frame) noexcept
    {
        if (!OutRunVrReviewHardening::IncomingProjectionValid(endInfo))
            return false;
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(
            endInfo->layers[0]);
        if (!projection || projection->viewCount < 2 || !projection->views)
            return false;

        for (int eye = 0; eye < 2; ++eye)
        {
            const auto& view = projection->views[eye];
            const auto& wire = frame.eye[eye];
            if (!Near(view.pose.orientation.x, wire.orientation[0]) ||
                !Near(view.pose.orientation.y, wire.orientation[1]) ||
                !Near(view.pose.orientation.z, wire.orientation[2]) ||
                !Near(view.pose.orientation.w, wire.orientation[3]) ||
                !Near(view.pose.position.x, wire.position[0]) ||
                !Near(view.pose.position.y, wire.position[1]) ||
                !Near(view.pose.position.z, wire.position[2]) ||
                !Near(view.fov.angleLeft, wire.fov.angleLeft) ||
                !Near(view.fov.angleRight, wire.fov.angleRight) ||
                !Near(view.fov.angleUp, wire.fov.angleUp) ||
                !Near(view.fov.angleDown, wire.fov.angleDown))
                return false;
        }
        return true;
    }

    inline DXGI_FORMAT ExpectedDirectDxgiFormat(std::uint32_t declared) noexcept
    {
        switch (static_cast<D3DFORMAT>(declared))
        {
        case D3DFMT_A8R8G8B8:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case D3DFMT_A8B8G8R8:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case D3DFMT_A2B10G10R10:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case D3DFMT_A16B16G16R16F:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    }

    inline bool DirectSafeEyeMatchesCommittedFrame(
        const OutRunVR::SharedRenderFrameState& frame) noexcept
    {
        using namespace OutRunVrD3D9ExDirectPassthrough;
        const auto expected = ExpectedDirectDxgiFormat(
            frame.reserved[OutRunVR::RenderFrameDirectFormatIndex]);
        const std::uint32_t width =
            frame.reserved[OutRunVR::RenderFrameDirectWidthIndex];
        const std::uint32_t height =
            frame.reserved[OutRunVR::RenderFrameDirectHeightIndex];
        const bool match = expected != DXGI_FORMAT_UNKNOWN &&
            SafeEyeFormat == expected && width != 0 && height != 0 &&
            SafeEyeWidth == width && SafeEyeHeight == height &&
            width == frame.backbufferWidth && height == frame.backbufferHeight;
        if (!match && !FirstDirectFormatMismatchLogged)
        {
            FirstDirectFormatMismatchLogged = true;
            std::cerr
                << "[R23] direct safe-eye rejected: actual resource format/size does not match committed Frame.v2 declaration\n";
        }
        return match;
    }

    inline XrResult RenderCommittedDirect(XrSession session,
        const XrFrameEndInfo* endInfo,
        const OutRunVrR23VerifiedBundle::Snapshot& verified)
    {
        using namespace OutRunVrD3D9ExDirectPassthrough;
        const auto kind = OutRunVrR23VerifiedBundle::SourceKind::DirectGpu;
        const auto& frame = verified.frame;

        if (!OutRunVrR21RuntimeHardening::DirectTransportRequested() ||
            !IncomingPoseMatchesCommittedFrame(endInfo, frame))
        {
            RecordFinalSubmission(frame.frameId, kind, false);
            return SubmitNoLayer(session, endInfo);
        }

        const auto state = OutRunVrR21RuntimeHardening::ReadDirectHostStateReadonly();
        if (!OutRunVrR22RuntimeHardening::DirectOpenMatchesLatest(state, frame))
        {
            RecordFinalSubmission(frame.frameId, kind, false);
            return SubmitNoLayer(session, endInfo);
        }

        // Once CopySharedFrameToSafeEyes completed, SafeEye is host-owned and
        // independent of producer ring reuse. Reuse it directly even when the
        // original Frame.v2 slot has already advanced/been overwritten.
        const bool safeAlreadyOwned = SafeFrameId == frame.frameId &&
            SafeTransportGeneration ==
                frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex] &&
            SafeEyeSrv[0] && SafeEyeSrv[1];
        if (!safeAlreadyOwned && !EnsureSafeFrame(frame.frameId))
        {
            RecordFinalSubmission(frame.frameId, kind, false);
            return SubmitNoLayer(session, endInfo);
        }
        if (!DirectSafeEyeMatchesCommittedFrame(frame))
        {
            RecordFinalSubmission(frame.frameId, kind, false);
            return SubmitNoLayer(session, endInfo);
        }
        if (safeAlreadyOwned && !FirstDirectSafeReuseLogged)
        {
            FirstDirectSafeReuseLogged = true;
            std::cerr
                << "[R23] reusing host-owned direct safe-eye bundle after producer advanced; frame="
                << frame.frameId << "\n";
        }

        OutRunVrR21RuntimeHardening::BindLegacyBlitConstantBufferToVs();
        XrCompositionLayerProjection projection{};
        std::array<XrCompositionLayerProjectionView, 2> views{};
        if (!RenderSafeProjection(session, endInfo, projection, views))
        {
            RecordFinalSubmission(frame.frameId, kind, false);
            return SubmitNoLayer(session, endInfo);
        }

        const XrCompositionLayerBaseHeader* layer =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo patched = *endInfo;
        patched.layerCount = 1;
        patched.layers = &layer;
        RecordFinalSubmission(frame.frameId, kind, true);
        return OutRunVrFinalTest::EndFrame(session, &patched);
    }

    inline XrResult XRAPI_CALL EndFrame(XrSession session,
        const XrFrameEndInfo* endInfo)
    {
        using OutRunVrR23VerifiedBundle::SourceKind;

        if (!FirstActiveLogged)
        {
            FirstActiveLogged = true;
            std::cerr
                << "[R23] final presentation authority ACTIVE build=" << BuildId
                << "; committed host-owned bundle survives newer producer frames for "
                << OutRunVrR23VerifiedBundle::MaxPresentationAgeMs << "ms\n";
        }

        const bool hasNonProjection =
            OutRunVrReviewHardening::HasIncomingNonProjectionLayer(endInfo);
        const bool hasProjection = IncomingContainsProjectionLayer(endInfo);
        if (hasNonProjection && !hasProjection)
        {
            RecordFinalSubmission(0, SourceKind::None,
                endInfo && endInfo->layerCount > 0);
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }
        if (hasNonProjection && hasProjection)
        {
            if (!FirstMixedLayerBlockLogged)
            {
                FirstMixedLayerBlockLogged = true;
                std::cerr
                    << "[R23] mixed projection/non-projection frame detected; dropping unverified projection while preserving non-projection layers\n";
            }
            RecordFinalSubmission(0, SourceKind::None, true);
            return SubmitNonProjectionOnly(session, endInfo);
        }

        if (!OutRunVrR21RuntimeHardening::HostShouldRenderReadonly())
        {
            RecordFinalSubmission(0, SourceKind::None, false);
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }

        OutRunVrR23VerifiedBundle::Snapshot verified{};
        if (!OutRunVrR23VerifiedBundle::ReadFresh(verified) ||
            !OutRunVrSbsCaptureOverride::FrameComplete(verified.frame))
        {
            if (!FirstBundleBlockLogged)
            {
                FirstBundleBlockLogged = true;
                std::cerr
                    << "[R23] gameplay layer blocked: no fresh committed host-owned frame/source/pose bundle\n";
            }
            RecordFinalSubmission(0, SourceKind::None, false);
            return SubmitNoLayer(session, endInfo);
        }

        if (verified.kind == SourceKind::DirectGpu)
            return RenderCommittedDirect(session, endInfo, verified);

        if (verified.kind != SourceKind::ClassicSbs)
        {
            RecordFinalSubmission(verified.frameId, verified.kind, false);
            return SubmitNoLayer(session, endInfo);
        }

        if (OutRunVrReviewHardening::IncomingProjectionValid(endInfo))
        {
            if (IncomingPoseMatchesCommittedFrame(endInfo, verified.frame))
            {
                RecordFinalSubmission(verified.frameId, verified.kind, true);
                return OutRunVrFinalTest::EndFrame(session, endInfo);
            }
            RecordFinalSubmission(verified.frameId, verified.kind, false);
            return SubmitNoLayer(session, endInfo);
        }

        const bool exactClassicSource = verified.sourceCaptureQpc > 0 &&
            OutRunVrSbsCaptureOverride::LastProductionPresentQpc ==
                verified.sourceCaptureQpc;
        if (exactClassicSource &&
            OutRunVrReviewHardening::FreshClassicFallbackAvailable())
        {
            OutRunVrR21RuntimeHardening::BindLegacyBlitConstantBufferToVs();
            if (!FirstFallbackLogged)
            {
                FirstFallbackLogged = true;
                std::cerr
                    << "[R23] R19 classic fallback allowed for exact committed source/frame bundle only\n";
            }
            RecordFinalSubmission(verified.frameId, verified.kind, true);
            return OutRunVrSbsCaptureOverride::EndFrame(session, endInfo);
        }

        if (!exactClassicSource && !FirstFallbackSourceMismatchLogged)
        {
            FirstFallbackSourceMismatchLogged = true;
            std::cerr
                << "[R23] classic fallback blocked: current production capture is not the committed bundle source\n";
        }
        RecordFinalSubmission(verified.frameId, verified.kind, false);
        return OutRunVrFinalTest::EndFrame(session, endInfo);
    }

    inline XrResult XRAPI_CALL DestroySession(XrSession session)
    {
        OutRunVrR23VerifiedBundle::Invalidate();
        RecordFinalSubmission(0, OutRunVrR23VerifiedBundle::SourceKind::None, false);
        return OutRunVrR22RuntimeHardening::DestroySession(session);
    }
}

#define xrEndFrame OutRunVrR23RuntimeHardening::EndFrame
#define xrDestroySession OutRunVrR23RuntimeHardening::DestroySession
