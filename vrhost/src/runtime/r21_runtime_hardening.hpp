#pragma once

// R21 runtime hardening.
//
// Goals:
//  1. Never issue a write-capable interlocked operation against the read-only
//     SharedPoseState mapping used by the x64 host.
//  2. Do not touch direct-transport state at all when the current frame is not
//     a direct frame (or direct transport is disabled).
//  3. Keep the existing R15 ownership policy, but make the final xrEndFrame
//     branch explicit and bind the legacy R19 blit constant buffer to VS before
//     any R19/direct fallback rendering.
//  4. Source-review hardening: never synthesize a fallback layer when the host
//     says shouldRender=false, and require the opened direct frame to match the
//     exact latest direct frame before safe staging/presentation.
//
// The main StereoCompositor shader is fixed separately so its UV transform is
// evaluated in PS (where the constant buffer is already bound).

#include "review_hardening.hpp"

#ifdef xrEndFrame
#undef xrEndFrame
#endif
#ifdef xrDestroySession
#undef xrDestroySession
#endif

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace OutRunVrR21RuntimeHardening
{
    inline constexpr const char* BuildId = "R21-readonly-ipc-vs-cbuffer-20260916";
    inline constexpr const char* ReviewFixBuildId = "R22-source-review-fixes-20260916";

    inline bool FirstActiveLogged = false;
    inline bool FirstReadonlyDirectReadLogged = false;
    inline bool FirstLegacyVsBindingLogged = false;
    inline bool FirstDirectDisabledSkipLogged = false;
    inline bool FirstShouldRenderBlockLogged = false;
    inline bool FirstDirectExactFrameBlockLogged = false;

    inline bool DirectTransportRequested() noexcept
    {
        const char* value = std::getenv("OUTRUN_VR_DIRECT_TRANSPORT");
        return !value || (std::strcmp(value, "0") != 0 &&
            _stricmp(value, "false") != 0 && _stricmp(value, "off") != 0);
    }

    inline bool HostShouldRenderReadonly() noexcept
    {
        using namespace OutRunVrD3D9ExDirectPassthrough;
        if (!EnsurePoseState())
            return false;

        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::uint32_t before = PoseState->sequence;
            if (before & 1u)
                continue;
            MemoryBarrier();
            const std::uint32_t flags = PoseState->flags;
            const std::uint32_t hostPid = PoseState->hostPid;
            MemoryBarrier();
            const std::uint32_t after = PoseState->sequence;
            if (before != after || (after & 1u))
                continue;

            constexpr std::uint32_t required =
                OutRunVR::HostAlive |
                OutRunVR::SessionVisible |
                OutRunVR::HostShouldRender;
            return hostPid == GetCurrentProcessId() &&
                (flags & required) == required;
        }
        return false;
    }

    inline OutRunVrD3D9ExDirectPassthrough::DirectHostState
        ReadDirectHostStateReadonly() noexcept
    {
        using namespace OutRunVrD3D9ExDirectPassthrough;
        DirectHostState out{};
        if (!EnsurePoseState())
            return out;

        // SharedPoseState is mapped FILE_MAP_READ. Use only aligned volatile
        // 32-bit loads under the existing sequence lock; no const_cast and no
        // InterlockedCompareExchange are permitted on this view.
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::uint32_t before = PoseState->sequence;
            if (before & 1u)
                continue;
            MemoryBarrier();
            out.flags = PoseState->flags;
            out.hostPid = PoseState->hostPid;
            out.openedFrame = PoseState->hostDirectConsumedFrameId;
            MemoryBarrier();
            const std::uint32_t after = PoseState->sequence;
            if (before != after || (after & 1u))
                continue;

            constexpr std::uint32_t required =
                OutRunVR::HostAlive |
                OutRunVR::HostDirectGpuTransport |
                OutRunVR::HostDirectGpuReady;
            out.valid = out.hostPid == GetCurrentProcessId() &&
                (out.flags & required) == required;

            if (!FirstReadonlyDirectReadLogged)
            {
                FirstReadonlyDirectReadLogged = true;
                std::cerr
                    << "[R21] direct host state uses read-only seqlock load; "
                       "write-capable CAS removed from final presentation path\n";
            }
            return out;
        }
        return {};
    }

    inline bool BindLegacyBlitConstantBufferToVs() noexcept
    {
        using namespace OutRunVrSbsCaptureOverride;
        if (!OutRunVrFinalTest::Context || !CreateShaders() || !ConstantBuffer)
            return false;
        OutRunVrFinalTest::Context->VSSetConstantBuffers(0, 1, &ConstantBuffer);
        if (!FirstLegacyVsBindingLogged)
        {
            FirstLegacyVsBindingLogged = true;
            std::cerr
                << "[R21] legacy R19/direct blit constant buffer bound to VS+PS; "
                   "UVScale/UvOffset now reach VSMain\n";
        }
        return true;
    }

    inline XrResult XRAPI_CALL EndFrame(XrSession session,
        const XrFrameEndInfo* endInfo)
    {
        if (!FirstActiveLogged)
        {
            FirstActiveLogged = true;
            std::cerr
                << "[R21] final OpenXR presentation owner ACTIVE build=" << BuildId
                << " reviewFix=" << ReviewFixBuildId
                << "; readonly IPC + deterministic classic/direct presentation policy\n";
        }

        // Menu/theater content is already a deliberate core-compositor layer.
        // Preserve it exactly as R15 did.
        if (OutRunVrReviewHardening::HasIncomingNonProjectionLayer(endInfo))
            return OutRunVrFinalTest::EndFrame(session, endInfo);

        // Main.cpp intentionally submits zero layers when OpenXR says
        // shouldRender=false. Do not turn that deliberate no-layer frame into a
        // stale desktop/direct fallback merely because Frame.v2 is still fresh.
        if (!HostShouldRenderReadonly())
        {
            if (!FirstShouldRenderBlockLogged)
            {
                FirstShouldRenderBlockLogged = true;
                std::cerr
                    << "[R22] host shouldRender/session-visible gate closed; preserving incoming/no-layer frame without fallback synthesis\n";
            }
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }

        OutRunVR::SharedRenderFrameState latest{};
        const bool latestDirect =
            OutRunVrReviewHardening::LatestCompleteDirectFrame(latest);
        const bool directRequested = DirectTransportRequested();

        // Critical R21 ordering: when this is a classic frame, do not even map
        // or read direct-only state.
        if (latestDirect && directRequested)
        {
            const auto directState = ReadDirectHostStateReadonly();
            const bool exactIncoming =
                OutRunVrReviewHardening::IncomingProjectionValid(endInfo);
            const bool directCandidate = directState.valid && exactIncoming &&
                directState.openedFrame != 0 &&
                directState.openedFrame == latest.frameId;

            if (directCandidate &&
                OutRunVrD3D9ExDirectPassthrough::EnsureSafeFrame(latest.frameId))
            {
                // RenderSafeProjection reuses R19 RenderTo(). Bind its shared
                // BlitParams buffer to VS before the draw; RenderTo already binds
                // the same buffer to PS.
                BindLegacyBlitConstantBufferToVs();
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
                    return OutRunVrFinalTest::EndFrame(session, &patched);
                }
            }

            if (!directCandidate && !FirstDirectExactFrameBlockLogged)
            {
                FirstDirectExactFrameBlockLogged = true;
                std::cerr
                    << "[R22] direct frame rejected because host-opened frame is not the exact latest direct frame; stale Ready/openedFrame state cannot present an older eye pair\n";
            }

            // A frame explicitly marked as direct must never be reinterpreted as
            // desktop SBS if safe direct staging/rendering is unavailable.
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }

        if (latestDirect && !directRequested)
        {
            if (!FirstDirectDisabledSkipLogged)
            {
                FirstDirectDisabledSkipLogged = true;
                std::cerr
                    << "[R21] direct frame observed while direct transport disabled; "
                       "direct-only IPC skipped and incoming layer preserved\n";
            }
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }

        // A valid core projection is authoritative for classic D3D9.
        if (OutRunVrReviewHardening::IncomingProjectionValid(endInfo))
            return OutRunVrFinalTest::EndFrame(session, endInfo);

        // Only a fresh, complete classic frame may enter the R19 Desktop
        // Duplication fallback. Ensure its VS sees BlitParams first.
        if (OutRunVrReviewHardening::FreshClassicFallbackAvailable())
        {
            BindLegacyBlitConstantBufferToVs();
            return OutRunVrSbsCaptureOverride::EndFrame(session, endInfo);
        }

        return OutRunVrFinalTest::EndFrame(session, endInfo);
    }

    inline XrResult XRAPI_CALL DestroySession(XrSession session)
    {
        // Preserve the established cleanup chain for R13 direct resources,
        // R19 swapchains/capture snapshots and the base OpenXR session state.
        return OutRunVrReviewHardening::DestroySession(session);
    }
}

#define xrEndFrame OutRunVrR21RuntimeHardening::EndFrame
#define xrDestroySession OutRunVrR21RuntimeHardening::DestroySession
