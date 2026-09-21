#pragma once

// R24 final visible-presentation guard.
//
// R23 deliberately fails closed to layerCount=0 whenever a verified
// frame/source/pose bundle cannot be proven. That is safe for pose/texture
// ownership, but on a headset it is indistinguishable from a broken renderer.
// R24 keeps the same stereo-safety rules while adding a visible degradation
// ladder:
//   1. preserve validated classic projection + non-projection layers;
//   2. render DirectGPU only from host-owned SafeEye copies and completion ACK;
//   3. allow a short display-only grace for an already committed bundle;
//   4. prefer the last released projection image, then a host-owned Direct
//      SafeEye flat view, then a left-eye-only theater recovery capture, then an
//      emergency visible quad instead of ever exposing the raw SBS desktop.
//
// The display-only grace never reopens game-side WVP/stereo injection. It only
// prevents a 250ms boundary race from turning a frame that was already rendered
// from a committed source into a black OpenXR frame.

#include "r23_runtime_hardening.hpp"

#ifdef xrEndFrame
#undef xrEndFrame
#endif
#ifdef xrDestroySession
#undef xrDestroySession
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>

namespace OutRunVrR24BlackScreenGuard
{
    // Keep the production marker stable because release CI and field logs use it
    // to prove that the final R24 owner is present in the shipped host binary.
    inline constexpr const char* BuildId =
        "R24-visible-fallback-final-20260916";
    inline constexpr ULONGLONG DisplayOnlyGraceMs =
        static_cast<ULONGLONG>(
            OutRunVrR23VerifiedBundle::MaxPresentationAgeMs);

    inline std::uint64_t ExactProjectionSubmits = 0;
    inline std::uint64_t SoftGraceProjectionSubmits = 0;
    inline std::uint64_t DirectSafeProjectionSubmits = 0;
    inline std::uint64_t LiveTheaterFallbacks = 0;
    inline std::uint64_t DirectFlatFallbacks = 0;
    inline std::uint64_t CachedLayerFallbacks = 0;
    inline std::uint64_t EmergencyLayerFallbacks = 0;
    inline std::uint64_t EmptyFrameFallbacks = 0;
    inline std::uint64_t MixedValidatedSubmits = 0;
    inline std::uint64_t IntentionalMonoProjectionSubmits = 0;
    inline std::atomic<bool> IntentionalMonoProjection{ false };

    inline bool FirstExactProjectionLogged = false;
    inline bool FirstSoftGraceLogged = false;
    inline bool FirstDirectSafeLogged = false;
    inline bool FirstLiveTheaterLogged = false;
    inline bool FirstDirectFlatLogged = false;
    inline bool FirstCachedLayerLogged = false;
    inline bool FirstEmergencyLayerLogged = false;
    inline bool FirstEmptyFallbackLogged = false;
    inline bool FirstMixedValidatedLogged = false;

    // A swapchain handle alone does not prove that any image was ever rendered
    // and successfully released. Track only images R24 itself has committed.
    inline std::uint64_t ProjectionCommittedGeneration = 0;
    inline std::uint64_t TheaterCommittedGeneration = 0;

    struct ProjectionSelection
    {
        const XrCompositionLayerBaseHeader* header = nullptr;
        std::uint32_t count = 0;
    };

    inline ProjectionSelection FindProjection(
        const XrFrameEndInfo* endInfo) noexcept
    {
        ProjectionSelection out{};
        if (!endInfo || !endInfo->layers)
            return out;
        for (std::uint32_t i = 0; i < endInfo->layerCount; ++i)
        {
            const auto* layer = endInfo->layers[i];
            if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            {
                if (!out.header)
                    out.header = layer;
                ++out.count;
            }
        }
        return out;
    }

    inline bool ProjectionMatchesSnapshot(
        const XrFrameEndInfo* endInfo,
        const OutRunVrR23VerifiedBundle::Snapshot& snapshot) noexcept
    {
        const auto projection = FindProjection(endInfo);
        if (!projection.header || projection.count != 1)
            return false;
        const XrCompositionLayerBaseHeader* oneLayer = projection.header;
        XrFrameEndInfo single = *endInfo;
        single.layerCount = 1;
        single.layers = &oneLayer;
        return OutRunVrR23RuntimeHardening::IncomingPoseMatchesCommittedFrame(
            &single, snapshot.frame);
    }

