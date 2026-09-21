// R32 review-consolidation overlay.
//
// Keeps R31/R29/R14 correctness policy while consolidating the reviewed hot
// paths. Review-2 additionally makes R22 the reset owner and prevents reuse of a
// DirectGPU producer slot while a timed-out D3D9 EVENT query is still pending.

#include "r32_policy.hpp"
#include "stereo_renderer_r31.cpp"

namespace OutRunVRStereo
{
    namespace
    {
        SafetyHookInline R32ResetR22Hook{};
        SafetyHookInline R32ResolveDirectR13Hook{};
        SafetyHookInline R32PresentR13Hook{};
        SafetyHookInline R32DrawPrimitiveR31Hook{};
        SafetyHookInline R32DrawIndexedPrimitiveR31Hook{};
        SafetyHookInline R32DrawPrimitiveUPR31Hook{};
        SafetyHookInline R32DrawIndexedPrimitiveUPR31Hook{};

        std::atomic<OutRunVR::RuntimeEligibility::InstallState> R32InstallState{
            OutRunVR::RuntimeEligibility::InstallState::Pending };

        std::uint64_t R32BatchWvpUploads = 0;
        std::uint64_t R32BatchWvpFailures = 0;
        std::uint64_t R32StateSnapshotFailures = 0;
        std::uint64_t R32FailClosedZeroDisparityDraws = 0;
        std::uint64_t R32ResetEpochRearms = 0;
        std::uint64_t R32ResetFailures = 0;
        std::uint64_t R32DirectProbeCacheHits = 0;
        std::uint64_t R32DirectFenceSuccess = 0;
        std::uint64_t R32DirectFenceBudgetFallbacks = 0;
        std::uint64_t R32DirectIdentityInvalidations = 0;
        std::uint64_t R32PendingFenceDrains = 0;
        std::uint64_t R32PendingFenceBlocks = 0;
        std::uint64_t R32PendingFenceErrors = 0;
        bool R32FirstStateSnapshotFailureLogged = false;
        bool R32FirstBatchWvpLogged = false;
        bool R32FirstFenceBudgetLogged = false;
        bool R32FirstResetRearmLogged = false;
        bool R32FirstPendingFenceLogged = false;
        bool R32FirstDirectCopyRejectLogged = false;
        bool R32DirectCopyPathRejected = false;
        HRESULT R32DirectCopyRejectHr = D3D_OK;

        std::uint32_t R32DirectHostPid = 0;
        std::uint32_t R32DirectHostLuidLow = 0;
        std::uint32_t R32DirectHostLuidHigh = 0;
        std::array<bool, OutRunVR::RenderFrameRingSize> R32ProducerFencePending{};
        std::array<std::uint32_t, OutRunVR::RenderFrameRingSize> R32ProducerPendingFrame{};

        struct R32CounterSnapshot
        {
            ULONGLONG lastLogMs = 0;
            std::uint64_t liveWvp = 0;
            std::uint64_t liveReject = 0;
            std::uint64_t stateRecord = 0;
            std::uint64_t stateApply = 0;
            std::uint64_t batch = 0;
            std::uint64_t batchFail = 0;
            std::uint64_t stateFail = 0;
            std::uint64_t zeroFallback = 0;
            std::uint64_t directCache = 0;
            std::uint64_t directFenceOk = 0;
            std::uint64_t directFenceFallback = 0;
            std::uint64_t directBackpressure = 0;
            std::uint64_t pendingDrain = 0;
            std::uint64_t pendingBlock = 0;
            std::uint64_t pendingError = 0;
            std::uint64_t resetRearm = 0;
            std::uint64_t resetFail = 0;
        };
        R32CounterSnapshot R32Counters{};

        struct R32EffectSnapshot
        {
            DWORD alphaBlend = FALSE;
            DWORD alphaTest = FALSE;
            DWORD zWrite = TRUE;
            DWORD zEnable = D3DZB_TRUE;
            DWORD cullMode = D3DCULL_CCW;
        };

        bool R32ReadEffectSnapshot(IDirect3DDevice9* device,
            R32EffectSnapshot& out) noexcept
        {
            if (!device)
                return false;
            const bool ok =
                SUCCEEDED(device->GetRenderState(
                    D3DRS_ALPHABLENDENABLE, &out.alphaBlend)) &&
                SUCCEEDED(device->GetRenderState(
                    D3DRS_ALPHATESTENABLE, &out.alphaTest)) &&
                SUCCEEDED(device->GetRenderState(
                    D3DRS_ZWRITEENABLE, &out.zWrite)) &&
                SUCCEEDED(device->GetRenderState(
                    D3DRS_ZENABLE, &out.zEnable)) &&
                SUCCEEDED(device->GetRenderState(D3DRS_CULLMODE, &out.cullMode));
            if (!ok)
            {
                ++R32StateSnapshotFailures;
                if (!R32FirstStateSnapshotFailureLogged)
                {
                    R32FirstStateSnapshotFailureLogged = true;
                    spdlog::warn(
                        "VR R32 SAFETY: render-state snapshot unavailable; draw is forced to stock-WVP zero disparity instead of fail-open world stereo");
                }
            }
            return ok;
        }

        bool R32EffectIsFragileLive(IDirect3DDevice9* device,
            bool& fragile) noexcept
        {
            R32EffectSnapshot state{};
            if (!R32ReadEffectSnapshot(device, state))
                return false;
            const auto policy = OutRunVR::PassPolicy::ClassifyEffectStereo(
                state.alphaBlend != FALSE,
                state.alphaTest != FALSE,
                state.zWrite != FALSE,
                state.zEnable != D3DZB_FALSE,
                state.cullMode == D3DCULL_NONE);
            fragile = !OutRunVR::PassPolicy::AllowsEffectWorldStereo(policy);
            return true;
        }

