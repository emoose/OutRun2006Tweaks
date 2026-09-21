// R13 hardening wrapper. The validated R9 renderer remains source-of-truth;
// cmake marks stereo_renderer.cpp HEADER_FILE_ONLY and compiles this TU.
//
// Review hardening added here keeps unsafe MRT/occlusion transitions single-
// execution: once such a main-backbuffer draw appears after stereo has started,
// the rest of that Present is rendered only into the already-seeded mono safety
// shadow and restored before Present. This avoids replaying side-effecting MRT
// or query draws while still producing a complete mono fallback.

#include "r13_bridge.hpp"
#include "stereo_renderer.cpp"

namespace OutRunVRStereo
{
    namespace
    {
        SafetyHookInline R13ResetR9Hook{};
        SafetyHookInline R13ResolveDirectHook{};
        SafetyHookInline R13PresentR9Hook{};
        SafetyHookInline R13DrawPrimitiveR9Hook{};
        SafetyHookInline R13DrawIndexedPrimitiveR9Hook{};
        SafetyHookInline R13DrawPrimitiveUPR9Hook{};
        SafetyHookInline R13DrawIndexedPrimitiveUPR9Hook{};

        constexpr std::uint32_t R13InstallPending = 0;
        constexpr std::uint32_t R13InstallReady = 1;
        constexpr std::uint32_t R13InstallFailed = 2;
        std::atomic<std::uint32_t> R13InstallState{R13InstallPending};
        std::atomic<bool> R13OverlayReady{false};

        std::uint64_t R13SafeAckBackpressure = 0;
        bool R13FirstSafeAckBlockLogged = false;
        bool R13FirstAckMappingLogged = false;
        bool R13ForceMonoShadow = false;
        std::uint64_t R13UnsafeTransitionFrames = 0;
        bool R13FirstUnsafeTransitionLogged = false;
        std::uint64_t R13DrawTimeZeroDisparityDraws = 0;
        bool R13FirstDrawTimeZeroDisparityLogged = false;
        bool R13FirstDrawTimeStateReadFailureLogged = false;

        enum class R13EffectClass : std::uint8_t
        {
            Unknown,
            World,
            Flat
        };
        struct R13EffectSnapshot
        {
            R13EffectClass classification = R13EffectClass::Unknown;
            bool zKnown = false;
            bool zEnabled = false;
        };
        thread_local bool R13EffectOverrideActive = false;
        thread_local R13EffectSnapshot R13EffectOverride{};
        struct R13EffectOverrideScope
        {
            bool previousActive = false;
            R13EffectSnapshot previous{};
            explicit R13EffectOverrideScope(
                const R13EffectSnapshot& snapshot) noexcept
                : previousActive(R13EffectOverrideActive),
                  previous(R13EffectOverride)
            {
                R13EffectOverride = snapshot;
                R13EffectOverrideActive = true;
            }
            ~R13EffectOverrideScope()
            {
                R13EffectOverride = previous;
                R13EffectOverrideActive = previousActive;
            }
        };

        HANDLE R13AckMapping = nullptr;
        const OutRunVR::R13::DirectGpuAckState* R13AckState = nullptr;

        bool R13EnsureAckState() noexcept
        {
            if (R13AckState &&
                R13AckState->magic == OutRunVR::R13::DirectGpuAckMagic &&
                R13AckState->version == OutRunVR::R13::DirectGpuAckVersion &&
                R13AckState->structSize == sizeof(OutRunVR::R13::DirectGpuAckState))
                return true;

            if (R13AckState)
            {
                UnmapViewOfFile(R13AckState);
                R13AckState = nullptr;
            }
            if (R13AckMapping)
            {
                CloseHandle(R13AckMapping);
                R13AckMapping = nullptr;
            }

            R13AckMapping = OpenFileMappingW(
                FILE_MAP_READ, FALSE, OutRunVR::R13::DirectGpuAckName);
            if (!R13AckMapping)
                return false;

            R13AckState = static_cast<const OutRunVR::R13::DirectGpuAckState*>(MapViewOfFile(
                R13AckMapping, FILE_MAP_READ, 0, 0,
                sizeof(OutRunVR::R13::DirectGpuAckState)));
            if (!R13AckState)
            {
                CloseHandle(R13AckMapping);
                R13AckMapping = nullptr;
                return false;
            }

            if (R13AckState->magic != OutRunVR::R13::DirectGpuAckMagic ||
                R13AckState->version != OutRunVR::R13::DirectGpuAckVersion ||
                R13AckState->structSize != sizeof(OutRunVR::R13::DirectGpuAckState))
            {
                UnmapViewOfFile(R13AckState);
                R13AckState = nullptr;
                CloseHandle(R13AckMapping);
                R13AckMapping = nullptr;
                return false;
            }

            if (!R13FirstAckMappingLogged)
            {
                R13FirstAckMappingLogged = true;
                spdlog::info(
                    "VR D3D9Ex R13: dedicated per-slot GPU-consumer ACK mapping opened; legacy pose reserved fields remain untouched");
            }
            return true;
        }