    inline bool ReadDisplayGraceSnapshot(
        OutRunVrR23VerifiedBundle::Snapshot& out) noexcept
    {
        using namespace OutRunVrR23VerifiedBundle;
        if (!Read(out) || out.kind == SourceKind::None || !out.frameId ||
            out.frame.frameId != out.frameId ||
            out.frame.sourcePoseSequence != out.poseSequence ||
            out.frame.presentQpc != out.presentQpc ||
            !OutRunVrSbsCaptureOverride::FrameComplete(out.frame) ||
            !out.publishedAtMs)
            return false;
        const ULONGLONG now = GetTickCount64();
        return now >= out.publishedAtMs &&
            now - out.publishedAtMs <= DisplayOnlyGraceMs;
    }

    inline XrResult SubmitOriginal(
        XrSession session, const XrFrameEndInfo* endInfo,
        const OutRunVrR23VerifiedBundle::Snapshot& snapshot,
        bool softGrace = false) noexcept
    {
        OutRunVrR23RuntimeHardening::RecordFinalSubmission(
            snapshot.frameId, snapshot.kind, true);
        if (softGrace)
        {
            ++SoftGraceProjectionSubmits;
            if (!FirstSoftGraceLogged)
            {
                FirstSoftGraceLogged = true;
                std::cerr
                    << "[R24] display-only grace accepted an already-rendered committed classic projection; game-side stereo eligibility remains unchanged build="
                    << BuildId << "\n";
            }
        }
        else
        {
            ++ExactProjectionSubmits;
            if (!FirstExactProjectionLogged)
            {
                FirstExactProjectionLogged = true;
                std::cerr
                    << "[R24] exact committed classic projection submitted directly; no redundant black-frame gate build="
                    << BuildId << "\n";
            }
        }
        return OutRunVrFinalTest::EndFrame(session, endInfo);
    }

    inline void BuildViewQuad(XrSwapchain handle, std::uint32_t width,
        std::uint32_t height, std::uint32_t arrayIndex,
        XrCompositionLayerQuad& quad) noexcept
    {
        using namespace OutRunVrSbsCaptureOverride;
        quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.space = ViewSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.pose.orientation.w = 1.0f;
        quad.pose.position.z = -1.5f;
        quad.subImage.swapchain = handle;
        quad.subImage.imageRect.offset = { 0, 0 };
        quad.subImage.imageRect.extent = {
            static_cast<std::int32_t>(width),
            static_cast<std::int32_t>(height)
        };
        quad.subImage.imageArrayIndex = arrayIndex;
        const float aspect = height ? static_cast<float>(width) /
            static_cast<float>(height) : (16.0f / 9.0f);
        quad.size.width = 2.0f;
        quad.size.height = 2.0f / std::max(0.5f, aspect);
    }

    // R13's legacy RenderSafeProjection did not propagate xrReleaseSwapchainImage
    // failure. R24 owns the final visible path, so duplicate the small blit here
    // and treat acquire/wait/render/release as one transaction.
    inline bool RenderSafeProjectionChecked(
        XrSession session, const XrFrameEndInfo* endInfo,
        XrCompositionLayerProjection& projection,
        std::array<XrCompositionLayerProjectionView, 2>& views) noexcept
    {
        using namespace OutRunVrSbsCaptureOverride;
        using namespace OutRunVrD3D9ExDirectPassthrough;

        const auto selected = FindProjection(endInfo);
        if (!selected.header || selected.count != 1 ||
            !SafeEyeSrv[0] || !SafeEyeSrv[1])
            return false;
        const auto* incoming =
            reinterpret_cast<const XrCompositionLayerProjection*>(selected.header);
        if (incoming->viewCount < 2 || !incoming->views)
            return false;

        const std::uint32_t width = std::max(
            1, incoming->views[0].subImage.imageRect.extent.width);
        const std::uint32_t height = std::max(
            1, incoming->views[0].subImage.imageRect.extent.height);
        if (!EnsureSwapchain(Projection, session, width, height, 2) ||
            !CreateShaders())
            return false;

        std::uint32_t image = 0;
        if (!Acquire(Projection, image))
            return false;
        if (image >= Projection.rtvs.size())
        {
            Release(Projection);
            return false;
        }

        ID3D11ShaderResourceView* savedSrv = SourceSrv;
        const DXGI_FORMAT savedFormat = SourceFormat;
        bool ok = true;
        for (int eye = 0; eye < 2; ++eye)
        {
            SourceSrv = SafeEyeSrv[eye];
            SourceFormat = SafeEyeFormat;
            ok = RenderTo(Projection.rtvs[image][eye], Projection.width,
                Projection.height, UvRect{ 0.f, 0.f, 1.f, 1.f }) && ok;
        }
        SourceSrv = savedSrv;
        SourceFormat = savedFormat;
        if (OutRunVrFinalTest::Context)
            OutRunVrFinalTest::Context->Flush();

        const bool released = Release(Projection);
        if (!ok || !released)
            return false;

        ProjectionCommittedGeneration = OutRunVrSbsCaptureOverride::Projection.generation;
        projection = *incoming;
        for (int eye = 0; eye < 2; ++eye)
        {
            views[eye] = incoming->views[eye];
            views[eye].subImage.swapchain = Projection.handle;
            views[eye].subImage.imageRect.offset = { 0, 0 };
            views[eye].subImage.imageRect.extent = {
                static_cast<std::int32_t>(Projection.width),
                static_cast<std::int32_t>(Projection.height)
            };
            views[eye].subImage.imageArrayIndex = eye;
        }
        projection.viewCount = 2;
        projection.views = views.data();
        return true;
    }