        bool R32SetWvpBatch(IDirect3DDevice9* device,
            const float* constants) noexcept
        {
            if (!device || !constants)
                return false;
            if (Settings::VRTelemetry)
                ++R32BatchWvpUploads;
            const HRESULT hr = device->SetVertexShaderConstantF(
                OutRunWvpRegister, constants, OutRunWvpRegisterCount);
            if (FAILED(hr))
            {
                ++R32BatchWvpFailures;
                return false;
            }
            if (Settings::VRTelemetry && !R32FirstBatchWvpLogged)
            {
                R32FirstBatchWvpLogged = true;
                spdlog::info(
                    "VR R32 PERF: verified stereo WVP uploads are batched as one c64..c67 call instead of four register calls");
            }
            return true;
        }

        bool R32GetSavedViewport(IDirect3DDevice9* device,
            D3DVIEWPORT9& viewport) noexcept
        {
            if (!device)
                return false;
            if (R31StateBlockTrackingReliable.load(std::memory_order_acquire))
                return R31GetSavedViewport(device, viewport);
            return SUCCEEDED(device->GetViewport(&viewport));
        }

        bool R32RestoreRightPassState(IDirect3DDevice9* device,
            IDirect3DSurface9* savedRt, IDirect3DSurface9* savedDepth,
            const D3DVIEWPORT9& savedViewport,
            const float* originalConstants, bool restoreWvp) noexcept
        {
            bool ok = true;
            if (savedRt && FAILED(SetRenderTargetHook.stdcall<HRESULT>(
                    device, 0u, savedRt)))
                ok = false;
            const HRESULT depthHr = SetDepthStencilSurfaceHook
                ? SetDepthStencilSurfaceHook.stdcall<HRESULT>(device, savedDepth)
                : device->SetDepthStencilSurface(savedDepth);
            if (FAILED(depthHr)) ok = false;
            if (FAILED(device->SetViewport(&savedViewport))) ok = false;
            if (restoreWvp && !R32SetWvpBatch(device, originalConstants)) ok = false;
            return ok;
        }

        template <typename ActualDraw>
        R31OwnedResult R32TryFastWorld(IDirect3DDevice9* device,
            ActualDraw&& actualDraw, const char* site)
        {
            if (R31StateBlockRecording || !R29StableStereoBase(device))
            {
                if (IsGameDevice(device) && !InternalStereoPass && TargetIsBackBuffer())
                    ++R31Frame.unstable;
                return {};
            }

            bool fragile = true;
            const bool stateBlocksReliable =
                R31StateBlockTrackingReliable.load(std::memory_order_acquire);
            const bool effectKnown = stateBlocksReliable
                ? R29FragileEffectCached(device, fragile)
                : R32EffectIsFragileLive(device, fragile);
            if (!effectKnown)
                return {};
            if (fragile)
            {
                ++R31Frame.fragile;
                return {};
            }

            if (!EnsureStereoResources(device))
                return {};
            if (TrackedDepthStencil &&
                (!RightDepthSynchronized || !RightStencilSynchronized))
                TryBootstrapRightDepthFromRecentClear(device);
            if (TrackedDepthStencil && !RightDepthSynchronized &&
                DepthTestActive(device))
                return {};
            if (TrackedDepthStencil && !RightStencilSynchronized &&
                StencilTestActive(device))
                return {};

            OutRunVRRenderer::LatchedStereoFrame stereo{};
            if (!OutRunVRRenderer::GetLatchedStereoFrame(stereo) ||
                stereo.poseSequence == 0)
                return {};
            if (FrameStereoPoseSequence != 0 &&
                FrameStereoPoseSequence != stereo.poseSequence)
                return {};

            DrawStereoState draw{};
            if (!R31BuildFastWorldConstants(device, stereo, draw))
                return {};

            D3DVIEWPORT9 savedViewport{};
            if (!R32GetSavedViewport(device, savedViewport))
                return {};

            bool leftWvpOk = false;
            {
                InternalPassScope guard;
                leftWvpOk = R32SetWvpBatch(device, draw.eyeConstants[0]);
            }
            if (!leftWvpOk)
            {
                bool rolledBack = false;
                {
                    InternalPassScope guard;
                    rolledBack = R32SetWvpBatch(device, draw.originalConstants);
                }
                if (!rolledBack)
                {
                    R9Poison(OutRunVR::StereoFailureRestoreFailed,
                        "R32/fast-left-WVP-rollback");
                    NoteRestoreFailure("R32 fast left-eye c64 rollback");
                    R29ArmMonoSafety();
                    return { true, E_FAIL };
                }
                return {};
            }

            ++R9DrawCalls;
            R9MonoBackupGap = true;
            if (LeftDrawMayWriteDepth(device) || LeftDrawMayWriteStencil(device))
                ++R9MainDepthContentSerial;

            R31OwnedResult result{ true, D3D_OK };
            result.hr = actualDraw();
            if (FAILED(result.hr))
            {
                InvalidateRightDepthStencilIfLeftMayWrite(device);
                R9Poison(OutRunVR::StereoFailureLeftDrawFailed, site, result.hr);
                bool restored = false;
                {
                    InternalPassScope guard;
                    restored = R32SetWvpBatch(device, draw.originalConstants);
                }
                if (!restored) NoteRestoreFailure("R32 fast left draw c64");
                R29ArmMonoSafety();
                return result;
            }

            IDirect3DSurface9* savedRt = TrackedRenderTarget;
            IDirect3DSurface9* savedDepth = TrackedDepthStencil;
            HRESULT rightHr = D3D_OK;
            OutRunVR::StereoFailureReason rightFailure =
                OutRunVR::StereoFailureRightStateFailed;
            bool restoreOk = true;
            {
                InternalPassScope guard;
                rightHr = SetRenderTargetHook.stdcall<HRESULT>(
                    device, 0u, RightEyeSurface);
                if (SUCCEEDED(rightHr))
                    rightHr = SetDepthStencilSurfaceHook.stdcall<HRESULT>(
                        device, TrackedDepthStencil ? RightEyeDepth : nullptr);
                if (SUCCEEDED(rightHr))
                    rightHr = device->SetViewport(&savedViewport);
                if (SUCCEEDED(rightHr) &&
                    !R32SetWvpBatch(device, draw.eyeConstants[1]))
                {
                    rightFailure = OutRunVR::StereoFailureRightWvpUploadFailed;
                    rightHr = E_FAIL;
                }
                if (SUCCEEDED(rightHr))
                {
                    rightFailure = OutRunVR::StereoFailureRightDrawFailed;
                    rightHr = actualDraw();
                }
                restoreOk = R32RestoreRightPassState(device, savedRt, savedDepth,
                    savedViewport, draw.originalConstants, true);
            }

            FrameHadDuplicatedDraw = true;
            FrameHadWorldStereo = true;
            ++DuplicatedDraws;
            ++WorldStereoDraws;
            ++R29StableTwoEyeDraws;
            ++R31FastWorldDraws;
            ++R31Frame.fastWorld;

            if (FrameStereoPoseSequence == 0)
            {
                FrameStereoPoseSequence = draw.poseSequence;
                FrameStereoMetadata = draw.stereoFrame;
            }

            if (FAILED(rightHr))
            {
                FrameRightDrawFailed = true;
                InvalidateRightDepthStencilIfLeftMayWrite(device);
                R9Poison(rightFailure, site, rightHr);
                R29ArmMonoSafety();
            }
            if (!restoreOk)
            {
                InvalidateRightDepthStencilIfLeftMayWrite(device);
                NoteRestoreFailure("R32 fast right-eye draw");
                R29ArmMonoSafety();
            }
            return result;
        }