        bool R13ReadGpuCompletedFrame(std::uint32_t slotIndex,
            std::uint32_t& completedFrame) noexcept
        {
            completedFrame = 0;
            if (slotIndex >= OutRunVR::RenderFrameRingSize || !R13EnsureAckState())
                return false;

            for (int attempt = 0; attempt < 4; ++attempt)
            {
                const std::uint32_t before = R13AckState->sequence;
                if (before & 1u)
                    continue;
                MemoryBarrier();

                OutRunVR::R13::DirectGpuAckState snapshot{};
                std::memcpy(&snapshot, R13AckState, sizeof(snapshot));

                MemoryBarrier();
                const std::uint32_t after = R13AckState->sequence;
                if (before != after || (after & 1u))
                    continue;

                if (snapshot.magic != OutRunVR::R13::DirectGpuAckMagic ||
                    snapshot.version != OutRunVR::R13::DirectGpuAckVersion ||
                    snapshot.structSize != sizeof(snapshot) ||
                    !snapshot.hostPid || !SharedState ||
                    snapshot.hostPid != SharedState->hostPid ||
                    snapshot.transportGeneration != DirectTransportGeneration)
                    return false;

                completedFrame = snapshot.completedFrameId[slotIndex];
                return true;
            }
            return false;
        }

        bool ResolveDirectTransportR13(IDirect3DDevice9* device, std::uint32_t frameId)
        {
            if (!R13OverlayReady.load(std::memory_order_acquire))
                return R13ResolveDirectHook.call<bool>(device, frameId);

            if (!frameId || !EnsureDirectTransportResources(device) ||
                !BackBuffer || !RightEyeSurface)
                return false;

            const std::uint32_t preferred =
                (frameId - 1u) % OutRunVR::RenderFrameRingSize;
            std::uint32_t selected = OutRunVR::RenderFrameRingSize;
            bool ackBlocked = false;

            // R38: the base producer can choose any free slot, so the safety
            // gate must validate the actual candidate slot rather than the old
            // frameId%ring slot. Only the dedicated host GPU-completion ACK can
            // release a published shared texture; the legacy global consumed
            // frame id is deliberately ignored here.
            for (std::uint32_t offset = 0;
                 offset < OutRunVR::RenderFrameRingSize; ++offset)
            {
                const std::uint32_t index =
                    (preferred + offset) % OutRunVR::RenderFrameRingSize;
                auto& slot = DirectTransportSlots[index];

                if (slot.producerPending)
                {
                    const HRESULT ready =
                        slot.fence ? slot.fence->GetData(nullptr, 0, 0) : E_FAIL;
                    if (ready == S_OK)
                    {
                        slot.producerPending = false;
                        slot.pendingFrameId = 0;
                        // A frame that missed its publication window was never
                        // visible to the host and needs no consumer ACK.
                        if (!slot.published)
                            slot.frameId = 0;
                    }
                    else if (ready == S_FALSE)
                    {
                        continue;
                    }
                    else
                    {
                        slot.producerPending = false;
                        slot.pendingFrameId = 0;
                        slot.frameId = 0;
                        slot.published = false;
                    }
                }

                if (slot.published && slot.frameId)
                {
                    std::uint32_t gpuCompleted = 0;
                    const bool ackValid =
                        R13ReadGpuCompletedFrame(index, gpuCompleted);
                    if (!ackValid ||
                        !FrameIdAtOrAfter(gpuCompleted, slot.frameId))
                    {
                        ackBlocked = true;
                        continue;
                    }
                    slot.frameId = 0;
                    slot.published = false;
                }

                selected = index;
                break;
            }

            if (selected >= OutRunVR::RenderFrameRingSize)
            {
                ++R13SafeAckBackpressure;
                ++DirectTransportRingBackpressure;
                if (ackBlocked && !R13FirstSafeAckBlockLogged)
                {
                    R13FirstSafeAckBlockLogged = true;
                    spdlog::info(
                        "VR D3D9Ex R38: per-slot GPU-completion ACK aware free-slot scan ACTIVE; published shared eyes remain immutable until the exact host EVENT fence completes");
                }
                return false;
            }

            auto& slot = DirectTransportSlots[selected];
            {
                InternalPassScope guard;
                if (FAILED(StretchDirectEye(
                        device, BackBuffer, slot.leftSurface)) ||
                    FAILED(StretchDirectEye(
                        device, RightEyeSurface, slot.rightSurface)) ||
                    FAILED(slot.fence->Issue(D3DISSUE_END)))
                    return false;
            }

            slot.producerPending = true;
            slot.pendingFrameId = frameId;
            slot.frameId = frameId;
            slot.published = false;
            ActiveDirectTransportSlot = selected;
            return true;
        }

