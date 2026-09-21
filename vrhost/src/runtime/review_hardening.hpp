#pragma once

// Final runtime ownership policy after the source review passes.
//
// Classic D3D9:
//   1. The main host's frame/QPC/pose-matched projection is authoritative.
//   2. R10 Desktop Duplication is only a fallback when no exact projection was
//      produced and a NEW complete classic frame has advanced recently.
//   3. A stale Frame.v2 mapping can no longer refresh fallback freshness merely
//      because xrEndFrame keeps reading the same frame id.
//   4. Fallback freshness is additionally bounded by the producer's presentQpc;
//      first observing an old frame after a mode/session transition cannot make
//      that frame fresh again.
//   5. An incoming non-projection layer (notably the theater quad) is owned by
//      the main host and must never be replaced by an old gameplay SBS frame.
//
// D3D9Ex direct:
//   1. R13 host-owned SafeEye copies remain the only final projection source.
//   2. If safe staging/rendering fails, never split the desktop as SBS: the game
//      may have intentionally skipped ComposeSbs while direct transport was ready.
//   3. Fall back to the already-built incoming OpenXR layer (or no layer), while
//      keeping the producer slot untrusted rather than fabricating two eyes.

#include "openxr_api_compat.hpp"

#ifdef xrEndFrame
#undef xrEndFrame
#endif
#ifdef xrDestroySession
#undef xrDestroySession
#endif

#include <array>
#include <cstdint>
#include <iostream>

namespace OutRunVrReviewHardening
{
    inline constexpr const char* BuildId = "R14-review-hardening-20260915";
    inline constexpr const char* Review10BuildId = "R15-source-review-10pass-20260915";
    inline constexpr ULONGLONG ClassicFallbackFreshMs = 500;

    inline std::uint32_t LastClassicFrameId = 0;
    inline ULONGLONG LastClassicAdvanceMs = 0;
    inline std::uint64_t ExactCoreProjectionFrames = 0;
    inline std::uint64_t PreservedNonProjectionLayers = 0;
    inline std::uint64_t ClassicFallbackFrames = 0;
    inline std::uint64_t StaleClassicFallbackBlocks = 0;
    inline std::uint64_t DirectSafeFrames = 0;
    inline std::uint64_t DirectUnsafeFallbackBlocks = 0;
    inline bool FirstCoreAuthorityLogged = false;
    inline bool FirstNonProjectionPreserveLogged = false;
    inline bool FirstStaleBlockLogged = false;
    inline bool FirstDirectAuthorityLogged = false;
    inline bool FirstDirectBlockLogged = false;

    inline bool IncomingProjectionValid(const XrFrameEndInfo* endInfo) noexcept
    {
        return endInfo && endInfo->layerCount > 0 && endInfo->layers &&
            endInfo->layers[0] &&
            endInfo->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    }