        template <typename ActualDraw>
        R31OwnedResult R32TryHud(IDirect3DDevice9* device,
            ActualDraw&& actualDraw, const char* site)
        {
            if (R31StateBlockRecording || !R29StableStereoBase(device) ||
                !R30CurrentPassIsScreenSpace2D())
                return {};
            if (!R31StateBlockTrackingReliable.load(std::memory_order_acquire))
            {
                R31DiscardUnreliableDrawCaches();
                const std::uintptr_t cachedShader =
                    CurrentVertexShaderIdentity.load(std::memory_order_acquire);
                if (!R31LiveShaderMatches(device, cachedShader))
                    return {};
            }
            if (!EnsureStereoResources(device))
                return {};
            if (TrackedDepthStencil &&
                (!RightDepthSynchronized || !RightStencilSynchronized))
                TryBootstrapRightDepthFromRecentClear(device);
            if (TrackedDepthStencil && !RightDepthSynchronized &&
                DepthTestActive(device))
                return {};
            if (TrackedDepthStencil && !RightStencilSynchronized &&
                StencilTestActive(device))
                return {};

            OutRunVRRenderer::LatchedStereoFrame stereo{};
            if (!OutRunVRRenderer::GetLatchedStereoFrame(stereo) ||
                stereo.poseSequence == 0)
                return {};
            if (FrameStereoPoseSequence != 0 &&
                FrameStereoPoseSequence != stereo.poseSequence)
                return {};

            float original[16]{};
            float eyeConstants[2][16]{};
            float eyeScale[2]{};
            float eyeOffset[2]{};
            if (!R30BuildScreenSpaceEyeConstants(device, stereo, original,
                    eyeConstants, eyeScale, eyeOffset))
                return {};

            D3DVIEWPORT9 savedViewport{};
            if (!R32GetSavedViewport(device, savedViewport))
                return {};

            bool leftWvpOk = false;
            {
                InternalPassScope guard;
                leftWvpOk = R32SetWvpBatch(device, eyeConstants[0]);
            }
            if (!leftWvpOk)
            {
                bool rolledBack = false;
                {
                    InternalPassScope guard;
                    rolledBack = R32SetWvpBatch(device, original);
                }
                if (!rolledBack)
                {
                    R9Poison(OutRunVR::StereoFailureRestoreFailed,
                        "R32/HUD-left-WVP-rollback");
                    NoteRestoreFailure("R32 HUD left-eye c64 rollback");
                    R29ArmMonoSafety();
                    return { true, E_FAIL };
                }
                return {};
            }

            ++R9DrawCalls;
            R9MonoBackupGap = true;
            if (LeftDrawMayWriteDepth(device) || LeftDrawMayWriteStencil(device))
                ++R9MainDepthContentSerial;

            R31OwnedResult result{ true, actualDraw() };
            if (FAILED(result.hr))
            {
                bool restored = false;
                {
                    InternalPassScope guard;
                    restored = R32SetWvpBatch(device, original);
                }
                R9Poison(OutRunVR::StereoFailureLeftDrawFailed, site, result.hr);
                if (!restored) NoteRestoreFailure("R32 HUD left draw c64");
                R29ArmMonoSafety();
                return result;
            }

            IDirect3DSurface9* savedRt = TrackedRenderTarget;
            IDirect3DSurface9* savedDepth = TrackedDepthStencil;
            HRESULT rightHr = D3D_OK;
            OutRunVR::StereoFailureReason rightFailure =
                OutRunVR::StereoFailureRightStateFailed;
            bool restoreOk = true;
            {
                InternalPassScope guard;
                rightHr = SetRenderTargetHook.stdcall<HRESULT>(
                    device, 0u, RightEyeSurface);
                if (SUCCEEDED(rightHr))
                    rightHr = SetDepthStencilSurfaceHook.stdcall<HRESULT>(
                        device, TrackedDepthStencil ? RightEyeDepth : nullptr);
                if (SUCCEEDED(rightHr))
                    rightHr = device->SetViewport(&savedViewport);
                if (SUCCEEDED(rightHr) &&
                    !R32SetWvpBatch(device, eyeConstants[1]))
                {
                    rightFailure = OutRunVR::StereoFailureRightWvpUploadFailed;
                    rightHr = E_FAIL;
                }
                if (SUCCEEDED(rightHr))
                {
                    rightFailure = OutRunVR::StereoFailureRightDrawFailed;
                    rightHr = actualDraw();
                }
                restoreOk = R32RestoreRightPassState(device, savedRt, savedDepth,
                    savedViewport, original, true);
            }

            FrameHadDuplicatedDraw = true;
            ++DuplicatedDraws;
            ++NonWorldDuplicatedDraws;
            ++R29StableTwoEyeDraws;
            ++R30ScreenSpaceFovDraws;
            ++R31HudDraws;
            ++R31Frame.hud;

            if (FAILED(rightHr))
            {
                FrameRightDrawFailed = true;
                InvalidateRightDepthStencilIfLeftMayWrite(device);
                R9Poison(rightFailure, site, rightHr);
                R29ArmMonoSafety();
            }
            if (!restoreOk)
            {
                InvalidateRightDepthStencilIfLeftMayWrite(device);
                NoteRestoreFailure("R32 HUD right-eye draw");
                R29ArmMonoSafety();
            }
            return result;
        }

