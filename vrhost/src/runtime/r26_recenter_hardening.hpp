#pragma once

// R26/R28 Quest/VDXR theater recenter bridge.
//
// Runtime reference-space changes and focus-return events normalize into the
// existing LOCAL theater-anchor invalidation path. R28 keeps an F10 request
// pending until a visible frame has actually been submitted. When the normal
// host has no layer yet (the startup/menu failure mode), R28 reuses R19's live
// captured texture but submits it on a persistent LOCAL anchor instead of the
// old head-locked VIEW-space fallback, so F10 has a visible effect before the
// first 3D gameplay/car-select projection ever appears.

#include <openxr/openxr.h>
#include "vr/ipc/recenter_request.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

#ifdef xrPollEvent
#undef xrPollEvent
#endif
#ifdef xrEndFrame
#undef xrEndFrame
#endif

namespace OutRunVrR26RecenterHardening
{
    inline bool PendingFocusRecenter = false;
    inline XrSession PendingFocusSession = XR_NULL_HANDLE;
    inline std::atomic<LONG> PendingGameRequestId{ 0 };
    inline std::atomic<bool> PendingApplicationRecenter{ false };
    inline std::atomic<std::uint64_t> ApplicationSpaceGeneration{ 0 };
    inline std::atomic<std::uint64_t> PendingGameTargetGeneration{ 0 };
    inline bool FallbackAnchorValid = false;
    inline XrPosef FallbackAnchor{};
    inline std::uint64_t ReferenceChangesNormalized = 0;
    inline std::uint64_t FocusRecentersQueued = 0;
    inline std::uint64_t GameRequestsReceived = 0;
    inline std::uint64_t GameRequestsApplied = 0;
    inline std::uint64_t AnchoredStartupFallbacks = 0;
    inline bool FirstReferenceChangeLogged = false;
    inline bool FirstFocusRecenterLogged = false;
    inline bool FirstAnchoredFallbackLogged = false;