    inline bool TryBuildDirectSafeProjection(
        XrSession session, const XrFrameEndInfo* endInfo,
        const OutRunVrR23VerifiedBundle::Snapshot& snapshot,
        XrCompositionLayerProjection& projection,
        std::array<XrCompositionLayerProjectionView, 2>& views) noexcept
    {
        using namespace OutRunVrD3D9ExDirectPassthrough;
        if (snapshot.kind != OutRunVrR23VerifiedBundle::SourceKind::DirectGpu ||
            !ProjectionMatchesSnapshot(endInfo, snapshot) ||
            !OutRunVrR21RuntimeHardening::DirectTransportRequested())
            return false;

        const auto state =
            OutRunVrR21RuntimeHardening::ReadDirectHostStateReadonly();
        if (!OutRunVrR22RuntimeHardening::DirectOpenMatchesLatest(
                state, snapshot.frame))
            return false;

        const auto generation = snapshot.frame.reserved[
            OutRunVR::RenderFrameDirectGenerationIndex];
        const bool safeAlreadyOwned =
            SafeFrameId == snapshot.frameId &&
            SafeTransportGeneration == generation &&
            SafeEyeSrv[0] && SafeEyeSrv[1];
        if (!safeAlreadyOwned && !EnsureSafeFrame(snapshot.frameId))
            return false;
        if (!OutRunVrR23RuntimeHardening::DirectSafeEyeMatchesCommittedFrame(
                snapshot.frame))
            return false;

        if (!OutRunVrR21RuntimeHardening::BindLegacyBlitConstantBufferToVs())
            return false;
        return RenderSafeProjectionChecked(session, endInfo, projection, views);
    }

    inline bool TrySubmitDirectSafeProjection(
        XrSession session, const XrFrameEndInfo* endInfo,
        const OutRunVrR23VerifiedBundle::Snapshot& snapshot,
        XrResult& result) noexcept
    {
        XrCompositionLayerProjection projection{};
        std::array<XrCompositionLayerProjectionView, 2> views{};
        if (!TryBuildDirectSafeProjection(
                session, endInfo, snapshot, projection, views))
            return false;

        const XrCompositionLayerBaseHeader* layer =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo patched = *endInfo;
        patched.layerCount = 1;
        patched.layers = &layer;
        OutRunVrR23RuntimeHardening::RecordFinalSubmission(
            snapshot.frameId, snapshot.kind, true);
        ++DirectSafeProjectionSubmits;
        if (!FirstDirectSafeLogged)
        {
            FirstDirectSafeLogged = true;
            std::cerr
                << "[R24] DirectGPU final submit uses host-owned SafeEye + checked acquire/wait/render/release transaction build="
                << BuildId << "\n";
        }
        result = OutRunVrFinalTest::EndFrame(session, &patched);
        return true;
    }