        template <typename LowerDraw>
        HRESULT R32LowerFailClosed(IDirect3DDevice9* device,
            LowerDraw&& lowerDraw) noexcept
        {
            if (!IsGameDevice(device) || InternalStereoPass ||
                !TargetIsBackBuffer() || !StereoWanted() || !R9StereoSeeded)
                return lowerDraw();

            R32EffectSnapshot snapshot{};
            if (R32ReadEffectSnapshot(device, snapshot))
                return lowerDraw();

            const std::uintptr_t savedIdentity =
                CurrentVertexShaderIdentity.exchange(0, std::memory_order_acq_rel);
            const HRESULT hr = lowerDraw();
            if (savedIdentity != 0)
            {
                std::uintptr_t expected = 0;
                CurrentVertexShaderIdentity.compare_exchange_strong(
                    expected, savedIdentity,
                    std::memory_order_acq_rel, std::memory_order_acquire);
            }
            ++R32FailClosedZeroDisparityDraws;
            return hr;
        }

        template <typename ActualDraw, typename LowerDraw>
        HRESULT R32Dispatch(IDirect3DDevice9* device,
            ActualDraw&& actualDraw, LowerDraw&& lowerDraw,
            const char* site) noexcept
        {
            R31ObserveDraw(device);

            if (!R31StateBlockRecording)
            {
                if (R30CurrentPassIsScreenSpace2D())
                {
                    const auto hud = R32TryHud(device,
                        std::forward<ActualDraw>(actualDraw), site);
                    if (hud.handled)
                        return hud.hr;
                }
                else
                {
                    const auto fast = R32TryFastWorld(device,
                        std::forward<ActualDraw>(actualDraw), site);
                    if (fast.handled)
                        return fast.hr;
                }
            }

            ++R31Frame.fallback;
            return R32LowerFailClosed(device,
                std::forward<LowerDraw>(lowerDraw));
        }

        HRESULT __stdcall DrawPrimitiveDestR32(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
        {
            auto actual = [&]() {
                return DrawPrimitiveHook.stdcall<HRESULT>(
                    device, type, startVertex, primitiveCount);
            };
            auto lower = [&]() {
                return R32DrawPrimitiveR31Hook.stdcall<HRESULT>(
                    device, type, startVertex, primitiveCount);
            };
            return R32Dispatch(device, actual, lower, "R32/DrawPrimitive");
        }

        HRESULT __stdcall DrawIndexedPrimitiveDestR32(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            INT baseVertexIndex, UINT minVertexIndex, UINT numVertices,
            UINT startIndex, UINT primitiveCount)
        {
            auto actual = [&]() {
                return DrawIndexedPrimitiveHook.stdcall<HRESULT>(device, type,
                    baseVertexIndex, minVertexIndex, numVertices, startIndex,
                    primitiveCount);
            };
            auto lower = [&]() {
                return R32DrawIndexedPrimitiveR31Hook.stdcall<HRESULT>(device,
                    type, baseVertexIndex, minVertexIndex, numVertices,
                    startIndex, primitiveCount);
            };
            return R32Dispatch(device, actual, lower,
                "R32/DrawIndexedPrimitive");
        }

        HRESULT __stdcall DrawPrimitiveUPDestR32(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data,
            UINT stride)
        {
            auto actual = [&]() {
                return DrawPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, primitiveCount, data, stride);
            };
            auto lower = [&]() {
                return R32DrawPrimitiveUPR31Hook.stdcall<HRESULT>(
                    device, type, primitiveCount, data, stride);
            };
            return R32Dispatch(device, actual, lower, "R32/DrawPrimitiveUP");
        }