    inline bool HasIncomingNonProjectionLayer(const XrFrameEndInfo* endInfo) noexcept
    {
        if (!endInfo || endInfo->layerCount == 0 || !endInfo->layers)
            return false;
        for (std::uint32_t i = 0; i < endInfo->layerCount; ++i)
        {
            const XrCompositionLayerBaseHeader* layer = endInfo->layers[i];
            if (layer && layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                return true;
        }
        return false;
    }

    inline bool ReadLatestFrame(OutRunVR::SharedRenderFrameState& frame) noexcept
    {
        std::uint32_t publish = 0;
        return OutRunVrFinalTest::ReadLatestFrame(frame, publish);
    }

    inline bool LatestCompleteDirectFrame(OutRunVR::SharedRenderFrameState& frame) noexcept
    {
        if (!ReadLatestFrame(frame) ||
            !OutRunVrSbsCaptureOverride::FrameComplete(frame))
            return false;
        return (frame.flags & OutRunVR::RenderFrameDirectGpuTransport) != 0;
    }

    inline bool ProducerPresentFresh(
        const OutRunVR::SharedRenderFrameState& frame,
        std::int64_t maxAgeMs = static_cast<std::int64_t>(ClassicFallbackFreshMs)) noexcept
    {
        if (frame.presentQpc <= 0 || maxAgeMs <= 0)
            return false;
        LARGE_INTEGER now{};
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) ||
            frequency.QuadPart <= 0)
            return false;
        const std::int64_t age = now.QuadPart - frame.presentQpc;
        const std::int64_t maxAge = (frequency.QuadPart * maxAgeMs) / 1000;
        return age >= 0 && age <= maxAge;
    }

    inline bool FreshClassicFallbackAvailable() noexcept
    {
        OutRunVR::SharedRenderFrameState frame{};
        if (!ReadLatestFrame(frame) ||
            !OutRunVrSbsCaptureOverride::FrameComplete(frame) ||
            (frame.flags & OutRunVR::RenderFrameDirectGpuTransport) != 0 ||
            !ProducerPresentFresh(frame))
            return false;

        const ULONGLONG now = GetTickCount64();
        if (frame.frameId != LastClassicFrameId)
        {
            LastClassicFrameId = frame.frameId;
            LastClassicAdvanceMs = now;
        }
        return LastClassicAdvanceMs != 0 &&
            now - LastClassicAdvanceMs <= ClassicFallbackFreshMs;
    }

    inline XrResult XRAPI_CALL EndFrame(XrSession session,
        const XrFrameEndInfo* endInfo)
    {
        // A quad (theater/menu) or any other non-projection layer is already a
        // deliberate main-host presentation decision. Never reinterpret a stale
        // gameplay Frame.v2 entry as authority over that layer.
        if (HasIncomingNonProjectionLayer(endInfo))
        {
            ++PreservedNonProjectionLayers;
            if (!FirstNonProjectionPreserveLogged)
            {
                FirstNonProjectionPreserveLogged = true;
                std::cerr
                    << "[R15] incoming non-projection layer preserved; classic/direct "
                       "gameplay fallback cannot replace theater/menu content build="
                    << Review10BuildId << "\n";
            }
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }

        OutRunVR::SharedRenderFrameState latest{};
        const bool latestDirect = LatestCompleteDirectFrame(latest);
        const auto directState =
            OutRunVrD3D9ExDirectPassthrough::ReadDirectHostState();
        const bool exactIncoming = IncomingProjectionValid(endInfo);
        const bool directCandidate = latestDirect && directState.valid &&
            directState.openedFrame != 0 && exactIncoming;

        if (latestDirect)
        {
            if (directCandidate &&
                OutRunVrD3D9ExDirectPassthrough::EnsureSafeFrame(
                    directState.openedFrame))
            {
                XrCompositionLayerProjection projection{};
                std::array<XrCompositionLayerProjectionView, 2> views{};
                if (OutRunVrD3D9ExDirectPassthrough::RenderSafeProjection(
                        session, endInfo, projection, views))
                {
                    const XrCompositionLayerBaseHeader* layer =
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                            &projection);
                    XrFrameEndInfo patched = *endInfo;
                    patched.layerCount = 1;
                    patched.layers = &layer;
                    ++DirectSafeFrames;
                    if (!FirstDirectAuthorityLogged)
                    {
                        FirstDirectAuthorityLogged = true;
                        std::cerr
                            << "[R14] D3D9Ex final authority: host-owned SafeEye projection; "
                               "desktop SBS fallback disabled for direct frames build="
                            << BuildId << "\n";
                    }
                    return OutRunVrFinalTest::EndFrame(session, &patched);
                }
            }

            ++DirectUnsafeFallbackBlocks;
            if (!FirstDirectBlockLogged)
            {
                FirstDirectBlockLogged = true;
                std::cerr
                    << "[R14] direct frame could not be staged/rendered safely; "
                       "refusing R10 desktop split and preserving incoming layer/no-layer\n";
            }
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }

        if (exactIncoming)
        {
            ++ExactCoreProjectionFrames;
            if (!FirstCoreAuthorityLogged)
            {
                FirstCoreAuthorityLogged = true;
                std::cerr
                    << "[R14] classic D3D9 exact core projection is authoritative; "
                       "R10 recapture will not overwrite a matched frame build="
                    << BuildId << "\n";
            }
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }

        if (FreshClassicFallbackAvailable())
        {
            ++ClassicFallbackFrames;
            return OutRunVrSbsCaptureOverride::EndFrame(session, endInfo);
        }

        ++StaleClassicFallbackBlocks;
        if (!FirstStaleBlockLogged && LastClassicFrameId != 0)
        {
            FirstStaleBlockLogged = true;
            std::cerr
                << "[R15] stale classic SBS frame blocked; fallback requires both a new "
                   "Frame.v2 id and producer presentQpc freshness build="
                << Review10BuildId << "\n";
        }
        return OutRunVrFinalTest::EndFrame(session, endInfo);
    }

    inline XrResult XRAPI_CALL DestroySession(XrSession session)
    {
        LastClassicFrameId = 0;
        LastClassicAdvanceMs = 0;
        return OutRunVrD3D9ExDirectPassthrough::DestroySession(session);
    }
}

#define xrEndFrame OutRunVrReviewHardening::EndFrame
#define xrDestroySession OutRunVrReviewHardening::DestroySession