    inline bool RenderDirectFlatFallback(
        XrSession session, XrCompositionLayerQuad& quad) noexcept
    {
        using namespace OutRunVrSbsCaptureOverride;
        using namespace OutRunVrD3D9ExDirectPassthrough;

        if (!SafeFrameId || !SafeEyeSrv[0] || !SafeEyeWidth || !SafeEyeHeight ||
            !EnsureViewSpace(session) || !CreateShaders() ||
            !EnsureSwapchain(Theater, session, 1920, 1080, 1))
            return false;

        std::uint32_t image = 0;
        if (!Acquire(Theater, image))
            return false;
        if (image >= Theater.rtvs.size())
        {
            Release(Theater);
            return false;
        }

        ID3D11ShaderResourceView* savedSrv = SourceSrv;
        const DXGI_FORMAT savedFormat = SourceFormat;
        SourceSrv = SafeEyeSrv[0];
        SourceFormat = SafeEyeFormat;
        const bool ok = RenderTo(Theater.rtvs[image][0], Theater.width,
            Theater.height, UvRect{ 0.f, 0.f, 1.f, 1.f });
        SourceSrv = savedSrv;
        SourceFormat = savedFormat;
        if (OutRunVrFinalTest::Context)
            OutRunVrFinalTest::Context->Flush();
        const bool released = Release(Theater);
        if (!ok || !released)
            return false;

        TheaterCommittedGeneration = OutRunVrSbsCaptureOverride::Theater.generation;
        BuildViewQuad(Theater.handle, Theater.width, Theater.height, 0, quad);
        const float aspect = SafeEyeHeight
            ? static_cast<float>(SafeEyeWidth) / static_cast<float>(SafeEyeHeight)
            : (16.0f / 9.0f);
        quad.size.height = quad.size.width / std::max(0.5f, aspect);
        return true;
    }

    inline bool BuildCachedVisibleQuad(
        XrSession session, XrCompositionLayerQuad& quad) noexcept
    {
        using namespace OutRunVrSbsCaptureOverride;
        if (!EnsureViewSpace(session))
            return false;

        // Never prefer a previously committed theater image over a
        // released projection image. The theater source may be a desktop SBS
        // capture, while Projection is already separated per eye.
        if (Projection.handle != XR_NULL_HANDLE && Projection.width &&
            Projection.height && Projection.generation != 0 &&
            Projection.committedGeneration == Projection.generation &&
            ProjectionCommittedGeneration == Projection.generation)
        {
            BuildViewQuad(Projection.handle, Projection.width, Projection.height, 0, quad);
            return true;
        }
        if (Theater.handle != XR_NULL_HANDLE && Theater.width && Theater.height &&
            Theater.generation != 0 &&
            Theater.committedGeneration == Theater.generation &&
            TheaterCommittedGeneration == Theater.generation)
        {
            BuildViewQuad(Theater.handle, Theater.width, Theater.height, 0, quad);
            return true;
        }
        return false;
    }

    inline bool BuildEmergencyVisibleQuad(
        XrSession session, XrCompositionLayerQuad& quad) noexcept
    {
        using namespace OutRunVrSbsCaptureOverride;
        if (!OutRunVrFinalTest::Context || !EnsureViewSpace(session) ||
            !EnsureSwapchain(Theater, session, 1920, 1080, 1))
            return false;

        std::uint32_t image = 0;
        if (!Acquire(Theater, image))
            return false;
        if (image >= Theater.rtvs.size())
        {
            Release(Theater);
            return false;
        }

        const float visibleError[4]{ 0.12f, 0.025f, 0.025f, 1.0f };
        OutRunVrFinalTest::Context->ClearRenderTargetView(
            Theater.rtvs[image][0], visibleError);
        OutRunVrFinalTest::Context->Flush();
        if (!Release(Theater))
            return false;

        TheaterCommittedGeneration = OutRunVrSbsCaptureOverride::Theater.generation;
        BuildViewQuad(Theater.handle, Theater.width, Theater.height, 0, quad);
        return true;
    }