        HRESULT __stdcall DrawIndexedPrimitiveUPDestR32(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            UINT minVertexIndex, UINT numVertices, UINT primitiveCount,
            const void* indexData, D3DFORMAT indexFormat,
            const void* vertexData, UINT stride)
        {
            auto actual = [&]() {
                return DrawIndexedPrimitiveUPHook.stdcall<HRESULT>(device, type,
                    minVertexIndex, numVertices, primitiveCount, indexData,
                    indexFormat, vertexData, stride);
            };
            auto lower = [&]() {
                return R32DrawIndexedPrimitiveUPR31Hook.stdcall<HRESULT>(device,
                    type, minVertexIndex, numVertices, primitiveCount,
                    indexData, indexFormat, vertexData, stride);
            };
            return R32Dispatch(device, actual, lower,
                "R32/DrawIndexedPrimitiveUP");
        }

        void R32ForgetDirectIdentity() noexcept
        {
            R32DirectHostPid = 0;
            R32DirectHostLuidLow = 0;
            R32DirectHostLuidHigh = 0;
        }

        void R32ClearPendingProducerFences() noexcept
        {
            R32ProducerFencePending.fill(false);
            R32ProducerPendingFrame.fill(0);
        }

        bool R32DirectIdentityMatches() noexcept
        {
            return SharedState && DirectInteropVerified &&
                R32DirectHostPid != 0 &&
                R32DirectHostPid == SharedState->hostPid &&
                R32DirectHostLuidLow == SharedState->hostAdapterLuidLow &&
                R32DirectHostLuidHigh == SharedState->hostAdapterLuidHigh;
        }

        void R32InvalidateDirectInteropOnly() noexcept
        {
            R32ClearPendingProducerFences();
            R32DirectCopyPathRejected = false;
            R32DirectCopyRejectHr = D3D_OK;
            ReleaseDirectTransportSlots();
            ReleaseCom(DirectInteropProbeFence);
            ReleaseCom(DirectInteropProbeSurface);
            ReleaseCom(DirectInteropProbeTexture);
            DirectInteropProbeHandle = nullptr;
            DirectInteropProbeToken = 0;
            DirectInteropVerified = false;
            if (SharedState && SharedState->magic == OutRunVR::SharedMagic)
            {
                InterlockedExchange(reinterpret_cast<volatile LONG*>(
                    &SharedState->clientInteropProbeHandle), 0);
                InterlockedExchange(reinterpret_cast<volatile LONG*>(
                    &SharedState->clientInteropProbeToken), 0);
            }
            R32ForgetDirectIdentity();
            ++R32DirectIdentityInvalidations;
        }

        bool R32EnsureDirectResources(IDirect3DDevice9* device) noexcept
        {
            if (DirectTransportResourcesReady && R32DirectIdentityMatches())
            {
                if (Settings::VRTelemetry) ++R32DirectProbeCacheHits;
                return true;
            }

            if (DirectTransportResourcesReady && !R32DirectIdentityMatches())
                R32InvalidateDirectInteropOnly();

            if (!DirectTransportResourcesReady)
                R32ClearPendingProducerFences();
            if (!EnsureDirectTransportResources(device))
                return false;
            if (!SharedState || !DirectInteropVerified)
                return false;

            R32DirectHostPid = SharedState->hostPid;
            R32DirectHostLuidLow = SharedState->hostAdapterLuidLow;
            R32DirectHostLuidHigh = SharedState->hostAdapterLuidHigh;
            return true;
        }

        bool R32WaitProducerFence(IDirect3DQuery9* query) noexcept
        {
            if (!query)
                return false;

            static const LONGLONG qpcFrequency = []() noexcept {
                LARGE_INTEGER value{};
                return QueryPerformanceFrequency(&value) != FALSE
                    ? value.QuadPart : 0;
            }();
            LARGE_INTEGER start{};
            const bool highResolutionClock = qpcFrequency > 0 &&
                QueryPerformanceCounter(&start) != FALSE;
            const ULONGLONG fallbackDeadline = highResolutionClock ? 0 :
                GetTickCount64() + OutRunVR::R32::ProducerFenceBudgetMs;
            const LONGLONG budgetTicks = highResolutionClock
                ? (qpcFrequency *
                    static_cast<LONGLONG>(OutRunVR::R32::ProducerFenceBudgetMs) +
                    999) / 1000
                : 0;

            // Budget starts before the FLUSH request so a slow first GetData is
            // accounted for instead of being hidden outside the 2 ms window.
            HRESULT ready = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
            if (ready == S_OK)
            {
                if (Settings::VRTelemetry) ++R32DirectFenceSuccess;
                return true;
            }
            if (ready != S_FALSE)
                return false;

            for (;;)
            {
                ready = query->GetData(nullptr, 0, 0);
                if (ready == S_OK)
                {
                    if (Settings::VRTelemetry) ++R32DirectFenceSuccess;
                    return true;
                }

                bool expired = ready != S_FALSE;
                if (!expired && highResolutionClock)
                {
                    LARGE_INTEGER now{};
                    expired = QueryPerformanceCounter(&now) == FALSE ||
                        now.QuadPart - start.QuadPart >= budgetTicks;
                }
                else if (!expired)
                {
                    expired = GetTickCount64() >= fallbackDeadline;
                }

                if (expired)
                {
                    if (Settings::VRTelemetry) ++R32DirectFenceBudgetFallbacks;
                    ++DirectTransportFenceTimeouts;
                    if (!R32FirstFenceBudgetLogged)
                    {
                        R32FirstFenceBudgetLogged = true;
                        spdlog::warn(
                            "VR R32 D3D9Ex: producer copy fence exceeded {}ms; falling back to SBS instead of stalling up to 12ms",
                            OutRunVR::R32::ProducerFenceBudgetMs);
                    }
                    return false;
                }
                SwitchToThread();
            }
        }