        void R13ResetCommonPre(IDirect3DDevice9*)
        {
            R13ForceMonoShadow = false;
            R9ReleaseMonoResources();
            R9ReleaseDepthIdentity();
            R9StereoSeeded = false;
            R9MonoSeeded = false;
            R9MonoBackupGap = false;
            R9ExpectMainDepthAfterReset = true;
            R9FirstFailureEpoch = 0;

            OutRunVRRenderer::NotifyGameReset();
            ReleaseStereoResources();
            AuxRenderTargetActive = {};
            ActiveOcclusionQueries.store(0, std::memory_order_release);
            CurrentVertexShaderIdentity.store(0, std::memory_order_release);
            VertexShaderSerial.store(0, std::memory_order_release);
            LastStereoWanted = false;
            RightStencilSynchronized = true;
            FramePoseMismatchLogged = false;
            FirstMainDepthReuseLogged = false;
            FirstMainClearLogged = false;
            FirstDepthBootstrapLogged = false;
            PresentEpoch = 1;
            LastMainDepthClearEpoch = 0;
            LastMainDepthClearFlags = 0;
            LastMainDepthClearZ = 1.0f;
            LastMainDepthClearStencil = 0;
            LastMainDepthClearDesc = {};
            LastBeginSceneCountAtPresent = OutRunVRRenderer::GetBeginSceneCallCount();
            FrameStereoIncomplete = false;
            FrameFailureReason = OutRunVR::StereoFailureNone;
            FrameStereoMetadata = {};
            PublishStereoState(OutRunVR::StereoDisabled, false, 0, 0);
            PublishRenderFrame(OutRunVR::StereoDisabled, 0, 0, 0,
                OutRunVR::StereoFailureNone, nullptr);
        }

        HRESULT __stdcall ResetDestR13(IDirect3DDevice9* device,
            D3DPRESENT_PARAMETERS* params)
        {
            if (!R13OverlayReady.load(std::memory_order_acquire))
                return R13ResetR9Hook.stdcall<HRESULT>(device, params);

            if (!IsGameDevice(device) ||
                !OutRunVRD3D9ExUpgradeR13::IsCompatDevice(device))
                return R13ResetR9Hook.stdcall<HRESULT>(device, params);

            OutRunVRD3D9ExUpgradeR13::DisarmLegacyResetHook();
            R13ResetCommonPre(device);

            HRESULT hr = D3DERR_INVALIDCALL;
            if (!OutRunVRD3D9ExUpgradeR13::ResetCompatDevice(device, params, hr))
            {
                spdlog::error(
                    "VR D3D9Ex R13: compat device lost ResetEx ownership unexpectedly; refusing competing Reset chain");
                return D3DERR_INVALIDCALL;
            }

            if (SUCCEEDED(hr))
            {
                EnsureStereoResources(device);
                if (TrackedDepthStencil)
                {
                    R9CaptureMainDepth(TrackedDepthStencil, "r13-resetex-complete");
                    R9ExpectMainDepthAfterReset = false;
                }
            }
            return hr;
        }