    inline XrResult SubmitVisibleFallback(
        XrSession session, const XrFrameEndInfo* endInfo,
        const char* reason) noexcept
    {
        XrCompositionLayerQuad quad{};

        // Degradation order is stereo-safe first: keep a released image
        // before attempting any new desktop/theater capture. This keeps a short
        // capture gap from exposing the monitor's raw SBS layout in the HMD.
        const bool cached = BuildCachedVisibleQuad(session, quad);
        const bool directFlat = !cached && RenderDirectFlatFallback(session, quad);

        // The legacy R19 shader consumes UVScale/UvOffset in VSMain. Do not
        // depend on inherited D3D11 state when R24 invokes it directly.
        OutRunVrR21RuntimeHardening::BindLegacyBlitConstantBufferToVs();
        const bool live = !cached && !directFlat &&
            OutRunVrSbsCaptureOverride::RenderTheaterOverride(session, quad);
        if (live)
            TheaterCommittedGeneration = OutRunVrSbsCaptureOverride::Theater.generation;

        const bool emergency = !cached && !directFlat && !live &&
            BuildEmergencyVisibleQuad(session, quad);

        if (!cached && !directFlat && !live && !emergency)
        {
            ++EmptyFrameFallbacks;
            if (!FirstEmptyFallbackLogged)
            {
                FirstEmptyFallbackLogged = true;
                std::cerr
                    << "[R24] every visible fallback failed; submitting no layer reason="
                    << reason << " build=" << BuildId << "\n";
            }
            OutRunVrR23RuntimeHardening::RecordFinalSubmission(
                0, OutRunVrR23VerifiedBundle::SourceKind::None, false);
            return OutRunVrR23RuntimeHardening::SubmitNoLayer(session, endInfo);
        }

        const XrCompositionLayerBaseHeader* layer =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        XrFrameEndInfo patched = *endInfo;
        patched.layerCount = 1;
        patched.layers = &layer;
        OutRunVrR23RuntimeHardening::RecordFinalSubmission(
            0, OutRunVrR23VerifiedBundle::SourceKind::None, true);

        if (cached)
        {
            ++CachedLayerFallbacks;
            if (!FirstCachedLayerLogged)
            {
                FirstCachedLayerLogged = true;
                std::cerr
                    << "[R24] last successfully released projection/theater image preserved visibility while stereo recovers reason="
                    << reason << " build=" << BuildId << "\n";
            }
        }
        else if (directFlat)
        {
            ++DirectFlatFallbacks;
            if (!FirstDirectFlatLogged)
            {
                FirstDirectFlatLogged = true;
                std::cerr
                    << "[R24] host-owned Direct SafeEye shown as a flat view while stereo projection recovers reason="
                    << reason << " build=" << BuildId << "\n";
            }
        }
        else if (live)
        {
            ++LiveTheaterFallbacks;
            if (!FirstLiveTheaterLogged)
            {
                FirstLiveTheaterLogged = true;
                std::cerr
                    << "[R24] live theater fallback replaced an unsafe/absent gameplay projection instead of submitting black reason="
                    << reason << " build=" << BuildId << "\n";
            }
        }

        else
        {
            ++EmergencyLayerFallbacks;
            if (!FirstEmergencyLayerLogged)
            {
                FirstEmergencyLayerLogged = true;
                std::cerr
                    << "[R24] emergency visible quad submitted because no game image was safely displayable; black/no-layer loop avoided reason="
                    << reason << " build=" << BuildId << "\n";
            }
        }
        return OutRunVrFinalTest::EndFrame(session, &patched);
    }