        bool R32DrainPendingProducerFence(std::uint32_t slotIndex) noexcept
        {
            if (slotIndex >= R32ProducerFencePending.size() ||
                !R32ProducerFencePending[slotIndex])
                return true;

            auto& slot = DirectTransportSlots[slotIndex];
            if (!slot.fence)
            {
                R32DirectCopyPathRejected = true;
                R32DirectCopyRejectHr = E_FAIL;
                if (Settings::VRTelemetry) ++R32PendingFenceErrors;
                return false;
            }

            const HRESULT ready = slot.fence->GetData(nullptr, 0, 0);
            if (ready == S_OK)
            {
                R32ProducerFencePending[slotIndex] = false;
                R32ProducerPendingFrame[slotIndex] = 0;
                if (Settings::VRTelemetry) ++R32PendingFenceDrains;
                return true;
            }
            if (ready == S_FALSE)
            {
                if (Settings::VRTelemetry) ++R32PendingFenceBlocks;
                ++DirectTransportRingBackpressure;
                if (!R32FirstPendingFenceLogged)
                {
                    R32FirstPendingFenceLogged = true;
                    spdlog::info(
                        "VR R32 D3D9Ex: timed-out producer EVENT remains pending; the ring slot is blocked from reuse until the GPU reports completion");
                }
                return false;
            }

            // A query error does not prove GPU completion. Keep the pending
            // marker intact and quarantine DirectGPU until Reset or interop
            // identity regeneration recreates the ring.
            R32DirectCopyPathRejected = true;
            R32DirectCopyRejectHr = ready;
            if (Settings::VRTelemetry) ++R32PendingFenceErrors;
            return false;
        }

        bool ResolveDirectTransportR32(IDirect3DDevice9* device,
            std::uint32_t frameId) noexcept
        {
            if (!R13OverlayReady.load(std::memory_order_acquire))
                return R32ResolveDirectR13Hook.call<bool>(device, frameId);
            if (!frameId || !R32EnsureDirectResources(device) ||
                !BackBuffer || !RightEyeSurface)
                return false;
            // R32EnsureDirectResources must run before this cached rejection:
            // host PID/LUID or transport-generation changes invalidate the old
            // interop identity and clear the rejection automatically.
            if (R32DirectCopyPathRejected)
                return false;

            const std::uint32_t slotIndex =
                (frameId - 1u) % OutRunVR::RenderFrameRingSize;
            if (!R32DrainPendingProducerFence(slotIndex))
                return false;

            auto& slot = DirectTransportSlots[slotIndex];
            if (slot.frameId)
            {
                std::uint32_t gpuCompleted = 0;
                const bool ackValid =
                    R13ReadGpuCompletedFrame(slotIndex, gpuCompleted);
                if (!ackValid || !FrameIdAtOrAfter(gpuCompleted, slot.frameId))
                {
                    ++R13SafeAckBackpressure;
                    ++DirectTransportRingBackpressure;
                    return false;
                }
            }

            {
                InternalPassScope guard;
                const HRESULT leftCopy = device->StretchRect(BackBuffer, nullptr,
                    slot.leftSurface, nullptr, D3DTEXF_NONE);
                const HRESULT rightCopy = SUCCEEDED(leftCopy)
                    ? device->StretchRect(RightEyeSurface, nullptr,
                        slot.rightSurface, nullptr, D3DTEXF_NONE)
                    : leftCopy;
                if (FAILED(leftCopy) || FAILED(rightCopy))
                {
                    R32DirectCopyPathRejected = true;
                    R32DirectCopyRejectHr = FAILED(leftCopy)
                        ? leftCopy : rightCopy;
                    if (!R32FirstDirectCopyRejectLogged)
                    {
                        R32FirstDirectCopyRejectLogged = true;
                        spdlog::warn(
                            "VR R32 D3D9Ex: shared-eye StretchRect rejected hr=0x{:08X}; DirectGPU copy path is disabled until Reset/interop revalidation instead of retrying every Present",
                            static_cast<unsigned>(R32DirectCopyRejectHr));
                    }
                    return false;
                }
                const HRESULT issueHr = slot.fence->Issue(D3DISSUE_END);
                if (FAILED(issueHr))
                {
                    // StretchRect commands are already queued. Without a valid
                    // EVENT we cannot prove when this producer slot is reusable,
                    // so quarantine the whole DirectGPU path until reset/interop
                    // revalidation rather than cycling back into this slot.
                    R32DirectCopyPathRejected = true;
                    R32DirectCopyRejectHr = issueHr;
                    if (Settings::VRTelemetry) ++R32PendingFenceErrors;
                    return false;
                }
            }

            R32ProducerFencePending[slotIndex] = true;
            R32ProducerPendingFrame[slotIndex] = frameId;
            if (!R32WaitProducerFence(slot.fence))
                return false;

            R32ProducerFencePending[slotIndex] = false;
            R32ProducerPendingFrame[slotIndex] = 0;
            slot.frameId = frameId;
            ActiveDirectTransportSlot = slotIndex;
            return true;
        }