        template <typename DrawCall>
        HRESULT R13DrawMonoShadowOnce(IDirect3DDevice9* device,
            DrawCall&& actualDraw, const char* site)
        {
            IDirect3DSurface9* savedRt = nullptr;
            IDirect3DSurface9* savedDepth = nullptr;
            D3DVIEWPORT9 savedViewport{};
            if (!R9BindMonoTarget(device, savedRt, savedDepth, savedViewport))
            {
                R9MonoBackupGap = true;
                R9Poison(OutRunVR::StereoFailureResourceUnavailable, site);
                return actualDraw();
            }

            const bool mayWriteDepth = LeftDrawMayWriteDepth(device);
            const bool mayWriteStencil = LeftDrawMayWriteStencil(device);
            const HRESULT drawHr = actualDraw();
            const bool restoreOk =
                R9RestoreGameTarget(device, savedRt, savedDepth, savedViewport);

            if (FAILED(drawHr))
            {
                ++R9MonoBackupDrawFailures;
                R9MonoBackupGap = true;
                R9Poison(OutRunVR::StereoFailureLeftDrawFailed, site, drawHr);
                return drawHr;
            }
            if (!restoreOk)
            {
                ++R9MonoBackupDrawFailures;
                R9MonoBackupGap = true;
                R9Poison(OutRunVR::StereoFailureRestoreFailed, site, E_FAIL);
                return E_FAIL;
            }

            ++R9MonoBackupDraws;
            if (mayWriteDepth || mayWriteStencil)
                ++R9MonoDepthContentSerial;
            return drawHr;
        }

        R13EffectSnapshot R13CaptureDrawTimeEffect(
            IDirect3DDevice9* device) noexcept
        {
            if (R13EffectOverrideActive)
                return R13EffectOverride;
            R13EffectSnapshot snapshot{};
            if (!device)
                return snapshot;

            DWORD alphaBlend = FALSE;
            DWORD alphaTest = FALSE;
            DWORD zWrite = TRUE;
            DWORD zEnable = D3DZB_TRUE;
            DWORD cullMode = D3DCULL_CCW;
            if (!ReadTrackedRenderState(device, D3DRS_ALPHABLENDENABLE, alphaBlend) ||
                !ReadTrackedRenderState(device, D3DRS_ALPHATESTENABLE, alphaTest) ||
                !ReadTrackedRenderState(device, D3DRS_ZWRITEENABLE, zWrite) ||
                !ReadTrackedRenderState(device, D3DRS_ZENABLE, zEnable) ||
                !ReadTrackedRenderState(device, D3DRS_CULLMODE, cullMode))
            {
                if (!R13FirstDrawTimeStateReadFailureLogged)
                {
                    R13FirstDrawTimeStateReadFailureLogged = true;
                    spdlog::warn(
                        "VR R13 effect policy: draw-time render-state snapshot unavailable; failing closed to zero-disparity");
                }
                return snapshot;
            }

            snapshot.zKnown = true;
            snapshot.zEnabled = zEnable != D3DZB_FALSE;
            const auto policy = OutRunVR::PassPolicy::ClassifyEffectStereo(
                alphaBlend != FALSE,
                alphaTest != FALSE,
                zWrite != FALSE,
                snapshot.zEnabled,
                cullMode == D3DCULL_NONE);
            snapshot.classification =
                OutRunVR::PassPolicy::AllowsEffectWorldStereo(policy)
                ? R13EffectClass::World : R13EffectClass::Flat;
            return snapshot;
        }

        bool R13DrawTimeFragileEffectNeedsZeroDisparity(
            IDirect3DDevice9* device) noexcept
        {
            return R13CaptureDrawTimeEffect(device).classification !=
                R13EffectClass::World;
        }