    inline XrResult XRAPI_CALL EndFrame(
        XrSession session, const XrFrameEndInfo* endInfo) noexcept
    {
        using OutRunVrR23VerifiedBundle::SourceKind;

        if (!endInfo || !OutRunVrR21RuntimeHardening::HostShouldRenderReadonly())
            return OutRunVrR23RuntimeHardening::EndFrame(session, endInfo);

        const auto projection = FindProjection(endInfo);
        const bool hasNonProjection =
            OutRunVrReviewHardening::HasIncomingNonProjectionLayer(endInfo);

        // Menus use one full mono desktop image in both projection eyes so VDXR
        // stays on its normal projection cadence instead of the slower theater
        // quad path. R23 arms this only for PresentationTheater/menu frames.
        if (IntentionalMonoProjection.load(std::memory_order_acquire) &&
            projection.count == 1 && !hasNonProjection)
        {
            ++IntentionalMonoProjectionSubmits;
            OutRunVrR23RuntimeHardening::RecordFinalSubmission(
                0, SourceKind::None, true);
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }

        // Theater/menu is already visible and deliberate; do not replace it.
        if (hasNonProjection && projection.count == 0)
        {
            OutRunVrR23RuntimeHardening::RecordFinalSubmission(
                0, SourceKind::None, endInfo->layerCount > 0);
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }

        OutRunVrR23VerifiedBundle::Snapshot verified{};
        const bool exactBundle =
            OutRunVrR23VerifiedBundle::ReadFresh(verified) &&
            OutRunVrSbsCaptureOverride::FrameComplete(verified.frame);

        // Mixed classic layers are valid when the single projection independently
        // matches the committed bundle. DirectGPU must still go through SafeEye.
        if (hasNonProjection && projection.count == 1 && exactBundle &&
            verified.kind == SourceKind::ClassicSbs &&
            ProjectionMatchesSnapshot(endInfo, verified))
        {
            ++MixedValidatedSubmits;
            if (!FirstMixedValidatedLogged)
            {
                FirstMixedValidatedLogged = true;
                std::cerr
                    << "[R24] mixed classic frame preserved after independent projection bundle validation build="
                    << BuildId << "\n";
            }
            OutRunVrR23RuntimeHardening::RecordFinalSubmission(
                verified.frameId, verified.kind, true);
            return OutRunVrFinalTest::EndFrame(session, endInfo);
        }
        if (hasNonProjection)
            return OutRunVrR23RuntimeHardening::SubmitNonProjectionOnly(
                session, endInfo);

        if (exactBundle)
        {
            if (verified.kind == SourceKind::DirectGpu)
            {
                XrResult directResult = XR_SUCCESS;
                if (TrySubmitDirectSafeProjection(
                        session, endInfo, verified, directResult))
                    return directResult;
                return SubmitVisibleFallback(
                    session, endInfo, "direct-safe-copy/render rejected");
            }

            if (verified.kind == SourceKind::ClassicSbs &&
                ProjectionMatchesSnapshot(endInfo, verified))
                return SubmitOriginal(session, endInfo, verified, false);

            const bool exactClassicSource =
                verified.kind == SourceKind::ClassicSbs &&
                verified.sourceCaptureQpc > 0 &&
                OutRunVrSbsCaptureOverride::LastProductionPresentQpc ==
                    verified.sourceCaptureQpc;
            if (exactClassicSource &&
                OutRunVrReviewHardening::FreshClassicFallbackAvailable())
            {
                OutRunVrR21RuntimeHardening::BindLegacyBlitConstantBufferToVs();
                OutRunVrR23RuntimeHardening::RecordFinalSubmission(
                    verified.frameId, verified.kind, true);
                return OutRunVrSbsCaptureOverride::EndFrame(session, endInfo);
            }

            return SubmitVisibleFallback(
                session, endInfo, "exact bundle has no safe projection source");
        }

        // If the core rendered a projection just before the 250ms R23 TTL
        // boundary, allow only that already-rendered pose-matched source for
        // another short interval. DirectGPU still performs SafeEye copy + ACK.
        OutRunVrR23VerifiedBundle::Snapshot grace{};
        if (ReadDisplayGraceSnapshot(grace))
        {
            if (grace.kind == SourceKind::DirectGpu)
            {
                XrResult directResult = XR_SUCCESS;
                if (TrySubmitDirectSafeProjection(
                        session, endInfo, grace, directResult))
                {
                    ++SoftGraceProjectionSubmits;
                    return directResult;
                }
            }
            else if (grace.kind == SourceKind::ClassicSbs &&
                ProjectionMatchesSnapshot(endInfo, grace))
            {
                return SubmitOriginal(session, endInfo, grace, true);
            }
        }

        // A fresh classic fallback remains preferable to a frozen image.
        if (OutRunVrReviewHardening::FreshClassicFallbackAvailable())
        {
            OutRunVrR21RuntimeHardening::BindLegacyBlitConstantBufferToVs();
            return OutRunVrSbsCaptureOverride::EndFrame(session, endInfo);
        }

        return SubmitVisibleFallback(
            session, endInfo, "no fresh verified gameplay bundle");
    }

    inline XrResult XRAPI_CALL DestroySession(XrSession session) noexcept
    {
        ExactProjectionSubmits = 0;
        SoftGraceProjectionSubmits = 0;
        DirectSafeProjectionSubmits = 0;
        LiveTheaterFallbacks = 0;
        DirectFlatFallbacks = 0;
        CachedLayerFallbacks = 0;
        EmergencyLayerFallbacks = 0;
        EmptyFrameFallbacks = 0;
        MixedValidatedSubmits = 0;
        ProjectionCommittedGeneration = 0;
        TheaterCommittedGeneration = 0;
        return OutRunVrR23RuntimeHardening::DestroySession(session);
    }
}

#define xrEndFrame OutRunVrR24BlackScreenGuard::EndFrame
#define xrDestroySession OutRunVrR24BlackScreenGuard::DestroySession