        void R32InvalidateResetCaches() noexcept
        {
            R29Effect = {};
            R31BlockedVerifiedGeneration = 0;
            R31FastWorldCandidates = 0;
            R31EyeCache = {};
            R31Frame = {};
            R31Window = {};
            R23LastStateSampleDrawSerial = 0;
            R23LastStateSampleEpoch = 0;
            OutRunVRRenderer::R29InvalidateRendererStateAfterExternalRestore();
            R32ForgetDirectIdentity();
            R32ClearPendingProducerFences();
            R32DirectCopyPathRejected = false;
            R32DirectCopyRejectHr = D3D_OK;
        }

        void R32ResetAfterGameReset() noexcept
        {
            R29MonoSafetyThroughEpoch = OutRunVR::R32::RearmMonoSafetyEpoch(
                PresentEpoch);
            R32InvalidateResetCaches();
            ++R32ResetEpochRearms;
            if (!R32FirstResetRearmLogged)
            {
                R32FirstResetRearmLogged = true;
                spdlog::info(
                    "VR R32 RESET: R22 completed Reset lifecycle first; mono-safety/cache generations were rearmed without discarding the freshly primed viewport/scissor shadow");
            }
        }

        HRESULT __stdcall ResetDestR32(IDirect3DDevice9* device,
            D3DPRESENT_PARAMETERS* params)
        {
            const bool gameDevice = IsGameDevice(device);
            if (gameDevice)
                R32ClearPendingProducerFences();

            const HRESULT hr = R32ResetR22Hook.stdcall<HRESULT>(device, params);
            if (gameDevice)
            {
                if (SUCCEEDED(hr))
                    R32ResetAfterGameReset();
                else
                {
                    R32InvalidateResetCaches();
                    ++R32ResetFailures;
                }
            }
            return hr;
        }

        void R32LogPerfWindow() noexcept
        {
            if (!Settings::VRTelemetry)
                return;
            const ULONGLONG now = GetTickCount64();
            if (R32Counters.lastLogMs == 0)
            {
                R32Counters.lastLogMs = now;
                R32Counters.liveWvp = R31FastWorldLiveValidations;
                R32Counters.liveReject = R31FastWorldValidationRejects;
                R32Counters.stateRecord = R31StateBlockRecordings;
                R32Counters.stateApply = R31StateBlockApplies;
                R32Counters.batch = R32BatchWvpUploads;
                R32Counters.batchFail = R32BatchWvpFailures;
                R32Counters.stateFail = R32StateSnapshotFailures;
                R32Counters.zeroFallback = R32FailClosedZeroDisparityDraws;
                R32Counters.directCache = R32DirectProbeCacheHits;
                R32Counters.directFenceOk = R32DirectFenceSuccess;
                R32Counters.directFenceFallback = R32DirectFenceBudgetFallbacks;
                R32Counters.directBackpressure = DirectTransportRingBackpressure;
                R32Counters.pendingDrain = R32PendingFenceDrains;
                R32Counters.pendingBlock = R32PendingFenceBlocks;
                R32Counters.pendingError = R32PendingFenceErrors;
                R32Counters.resetRearm = R32ResetEpochRearms;
                R32Counters.resetFail = R32ResetFailures;
                return;
            }
            if (now - R32Counters.lastLogMs < 5000)
                return;

            spdlog::info(
                "VR R32 PERF 5s: liveWvpCheck={} liveReject={} stateBlock[record={},apply={}] batchWvp[ok={},fail={}] safety[stateReadFail={},forcedZero={}] direct[probeCacheHit={},producerFenceOk={},producerBudgetFallback={},backpressure={},pendingDrain={},pendingBlock={},pendingError={}] reset[rearm={},fail={}]",
                R31FastWorldLiveValidations - R32Counters.liveWvp,
                R31FastWorldValidationRejects - R32Counters.liveReject,
                R31StateBlockRecordings - R32Counters.stateRecord,
                R31StateBlockApplies - R32Counters.stateApply,
                R32BatchWvpUploads - R32Counters.batch,
                R32BatchWvpFailures - R32Counters.batchFail,
                R32StateSnapshotFailures - R32Counters.stateFail,
                R32FailClosedZeroDisparityDraws - R32Counters.zeroFallback,
                R32DirectProbeCacheHits - R32Counters.directCache,
                R32DirectFenceSuccess - R32Counters.directFenceOk,
                R32DirectFenceBudgetFallbacks - R32Counters.directFenceFallback,
                DirectTransportRingBackpressure - R32Counters.directBackpressure,
                R32PendingFenceDrains - R32Counters.pendingDrain,
                R32PendingFenceBlocks - R32Counters.pendingBlock,
                R32PendingFenceErrors - R32Counters.pendingError,
                R32ResetEpochRearms - R32Counters.resetRearm,
                R32ResetFailures - R32Counters.resetFail);

            R32Counters.lastLogMs = now;
            R32Counters.liveWvp = R31FastWorldLiveValidations;
            R32Counters.liveReject = R31FastWorldValidationRejects;
            R32Counters.stateRecord = R31StateBlockRecordings;
            R32Counters.stateApply = R31StateBlockApplies;
            R32Counters.batch = R32BatchWvpUploads;
            R32Counters.batchFail = R32BatchWvpFailures;
            R32Counters.stateFail = R32StateSnapshotFailures;
            R32Counters.zeroFallback = R32FailClosedZeroDisparityDraws;
            R32Counters.directCache = R32DirectProbeCacheHits;
            R32Counters.directFenceOk = R32DirectFenceSuccess;
            R32Counters.directFenceFallback = R32DirectFenceBudgetFallbacks;
            R32Counters.directBackpressure = DirectTransportRingBackpressure;
            R32Counters.pendingDrain = R32PendingFenceDrains;
            R32Counters.pendingBlock = R32PendingFenceBlocks;
            R32Counters.pendingError = R32PendingFenceErrors;
            R32Counters.resetRearm = R32ResetEpochRearms;
            R32Counters.resetFail = R32ResetFailures;
        }