    inline XrVector3f RotateVector(const XrQuaternionf& q,
        const XrVector3f& v) noexcept
    {
        const XrVector3f u{ q.x, q.y, q.z };
        const XrVector3f t{
            2.0f * (u.y * v.z - u.z * v.y),
            2.0f * (u.z * v.x - u.x * v.z),
            2.0f * (u.x * v.y - u.y * v.x)
        };
        return {
            v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
            v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)
        };
    }

    inline void InvalidateFallbackAnchor() noexcept
    {
        FallbackAnchorValid = false;
        FallbackAnchor = {};
    }

    inline void QueueApplicationRecenter() noexcept
    {
        PendingApplicationRecenter.store(true, std::memory_order_release);
        InvalidateFallbackAnchor();
    }

    inline bool ApplicationRecenterAppliedForPendingGameRequest() noexcept
    {
        const std::uint64_t target =
            PendingGameTargetGeneration.load(std::memory_order_acquire);
        return target == 0 ||
            ApplicationSpaceGeneration.load(std::memory_order_acquire) >= target;
    }

    // Apply a real application-space recenter. The new LOCAL space is created
    // relative to the immutable runtime LOCAL origin at the current HMD pose,
    // making the current headset pose the application's new origin. Keeping the
    // original base LOCAL alive also makes repeated recenter requests stable.
    inline bool ApplyPendingApplicationRecenter(XrSession session,
        XrSpace viewSpace, XrSpace& localSpace, XrTime displayTime) noexcept
    {
        if (!PendingApplicationRecenter.load(std::memory_order_acquire))
            return true;
        const XrSpace base = OutRunVrFinalTest::BaseLocalSpace != XR_NULL_HANDLE
            ? OutRunVrFinalTest::BaseLocalSpace : localSpace;
        if (session == XR_NULL_HANDLE || viewSpace == XR_NULL_HANDLE ||
            base == XR_NULL_HANDLE || displayTime == 0)
            return false;

        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(::xrLocateSpace(viewSpace, base, displayTime, &location)))
            return false;
        constexpr XrSpaceLocationFlags need =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((location.locationFlags & need) != need)
            return false;

        XrReferenceSpaceCreateInfo create{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        create.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        create.poseInReferenceSpace = location.pose;
        XrSpace recentered = XR_NULL_HANDLE;
        if (XR_FAILED(::xrCreateReferenceSpace(session, &create, &recentered)) ||
            recentered == XR_NULL_HANDLE)
            return false;

        const XrSpace previous = localSpace;
        localSpace = recentered;
        OutRunVrFinalTest::LocalSpace = recentered;
        std::uint64_t generation =
            ApplicationSpaceGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (generation == 0)
            generation =
                ApplicationSpaceGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        PendingApplicationRecenter.store(false, std::memory_order_release);
        InvalidateFallbackAnchor();

        // Never destroy the immutable runtime LOCAL. Old application-created
        // recenter spaces can be retired immediately after all future locates
        // are redirected to the new handle.
        if (previous != XR_NULL_HANDLE && previous != base)
            ::xrDestroySpace(previous);

        std::cerr
            << "[R46 recenter] application LOCAL origin recreated generation="
            << generation << " current headset pose is now application center\n";
        return true;
    }

    inline bool EnsureFallbackAnchor(XrSession session,
        XrTime displayTime) noexcept
    {
        if (FallbackAnchorValid)
            return true;
        if (session == XR_NULL_HANDLE || displayTime == 0 ||
            OutRunVrFinalTest::LocalSpace == XR_NULL_HANDLE ||
            !OutRunVrSbsCaptureOverride::EnsureViewSpace(session) ||
            OutRunVrSbsCaptureOverride::ViewSpace == XR_NULL_HANDLE)
            return false;

        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(::xrLocateSpace(
                OutRunVrSbsCaptureOverride::ViewSpace,
                OutRunVrFinalTest::LocalSpace, displayTime, &location)))
            return false;
        constexpr XrSpaceLocationFlags need =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((location.locationFlags & need) != need)
            return false;

        FallbackAnchor = location.pose;
        const XrVector3f forward = RotateVector(
            location.pose.orientation, { 0.0f, 0.0f, -1.5f });
        FallbackAnchor.position.x += forward.x;
        FallbackAnchor.position.y += forward.y;
        FallbackAnchor.position.z += forward.z;
        FallbackAnchorValid = true;
        return true;
    }

    inline bool TrySubmitAnchoredStartupFallback(XrSession session,
        const XrFrameEndInfo* endInfo, XrResult& result) noexcept
    {
        if (!endInfo || endInfo->layerCount != 0 ||
            !EnsureFallbackAnchor(session, endInfo->displayTime))
            return false;

        XrCompositionLayerQuad quad{};
        if (!OutRunVrSbsCaptureOverride::RenderTheaterOverride(session, quad))
            return false;

        // R19 renders the correct live texture but normally returns a VIEW-space
        // head-locked quad. Keep its swapchain/size and replace only its spatial
        // interpretation with the current LOCAL anchor.
        quad.space = OutRunVrFinalTest::LocalSpace;
        quad.pose = FallbackAnchor;
        const XrCompositionLayerBaseHeader* layer =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        XrFrameEndInfo patched = *endInfo;
        patched.layerCount = 1;
        patched.layers = &layer;
        OutRunVrR23RuntimeHardening::RecordFinalSubmission(
            0, OutRunVrR23VerifiedBundle::SourceKind::None, true);
        result = OutRunVrFinalTest::EndFrame(session, &patched);
        if (XR_SUCCEEDED(result))
        {
            ++AnchoredStartupFallbacks;
            if (!FirstAnchoredFallbackLogged)
            {
                FirstAnchoredFallbackLogged = true;
                std::cerr
                    << "[R28 recenter] startup/menu no-layer fallback is now LOCAL-anchored and recenterable before first gameplay projection\n";
            }
        }
        return true;
    }

    inline void WriteSyntheticLocalChange(XrEventDataBuffer* eventData,
        XrSession session) noexcept
    {
        XrEventDataReferenceSpaceChangePending synthetic{
            XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING };
        synthetic.session = session;
        synthetic.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        synthetic.changeTime = 0;
        synthetic.poseValid = XR_FALSE;
        std::memset(eventData, 0, sizeof(*eventData));
        std::memcpy(eventData, &synthetic, sizeof(synthetic));
    }

    inline XrResult XRAPI_CALL PollEvent(XrInstance instance,
        XrEventDataBuffer* eventData) noexcept
    {
        if (eventData)
        {
            LONG requestId = 0;
            DWORD requesterPid = 0;
            auto& channel = OutRunVR::RecenterIpc::SharedChannel();
            if (channel.Pending(requestId, requesterPid))
            {
                // Received means only that the host accepted ownership. Applied
                // is deliberately deferred until EndFrame succeeds after the
                // LOCAL anchor invalidation/rebuild cycle.
                const std::uint64_t targetGeneration =
                    ApplicationSpaceGeneration.load(std::memory_order_acquire) + 1;
                PendingGameTargetGeneration.store(
                    targetGeneration, std::memory_order_release);
                QueueApplicationRecenter();
                WriteSyntheticLocalChange(eventData, XR_NULL_HANDLE);
                channel.MarkReceived(requestId);
                PendingGameRequestId.store(requestId, std::memory_order_release);
                ++GameRequestsReceived;
                std::cerr
                    << "[R28 recenter] F10 request received requestId="
                    << requestId << " pid=" << requesterPid
                    << "; pending until visible post-reanchor submission\n";
                return XR_SUCCESS;
            }
        }

        if (PendingFocusRecenter && eventData)
        {
            QueueApplicationRecenter();
            WriteSyntheticLocalChange(eventData, PendingFocusSession);
            PendingFocusRecenter = false;
            PendingFocusSession = XR_NULL_HANDLE;
            return XR_SUCCESS;
        }

        const XrResult result = ::xrPollEvent(instance, eventData);
        if (result != XR_SUCCESS || !eventData)
            return result;

        if (eventData->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {
            auto* change = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(
                eventData);

            // The existing host loop already has the correct invalidation and
            // re-anchor path; make every runtime reference-space change enter it.
            change->referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            InvalidateFallbackAnchor();
            ++ReferenceChangesNormalized;
            if (!FirstReferenceChangeLogged)
            {
                FirstReferenceChangeLogged = true;
                std::cerr
                    << "[R26 recenter] OpenXR reference-space changes now reanchor the theater regardless of runtime space type\n";
            }
        }
        else if (eventData->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto* state =
                reinterpret_cast<const XrEventDataSessionStateChanged*>(eventData);
            if (state->state == XR_SESSION_STATE_FOCUSED)
            {
                PendingFocusRecenter = true;
                PendingFocusSession = state->session;
                ++FocusRecentersQueued;
                if (!FirstFocusRecenterLogged)
                {
                    FirstFocusRecenterLogged = true;
                    std::cerr
                        << "[R26 recenter] session FOCUSED queues a theater reanchor for Quest/VDXR recenter and focus return\n";
                }
            }
        }

        return result;
    }

    inline const char* IncomingPath(const XrFrameEndInfo* endInfo) noexcept
    {
        if (!endInfo || endInfo->layerCount == 0 || !endInfo->layers)
            return "no-layer";
        for (std::uint32_t i = 0; i < endInfo->layerCount; ++i)
        {
            const auto* layer = endInfo->layers[i];
            if (!layer) continue;
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                return "projection";
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
                return "theater-quad";
        }
        return "other-layer";
    }

    inline bool CompletePendingGameRequestAfterVisibleProjection() noexcept
    {
        const LONG pending =
            PendingGameRequestId.load(std::memory_order_acquire);
        if (pending == 0 ||
            !ApplicationRecenterAppliedForPendingGameRequest())
            return false;

        auto& channel = OutRunVR::RecenterIpc::SharedChannel();
        channel.MarkApplied(pending);
        LONG expected = pending;
        const bool cleared = PendingGameRequestId.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel,
            std::memory_order_acquire);
        if (cleared)
        {
            ++GameRequestsApplied;
            PendingGameTargetGeneration.store(0, std::memory_order_release);
            InvalidateFallbackAnchor();
            std::cerr
                << "[R45 recenter] requestId=" << pending
                << " completed by fresh visible DirectGPU projection; fast path remained live\n";
        }
        return cleared;
    }

    inline XrResult XRAPI_CALL EndFrame(XrSession session,
        const XrFrameEndInfo* endInfo) noexcept
    {
        const LONG pending = PendingGameRequestId.load(std::memory_order_acquire);

        XrResult result = XR_SUCCESS;
        bool anchoredStartup = false;
        if (endInfo && endInfo->layerCount == 0)
            anchoredStartup = TrySubmitAnchoredStartupFallback(
                session, endInfo, result);

        const std::uint64_t liveBefore =
            OutRunVrR24BlackScreenGuard::LiveTheaterFallbacks;
        const std::uint64_t flatBefore =
            OutRunVrR24BlackScreenGuard::DirectFlatFallbacks;
        const std::uint64_t cachedBefore =
            OutRunVrR24BlackScreenGuard::CachedLayerFallbacks;
        const std::uint64_t emergencyBefore =
            OutRunVrR24BlackScreenGuard::EmergencyLayerFallbacks;

        if (!anchoredStartup)
            result = OutRunVrR24BlackScreenGuard::EndFrame(session, endInfo);

        const bool r24ViewFallback = !anchoredStartup &&
            (OutRunVrR24BlackScreenGuard::LiveTheaterFallbacks != liveBefore ||
             OutRunVrR24BlackScreenGuard::DirectFlatFallbacks != flatBefore ||
             OutRunVrR24BlackScreenGuard::CachedLayerFallbacks != cachedBefore ||
             OutRunVrR24BlackScreenGuard::EmergencyLayerFallbacks != emergencyBefore);

        if (pending != 0 && ApplicationRecenterAppliedForPendingGameRequest() &&
            XR_SUCCEEDED(result) && !r24ViewFallback &&
            (anchoredStartup || (endInfo && endInfo->layerCount > 0)))
        {
            auto& channel = OutRunVR::RecenterIpc::SharedChannel();
            channel.MarkApplied(pending);
            LONG expected = pending;
            PendingGameRequestId.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel,
                std::memory_order_acquire);
            ++GameRequestsApplied;
            PendingGameTargetGeneration.store(0, std::memory_order_release);
            std::cerr
                << "[R28 recenter] requestId=" << pending
                << " path=" << (anchoredStartup ?
                    "local-live-fallback" : IncomingPath(endInfo))
                << " anchorUpdated=1 submitSuccess=1 appliedId="
                << pending << "\n";
        }
        else if (pending != 0 && r24ViewFallback)
        {
            // Do not claim completion for a head-locked R24 emergency/fallback
            // quad. Keep the request pending until a LOCAL/projection path or the
            // anchored startup fallback is actually submitted.
            InvalidateFallbackAnchor();
        }

        return result;
    }
}

#define xrPollEvent OutRunVrR26RecenterHardening::PollEvent
#define xrEndFrame OutRunVrR26RecenterHardening::EndFrame