        template <typename LegacyDraw>
        HRESULT R13RunLegacyDrawWithDrawTimeEffectPolicy(
            IDirect3DDevice9* device, bool stereoActive,
            LegacyDraw&& legacyDraw) noexcept
        {
            if (!stereoActive ||
                !R13DrawTimeFragileEffectNeedsZeroDisparity(device))
                return legacyDraw();

            // BuildEyeConstants treats a zero shader identity as NonWorld and
            // therefore replays the draw to both eyes with the untouched stock
            // WVP. Temporarily mask only this draw. Restore the identity only if
            // no nested SetVertexShader changed it while the draw was executing.
            const std::uintptr_t savedIdentity =
                CurrentVertexShaderIdentity.exchange(0, std::memory_order_acq_rel);
            const HRESULT hr = legacyDraw();
            if (savedIdentity != 0)
            {
                std::uintptr_t expected = 0;
                CurrentVertexShaderIdentity.compare_exchange_strong(
                    expected, savedIdentity,
                    std::memory_order_acq_rel, std::memory_order_acquire);
            }

            ++R13DrawTimeZeroDisparityDraws;
            if (!R13FirstDrawTimeZeroDisparityLogged)
            {
                R13FirstDrawTimeZeroDisparityLogged = true;
                spdlog::info(
                    "VR R13 effect policy: draw-time alpha/billboard/shadow state confirmed; current draw duplicated zero-disparity with stock WVP");
            }
            return hr;
        }

        template <typename ActualDraw, typename LegacyDraw>
        HRESULT R13GuardedDraw(IDirect3DDevice9* device,
            ActualDraw&& actualDraw, LegacyDraw&& legacyDraw, const char* site)
        {
            if (!R13OverlayReady.load(std::memory_order_acquire))
                return legacyDraw();

            const bool gameDevice = IsGameDevice(device);
            const bool internalStereo = InternalStereoPass;
            if (!gameDevice || internalStereo)
                return legacyDraw();

            const bool mainTarget = TargetIsBackBuffer();
            if (!mainTarget)
                return legacyDraw();

            const bool unsafeMrt = AnyAuxRenderTargetActive();
            const bool unsafeOcclusion =
                ActiveOcclusionQueries.load(std::memory_order_acquire) > 0 ||
                OcclusionQueryTrackingUnavailable.load(std::memory_order_acquire);
            const bool stereoWanted = StereoWanted();
            const auto replayPolicy = OutRunVR::PassPolicy::ClassifyDrawReplay(
                gameDevice, internalStereo, mainTarget, R13ForceMonoShadow,
                stereoWanted, R9StereoSeeded, unsafeMrt, unsafeOcclusion);

            if (replayPolicy == OutRunVR::PassPolicy::DrawReplayPolicy::Legacy)
            {
                return R13RunLegacyDrawWithDrawTimeEffectPolicy(
                    device, stereoWanted && R9StereoSeeded,
                    std::forward<LegacyDraw>(legacyDraw));
            }
            if (replayPolicy == OutRunVR::PassPolicy::DrawReplayPolicy::ForcedMonoShadow)
                return R13DrawMonoShadowOnce(device, actualDraw, site);

            // UnsafeSingleExecution is the fail-closed class: MRT/query side
            // effects must execute only once even if stereo had already begun.
            const auto reason = unsafeMrt
                ? OutRunVR::StereoFailureMrtActive
                : OutRunVR::StereoFailureOcclusionQueryActive;
            R9Poison(reason, site);
            ++R13UnsafeTransitionFrames;

            if (R9MonoSeeded && !R9MonoBackupGap &&
                R9CurrentDepthCanMirror())
            {
                R13ForceMonoShadow = true;
                if (!R13FirstUnsafeTransitionLogged)
                {
                    R13FirstUnsafeTransitionLogged = true;
                    spdlog::info(
                        "VR R13 pass policy: unsafe MRT/occlusion classified single-execution; remainder of Present uses mono shadow without duplicated side effects");
                }
                return R13DrawMonoShadowOnce(device, actualDraw, site);
            }

            // If the independent mono history is unavailable, do not replay a
            // side-effecting draw. Continue the real target once and mark the
            // shadow unusable for Present restoration.
            R9MonoBackupGap = true;
            return actualDraw();
        }