        HRESULT __stdcall PresentDestR32(IDirect3DDevice9* device,
            const RECT* sourceRect, const RECT* destRect,
            HWND destWindowOverride, const RGNDATA* dirtyRegion)
        {
            const HRESULT hr = R32PresentR13Hook.stdcall<HRESULT>(device,
                sourceRect, destRect, destWindowOverride, dirtyRegion);
            if (IsGameDevice(device))
                R32LogPerfWindow();
            return hr;
        }

        void R32RollbackHooks() noexcept
        {
            R32DrawIndexedPrimitiveUPR31Hook = {};
            R32DrawPrimitiveUPR31Hook = {};
            R32DrawIndexedPrimitiveR31Hook = {};
            R32DrawPrimitiveR31Hook = {};
            R32PresentR13Hook = {};
            R32ResolveDirectR13Hook = {};
            R32ResetR22Hook = {};
        }

        bool R32EnableHooks() noexcept
        {
            SafetyHookInline* hooks[]{
                &R32ResetR22Hook,
                &R32ResolveDirectR13Hook,
                &R32PresentR13Hook,
                &R32DrawPrimitiveR31Hook,
                &R32DrawIndexedPrimitiveR31Hook,
                &R32DrawPrimitiveUPR31Hook,
                &R32DrawIndexedPrimitiveUPR31Hook
            };
            for (auto* hook : hooks)
                if (!*hook || !hook->enable().has_value())
                    return false;
            return true;
        }

        DWORD WINAPI R32InstallThread(void*)
        {
            using State = OutRunVR::RuntimeEligibility::InstallState;
            R32InstallState.store(State::Pending, std::memory_order_release);
            for (int attempt = 0; attempt < 4800; ++attempt)
            {
                const auto r31 = R31InstallState.load(std::memory_order_acquire);
                const auto r22 = R22InstallState.load(std::memory_order_acquire);
                const auto r13 = R13InstallState.load(std::memory_order_acquire);
                if (r31 == State::Failed || r22 == State::Failed ||
                    r13 == R13InstallFailed)
                {
                    R32InstallState.store(State::Failed, std::memory_order_release);
                    HookManager::ReportAsyncResult("OpenXRVRStereoR32Review", false);
                    return 0;
                }
                if (r31 == State::Ready && r22 == State::Ready &&
                    r13 == R13InstallReady)
                {
                    const auto disabled = safetyhook::InlineHook::StartDisabled;
                    R32ResetR22Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&ResetDestR22), ResetDestR32, disabled);
                    R32ResolveDirectR13Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&ResolveDirectTransportR13),
                        ResolveDirectTransportR32, disabled);
                    R32PresentR13Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&PresentDestR13), PresentDestR32, disabled);
                    R32DrawPrimitiveR31Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveDestR31),
                        DrawPrimitiveDestR32, disabled);
                    R32DrawIndexedPrimitiveR31Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveDestR31),
                        DrawIndexedPrimitiveDestR32, disabled);
                    R32DrawPrimitiveUPR31Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveUPDestR31),
                        DrawPrimitiveUPDestR32, disabled);
                    R32DrawIndexedPrimitiveUPR31Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveUPDestR31),
                        DrawIndexedPrimitiveUPDestR32, disabled);

                    if (!R32EnableHooks())
                    {
                        R32RollbackHooks();
                        R32InstallState.store(State::Failed,
                            std::memory_order_release);
                        HookManager::ReportAsyncResult(
                            "OpenXRVRStereoR32Review", false);
                        spdlog::error(
                            "VR R32: review/optimization hook transaction was partial; R31/R22 remain authoritative");
                        return 0;
                    }

                    R32InstallState.store(State::Ready, std::memory_order_release);
                    HookManager::ReportAsyncResult("OpenXRVRStereoR32Review", true);
                    spdlog::info(
                        "VR R32 REVIEW2: R22-owned Reset lifecycle + fail-closed state reads + batched WVP + cached D3D9Ex interop + pending-fence-safe producer ring + delta telemetry READY");
                    return 0;
                }
                Sleep(25);
            }

            R32InstallState.store(State::Failed, std::memory_order_release);
            HookManager::ReportAsyncResult("OpenXRVRStereoR32Review", false);
            spdlog::error("VR R32: timed out waiting for R31/R22/R13 prerequisites");
            return 0;
        }

        class VRStereoR32ReviewHook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRStereoR32Review";
            }
            bool validate() override { return true; }
            bool apply() override
            {
                HANDLE thread = CreateThread(
                    nullptr, 0, R32InstallThread, nullptr, 0, nullptr);
                if (!thread)
                {
                    R32InstallState.store(
                        OutRunVR::RuntimeEligibility::InstallState::Failed,
                        std::memory_order_release);
                    return false;
                }
                CloseHandle(thread);
                return true;
            }
            static VRStereoR32ReviewHook instance;
        };

        VRStereoR32ReviewHook VRStereoR32ReviewHook::instance;
    }
}