        HRESULT __stdcall DrawPrimitiveDestR13(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
        {
            auto actual = [&]() {
                return DrawPrimitiveHook.stdcall<HRESULT>(
                    device, type, startVertex, primitiveCount);
            };
            auto legacy = [&]() {
                return R13DrawPrimitiveR9Hook.stdcall<HRESULT>(
                    device, type, startVertex, primitiveCount);
            };
            return R13GuardedDraw(device, actual, legacy, "R13/DrawPrimitive");
        }

        HRESULT __stdcall DrawIndexedPrimitiveDestR13(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, INT baseVertexIndex, UINT minVertexIndex,
            UINT numVertices, UINT startIndex, UINT primitiveCount)
        {
            auto actual = [&]() {
                return DrawIndexedPrimitiveHook.stdcall<HRESULT>(device, type,
                    baseVertexIndex, minVertexIndex, numVertices, startIndex,
                    primitiveCount);
            };
            auto legacy = [&]() {
                return R13DrawIndexedPrimitiveR9Hook.stdcall<HRESULT>(device, type,
                    baseVertexIndex, minVertexIndex, numVertices, startIndex,
                    primitiveCount);
            };
            return R13GuardedDraw(
                device, actual, legacy, "R13/DrawIndexedPrimitive");
        }

        HRESULT __stdcall DrawPrimitiveUPDestR13(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data, UINT stride)
        {
            auto actual = [&]() {
                return DrawPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, primitiveCount, data, stride);
            };
            auto legacy = [&]() {
                return R13DrawPrimitiveUPR9Hook.stdcall<HRESULT>(
                    device, type, primitiveCount, data, stride);
            };
            return R13GuardedDraw(device, actual, legacy, "R13/DrawPrimitiveUP");
        }

        HRESULT __stdcall DrawIndexedPrimitiveUPDestR13(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT minVertexIndex, UINT numVertices,
            UINT primitiveCount, const void* indexData, D3DFORMAT indexFormat,
            const void* vertexData, UINT stride)
        {
            auto actual = [&]() {
                return DrawIndexedPrimitiveUPHook.stdcall<HRESULT>(device, type,
                    minVertexIndex, numVertices, primitiveCount, indexData,
                    indexFormat, vertexData, stride);
            };
            auto legacy = [&]() {
                return R13DrawIndexedPrimitiveUPR9Hook.stdcall<HRESULT>(device, type,
                    minVertexIndex, numVertices, primitiveCount, indexData,
                    indexFormat, vertexData, stride);
            };
            return R13GuardedDraw(
                device, actual, legacy, "R13/DrawIndexedPrimitiveUP");
        }

        HRESULT __stdcall PresentDestR13(IDirect3DDevice9* device,
            const RECT* sourceRect, const RECT* destRect,
            HWND destWindowOverride, const RGNDATA* dirtyRegion)
        {
            if (!R13OverlayReady.load(std::memory_order_acquire))
            {
                return R13PresentR9Hook.stdcall<HRESULT>(device,
                    sourceRect, destRect, destWindowOverride, dirtyRegion);
            }

            const HRESULT hr = R13PresentR9Hook.stdcall<HRESULT>(device,
                sourceRect, destRect, destWindowOverride, dirtyRegion);
            if (IsGameDevice(device))
                R13ForceMonoShadow = false;
            return hr;
        }

        void R13RollbackOverlayHooks() noexcept
        {
            R13OverlayReady.store(false, std::memory_order_release);
            R13ResetR9Hook = {};
            R13ResolveDirectHook = {};
            R13PresentR9Hook = {};
            R13DrawPrimitiveR9Hook = {};
            R13DrawIndexedPrimitiveR9Hook = {};
            R13DrawPrimitiveUPR9Hook = {};
            R13DrawIndexedPrimitiveUPR9Hook = {};
            R13InstallState.store(R13InstallFailed, std::memory_order_release);
        }

        bool R13EnableOverlayHooks() noexcept
        {
            if (!R13ResetR9Hook.enable()) return false;
            if (!R13ResolveDirectHook.enable()) return false;
            if (!R13PresentR9Hook.enable()) return false;
            if (!R13DrawPrimitiveR9Hook.enable()) return false;
            if (!R13DrawIndexedPrimitiveR9Hook.enable()) return false;
            if (!R13DrawPrimitiveUPR9Hook.enable()) return false;
            if (!R13DrawIndexedPrimitiveUPR9Hook.enable()) return false;
            return true;
        }

        DWORD WINAPI R13StereoInstallThread(void*)
        {
            R13OverlayReady.store(false, std::memory_order_release);
            R13InstallState.store(R13InstallPending, std::memory_order_release);
            for (int attempt = 0; attempt < 4800; ++attempt)
            {
                const std::uint32_t r9State = R9InstallState.load(std::memory_order_acquire);
                if (r9State == R9InstallFailed)
                {
                    R13OverlayReady.store(false, std::memory_order_release);
                    R13InstallState.store(R13InstallFailed, std::memory_order_release);
                    spdlog::error(
                        "VR R13: R9 callback transaction failed; hardening overlay not installed");
                    return 0;
                }
                if (r9State == R9InstallReady)
                {
                    constexpr auto disabled = safetyhook::InlineHook::StartDisabled;
                    R13ResetR9Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&ResetDestR9), ResetDestR13, disabled);
                    R13ResolveDirectHook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&ResolveDirectTransport),
                        ResolveDirectTransportR13, disabled);
                    R13PresentR9Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&PresentDestR9), PresentDestR13, disabled);
                    R13DrawPrimitiveR9Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveDestR9),
                        DrawPrimitiveDestR13, disabled);
                    R13DrawIndexedPrimitiveR9Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveDestR9),
                        DrawIndexedPrimitiveDestR13, disabled);
                    R13DrawPrimitiveUPR9Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveUPDestR9),
                        DrawPrimitiveUPDestR13, disabled);
                    R13DrawIndexedPrimitiveUPR9Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveUPDestR9),
                        DrawIndexedPrimitiveUPDestR13, disabled);

                    const bool created =
                        R13ResetR9Hook && R13ResolveDirectHook &&
                        R13PresentR9Hook && R13DrawPrimitiveR9Hook &&
                        R13DrawIndexedPrimitiveR9Hook && R13DrawPrimitiveUPR9Hook &&
                        R13DrawIndexedPrimitiveUPR9Hook;
                    const bool enabled = created && R13EnableOverlayHooks();

                    if (enabled)
                    {
                        R13OverlayReady.store(true, std::memory_order_release);
                        R13InstallState.store(R13InstallReady, std::memory_order_release);
                        spdlog::info(
                            "VR R13: stereo hardening ACTIVE; disabled-first transactional hooks=READY + atomic R7/R9 install handoff + draw-time fragile-effect validation + single ResetEx owner + GPU-completion direct-ring backpressure + single-execution MRT/occlusion fallback");
                    }
                    else
                    {
                        R13RollbackOverlayHooks();
                        spdlog::error(
                            "VR R13: overlay hook create/enable transaction was partial; all R13 overlay hooks rolled back immediately");
                    }
                    return 0;
                }
                Sleep(25);
            }

            R13OverlayReady.store(false, std::memory_order_release);
            R13InstallState.store(R13InstallFailed, std::memory_order_release);
            spdlog::warn(
                "VR R13: R9 transactional install did not become ready; hardening overlay not installed");
            return 0;
        }

        class VRStereoR13HardeningHook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRStereoR13Hardening";
            }
            bool validate() override { return true; }
            bool apply() override
            {
                R13OverlayReady.store(false, std::memory_order_release);
                HANDLE thread = CreateThread(
                    nullptr, 0, R13StereoInstallThread, nullptr, 0, nullptr);
                if (!thread)
                {
                    R13InstallState.store(R13InstallFailed, std::memory_order_release);
                    return false;
                }
                CloseHandle(thread);
                return true;
            }
            static VRStereoR13HardeningHook instance;
        };

        VRStereoR13HardeningHook VRStereoR13HardeningHook::instance;
    }

    PoseInjectionSnapshot CurrentPoseInjectionSnapshot() noexcept
    {
        // Sample all mutable D3D9 pass signals exactly once. The legacy invariant is
        // intentionally calculated from the raw signals rather than from the
        // enum result, so the checked classifier can catch future policy drift
        // without observing two different moments of D3D9 state.
        const bool internalStereo = InternalStereoPass;
        const bool mainBackbuffer = TargetIsBackBuffer();
        const bool auxiliaryRenderTargetActive = AnyAuxRenderTargetActive();

        PoseInjectionSnapshot snapshot{};
        snapshot.policy = OutRunVR::PassPolicy::ClassifyPoseInjection(
            internalStereo, mainBackbuffer, auxiliaryRenderTargetActive);
        snapshot.legacyMainBackbufferInvariant =
            !internalStereo && mainBackbuffer && !auxiliaryRenderTargetActive;
        return snapshot;
    }

    OutRunVR::PassPolicy::PoseInjectionPolicy CurrentPoseInjectionPolicy() noexcept
    {
        return CurrentPoseInjectionSnapshot().policy;
    }

    bool IsMainBackbufferPoseInjectionPass() noexcept
    {
        return CurrentPoseInjectionSnapshot().legacyMainBackbufferInvariant;
    }
}
