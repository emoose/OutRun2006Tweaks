// R31 high-draw-count performance overlay.
//
// R29 removed the steady-state mono replay, but the validated R7 world path
// still read c64..c67 and rebuilt all eye transforms for every draw. Complex
// OutRun stages can submit several thousand top-level draws per Present, so the
// validation itself becomes measurable even after the third geometry pass is
// gone.
//
// R31 keeps R29/R30 safety policy and changes only the proven stable hot path:
//  * verified c64..c67 from the renderer hook is the authoritative stock WVP;
//  * shader epoch + pose sequence must still match exactly;
//  * eyeInverse*eyeProjection is cached per pose/projection/world-scale;
//  * one live c64 validation is retained every 16 fast world draws only after
//    StateBlock interception is proven; otherwise every candidate is validated;
//  * Begin/End/Apply invalidate and resynchronize WVP, projection, shader,
//    effect, viewport and scissor caches as one state generation;
//  * HUD draws use R30 math but an explicit {handled,hr} result, removing the
//    E_NOTIMPL sentinel/double-draw ambiguity;
//  * a five-second route summary separates main/offscreen/aux/world/HUD/fallback
//    work so stage-specific 300 -> 3000+ draw explosions can be diagnosed.

#include "stereo_renderer_r30.cpp"

namespace OutRunVRStereo
{
    namespace
    {
        constexpr std::size_t CreateStateBlockVtableIndex = 59;
        constexpr std::size_t BeginStateBlockVtableIndex = 60;
        constexpr std::size_t EndStateBlockVtableIndex = 61;
        constexpr std::size_t StateBlockApplyVtableIndex = 5;
        constexpr std::uint64_t LiveWvpValidationInterval = 16;

        SafetyHookInline R31DrawPrimitiveR30Hook{};
        SafetyHookInline R31DrawIndexedPrimitiveR30Hook{};
        SafetyHookInline R31DrawPrimitiveUPR30Hook{};
        SafetyHookInline R31DrawIndexedPrimitiveUPR30Hook{};
        SafetyHookInline R31CreateStateBlockHook{};
        SafetyHookInline R31BeginStateBlockHook{};
        SafetyHookInline R31EndStateBlockHook{};
        SafetyHookInline R31StateBlockApplyHook{};
        void* R31StateBlockApplyTarget = nullptr;

        std::atomic<OutRunVR::RuntimeEligibility::InstallState> R31InstallState{
            OutRunVR::RuntimeEligibility::InstallState::Pending };

        std::uint32_t R31BlockedVerifiedGeneration = 0;
        std::uint64_t R31FastWorldCandidates = 0;
        std::uint64_t R31FastWorldDraws = 0;
        std::uint64_t R31FastWorldLiveValidations = 0;
        std::uint64_t R31FastWorldValidationRejects = 0;
        std::uint64_t R31StateBlockApplies = 0;
        std::uint64_t R31StateBlockRecordings = 0;
        std::uint64_t R31HudDraws = 0;
        std::atomic<bool> R31StateBlockTrackingReliable{ false };
        std::atomic<bool> R31StateBlockCoverageLost{ false };
        thread_local bool R31StateBlockResyncPending = false;
        thread_local bool R31StateBlockRecording = false;
        bool R31FirstFastWorldLogged = false;
        bool R31FirstStateBlockLogged = false;
        bool R31FirstAlternateStateBlockLogged = false;
        bool R31FirstHudLogged = false;

        struct R31EyeTailCache
        {
            bool valid = false;
            std::uint32_t poseSequence = 0;
            float worldScale = 0.0f;
            D3DMATRIX projection{};
            D3DMATRIX inverseProjection{};
            D3DMATRIX eyeTail[2]{};
        };
        R31EyeTailCache R31EyeCache{};

        struct R31FramePerf
        {
            std::uint64_t epoch = 0;
            std::uint64_t draws = 0;
            std::uint64_t main = 0;
            std::uint64_t offscreen = 0;
            std::uint64_t aux = 0;
            std::uint64_t fastWorld = 0;
            std::uint64_t hud = 0;
            std::uint64_t fragile = 0;
            std::uint64_t unstable = 0;
            std::uint64_t fallback = 0;
        };
        R31FramePerf R31Frame{};

        struct R31WindowPerf
        {
            std::uint64_t presents = 0;
            std::uint64_t draws = 0;
            std::uint64_t main = 0;
            std::uint64_t offscreen = 0;
            std::uint64_t aux = 0;
            std::uint64_t fastWorld = 0;
            std::uint64_t hud = 0;
            std::uint64_t fragile = 0;
            std::uint64_t unstable = 0;
            std::uint64_t fallback = 0;
            std::uint64_t maxDraws = 0;
            ULONGLONG lastLogMs = 0;
        };
        R31WindowPerf R31Window{};

        void R31FinalizePerfFrame() noexcept
        {
            if (R31Frame.epoch == 0 || R31Frame.draws == 0)
                return;
            ++R31Window.presents;
            R31Window.draws += R31Frame.draws;
            R31Window.main += R31Frame.main;
            R31Window.offscreen += R31Frame.offscreen;
            R31Window.aux += R31Frame.aux;
            R31Window.fastWorld += R31Frame.fastWorld;
            R31Window.hud += R31Frame.hud;
            R31Window.fragile += R31Frame.fragile;
            R31Window.unstable += R31Frame.unstable;
            R31Window.fallback += R31Frame.fallback;
            R31Window.maxDraws = std::max(R31Window.maxDraws, R31Frame.draws);

            const ULONGLONG now = GetTickCount64();
            if (R31Window.lastLogMs == 0)
                R31Window.lastLogMs = now;
            if (Settings::VRTelemetry && now - R31Window.lastLogMs >= 5000 &&
                R31Window.presents != 0)
            {
                const double avg = static_cast<double>(R31Window.draws) /
                    static_cast<double>(R31Window.presents);
                spdlog::info(
                    "VR R31 PERF: topLevelGameCalls/present avg={:.1f} max={} presents={} targets[main={},offscreen={},auxOverlay={}] ownedEyeRoutes[fastWorld={},hud={}] fallbackReasons[fragile={},unstable={}] fallbackDispatch={} liveWvpCheck={} liveReject={} stateBlock[record={},apply={}]",
                    avg, R31Window.maxDraws, R31Window.presents,
                    R31Window.main, R31Window.offscreen, R31Window.aux,
                    R31Window.fastWorld, R31Window.hud, R31Window.fragile,
                    R31Window.unstable, R31Window.fallback,
                    R31FastWorldLiveValidations, R31FastWorldValidationRejects,
                    R31StateBlockRecordings, R31StateBlockApplies);
                R31Window = {};
                R31Window.lastLogMs = now;
            }
        }

        void R31ObserveDraw(IDirect3DDevice9* device) noexcept
        {
            if (!IsGameDevice(device) || InternalStereoPass)
                return;
            if (R31Frame.epoch != PresentEpoch)
            {
                R31FinalizePerfFrame();
                R31Frame = {};
                R31Frame.epoch = PresentEpoch;
            }
            ++R31Frame.draws;
            if (TargetIsBackBuffer()) ++R31Frame.main;
            else ++R31Frame.offscreen;
            if (AnyAuxRenderTargetActive()) ++R31Frame.aux;
        }

        bool R31GetSavedViewport(IDirect3DDevice9* device,
            D3DVIEWPORT9& viewport) noexcept
        {
            if (R22ShadowState.Valid())
            {
                viewport = R22ShadowState.viewport;
                return true;
            }
            return device && SUCCEEDED(device->GetViewport(&viewport));
        }

        bool R31ProjectionMatches(const D3DMATRIX& a,
            const D3DMATRIX& b) noexcept
        {
            return std::memcmp(&a, &b, sizeof(D3DMATRIX)) == 0;
        }

        bool R31LiveShaderMatches(IDirect3DDevice9* device,
            std::uintptr_t expected) noexcept
        {
            if (!device || expected == 0)
                return false;
            IDirect3DVertexShader9* shader = nullptr;
            if (FAILED(device->GetVertexShader(&shader)))
                return false;
            const std::uintptr_t actual =
                reinterpret_cast<std::uintptr_t>(shader);
            if (shader) shader->Release();
            return actual == expected;
        }

        void R31DiscardUnreliableDrawCaches() noexcept
        {
            if (R31StateBlockTrackingReliable.load(std::memory_order_acquire))
                return;
            R29Effect.valid = false;
            R22ShadowState = {};
            R23LastStateSampleDrawSerial = 0;
            R23LastStateSampleEpoch = 0;
        }

        bool R31PrepareEyeTailCache(
            const OutRunVRRenderer::LatchedStereoFrame& stereo,
            const D3DMATRIX& projection,
            const D3DMATRIX& inverseProjection) noexcept
        {
            const float worldScale = Settings::VRWorldScale;
            if (R31EyeCache.valid &&
                R31EyeCache.poseSequence == stereo.poseSequence &&
                R31EyeCache.worldScale == worldScale &&
                R31ProjectionMatches(R31EyeCache.projection, projection))
                return true;

            R31EyeTailCache next{};
            next.poseSequence = stereo.poseSequence;
            next.worldScale = worldScale;
            next.projection = projection;
            next.inverseProjection = inverseProjection;
            for (int eye = 0; eye < 2; ++eye)
            {
                const D3DMATRIX eyePose = MatrixFromQuaternionTranslation(
                    stereo.eyeOrientation[eye], stereo.eyeOffset[eye], worldScale);
                const D3DMATRIX eyeInverse = InverseRigid(eyePose);
                const D3DMATRIX eyeProjection = ProjectionFromFov(
                    projection, stereo.eyeFov[eye]);
                next.eyeTail[eye] = MultiplyMatrix(eyeInverse, eyeProjection);
                if (!MatrixFinite(next.eyeTail[eye]))
                    return false;
            }
            next.valid = true;
            R31EyeCache = next;
            return true;
        }

        bool R31BuildFastWorldConstants(IDirect3DDevice9* device,
            const OutRunVRRenderer::LatchedStereoFrame& stereo,
            DrawStereoState& draw) noexcept
        {
            float verified[16]{};
            std::uint32_t generation = 0;
            std::uint32_t poseSequence = 0;
            std::uintptr_t verifiedShader = 0;
            std::uint64_t verifiedShaderSerial = 0;
            if (!OutRunVRRenderer::GetLastVerifiedWvp(verified, generation,
                    poseSequence, verifiedShader, verifiedShaderSerial))
                return false;
            if (!generation || generation == R31BlockedVerifiedGeneration ||
                poseSequence != stereo.poseSequence)
                return false;

            std::uintptr_t currentShader = 0;
            std::uint64_t currentShaderSerial = 0;
            if (!GetCurrentShaderEpoch(currentShader, currentShaderSerial) ||
                currentShader == 0 || currentShader != verifiedShader ||
                currentShaderSerial != verifiedShaderSerial)
                return false;

            if (!R31StateBlockTrackingReliable.load(std::memory_order_acquire) &&
                !R31LiveShaderMatches(device, verifiedShader))
                return false;

            ++R31FastWorldCandidates;
            float live[16]{};
            bool liveValidated = false;
            if (!R31StateBlockTrackingReliable.load(std::memory_order_acquire) ||
                (R31FastWorldCandidates % LiveWvpValidationInterval) == 0)
            {
                ++R31FastWorldLiveValidations;
                if (FAILED(device->GetVertexShaderConstantF(
                        OutRunWvpRegister, live, OutRunWvpRegisterCount)) ||
                    !FloatArrayNear(live, verified, 16, VerifiedWvpEpsilon))
                {
                    R31BlockedVerifiedGeneration = generation;
                    ++R31FastWorldValidationRejects;
                    return false;
                }
                liveValidated = true;
            }

            D3DMATRIX projection{};
            D3DMATRIX inverseProjection{};
            float verifiedProjection[16]{};
            std::uint32_t projectionGeneration = 0;
            std::uint32_t projectionPoseSequence = 0;
            if (!OutRunVRRenderer::GetR28VerifiedProjection(
                    verifiedProjection, projectionGeneration,
                    projectionPoseSequence) ||
                projectionGeneration != generation ||
                projectionPoseSequence != poseSequence)
                return false;
            std::memcpy(&projection, verifiedProjection, sizeof(projection));
            if (!MatrixFinite(projection) ||
                !GetInverseProjection(projection, inverseProjection) ||
                !R31PrepareEyeTailCache(stereo, projection, inverseProjection))
                return false;

            std::memcpy(draw.originalConstants,
                liveValidated ? live : verified, sizeof(verified));
            D3DMATRIX uploadedT{};
            std::memcpy(&uploadedT, verified, sizeof(uploadedT));
            const D3DMATRIX currentWvp = TransposeMatrix(uploadedT);
            const D3DMATRIX correctedWorldView = MultiplyMatrix(
                currentWvp, R31EyeCache.inverseProjection);
            if (!MatrixFinite(correctedWorldView))
                return false;

            for (int eye = 0; eye < 2; ++eye)
            {
                const D3DMATRIX eyeWvp = MultiplyMatrix(
                    correctedWorldView, R31EyeCache.eyeTail[eye]);
                if (!MatrixFinite(eyeWvp))
                    return false;
                const D3DMATRIX eyeWvpT = TransposeMatrix(eyeWvp);
                std::memcpy(draw.eyeConstants[eye], &eyeWvpT,
                    sizeof(eyeWvpT));
            }
            draw.worldStereo = true;
            draw.poseSequence = stereo.poseSequence;
            draw.stereoFrame = stereo;
            return true;
        }

        struct R31OwnedResult
        {
            bool handled = false;
            HRESULT hr = D3D_OK;
        };

        template <typename ActualDraw>
        R31OwnedResult R31TryFastWorld(IDirect3DDevice9* device,
            ActualDraw&& actualDraw, const char* site)
        {
            if (R31StateBlockRecording || !R29StableStereoBase(device))
            {
                if (IsGameDevice(device) && !InternalStereoPass && TargetIsBackBuffer())
                    ++R31Frame.unstable;
                return {};
            }

            bool fragile = true;
            if (!R29FragileEffectCached(device, fragile))
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
            if (!R31GetSavedViewport(device, savedViewport))
                return {};

            bool leftWvpOk = false;
            {
                InternalPassScope guard;
                leftWvpOk = SetWvpOneRegisterAtATime(
                    device, draw.eyeConstants[0]);
            }
            if (!leftWvpOk)
            {
                bool rolledBack = false;
                {
                    InternalPassScope guard;
                    rolledBack = SetWvpOneRegisterAtATime(
                        device, draw.originalConstants);
                }
                if (!rolledBack)
                {
                    R9Poison(OutRunVR::StereoFailureRestoreFailed,
                        "R31/fast-left-WVP-rollback");
                    NoteRestoreFailure("R31 fast left-eye c64 rollback");
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
                    restored = SetWvpOneRegisterAtATime(
                        device, draw.originalConstants);
                }
                if (!restored) NoteRestoreFailure("R31 fast left draw c64");
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
                if (SUCCEEDED(rightHr) && !SetWvpOneRegisterAtATime(
                        device, draw.eyeConstants[1]))
                {
                    rightFailure = OutRunVR::StereoFailureRightWvpUploadFailed;
                    rightHr = E_FAIL;
                }
                if (SUCCEEDED(rightHr))
                {
                    rightFailure = OutRunVR::StereoFailureRightDrawFailed;
                    rightHr = actualDraw();
                }
                restoreOk = RestoreRightPassState(device, savedRt, savedDepth,
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

            if (!R31FirstFastWorldLogged)
            {
                R31FirstFastWorldLogged = true;
                spdlog::info(
                    "VR R31 PERF: verified world hot path ACTIVE; steady draws use renderer-authoritative c64 + cached eye tails, live D3D c64 validation every {} draws",
                    LiveWvpValidationInterval);
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
                NoteRestoreFailure("R31 fast right-eye draw");
                R29ArmMonoSafety();
            }
            return result;
        }

        template <typename ActualDraw>
        R31OwnedResult R31TryHud(IDirect3DDevice9* device,
            ActualDraw&& actualDraw, const char* site)
        {
            if (R31StateBlockRecording || !R29StableStereoBase(device) ||
                !R30CurrentPassIsScreenSpace2D())
                return {};
            if (!R31StateBlockTrackingReliable.load(std::memory_order_acquire))
            {
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
            if (!R31GetSavedViewport(device, savedViewport))
                return {};

            bool leftWvpOk = false;
            {
                InternalPassScope guard;
                leftWvpOk = SetWvpOneRegisterAtATime(device, eyeConstants[0]);
            }
            if (!leftWvpOk)
            {
                bool rolledBack = false;
                {
                    InternalPassScope guard;
                    rolledBack = SetWvpOneRegisterAtATime(device, original);
                }
                if (!rolledBack)
                {
                    R9Poison(OutRunVR::StereoFailureRestoreFailed,
                        "R31/HUD-left-WVP-rollback");
                    NoteRestoreFailure("R31 HUD left-eye c64 rollback");
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
                    restored = SetWvpOneRegisterAtATime(device, original);
                }
                R9Poison(OutRunVR::StereoFailureLeftDrawFailed, site, result.hr);
                if (!restored) NoteRestoreFailure("R31 HUD left draw c64");
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
                    !SetWvpOneRegisterAtATime(device, eyeConstants[1]))
                {
                    rightFailure = OutRunVR::StereoFailureRightWvpUploadFailed;
                    rightHr = E_FAIL;
                }
                if (SUCCEEDED(rightHr))
                {
                    rightFailure = OutRunVR::StereoFailureRightDrawFailed;
                    rightHr = actualDraw();
                }
                restoreOk = RestoreRightPassState(device, savedRt, savedDepth,
                    savedViewport, original, true);
            }

            FrameHadDuplicatedDraw = true;
            ++DuplicatedDraws;
            ++NonWorldDuplicatedDraws;
            ++R29StableTwoEyeDraws;
            ++R30ScreenSpaceFovDraws;
            ++R31HudDraws;
            ++R31Frame.hud;

            if (!R31FirstHudLogged)
            {
                R31FirstHudLogged = true;
                spdlog::info(
                    "VR R31 HUD: explicit handled-result path ACTIVE; E_NOTIMPL can no longer trigger a second draw after an owned HUD submission");
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
                NoteRestoreFailure("R31 HUD right-eye draw");
                R29ArmMonoSafety();
            }
            return result;
        }

        template <typename ActualDraw, typename R29Draw>
        HRESULT R31Dispatch(IDirect3DDevice9* device, ActualDraw&& actualDraw,
            R29Draw&& r29Draw, const char* site)
        {
            R31ObserveDraw(device);
            R31DiscardUnreliableDrawCaches();

            if (R31StateBlockRecording)
            {
                ++R31Frame.fallback;
                return actualDraw();
            }

            if (R30CurrentPassIsScreenSpace2D())
            {
                const auto hud = R31TryHud(device,
                    std::forward<ActualDraw>(actualDraw), site);
                if (hud.handled)
                    return hud.hr;
            }
            else
            {
                const auto fast = R31TryFastWorld(device,
                    std::forward<ActualDraw>(actualDraw), site);
                if (fast.handled)
                    return fast.hr;
            }

            ++R31Frame.fallback;
            return r29Draw();
        }

        HRESULT __stdcall DrawPrimitiveDestR31(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
        {
            auto actual = [&]() {
                return DrawPrimitiveHook.stdcall<HRESULT>(
                    device, type, startVertex, primitiveCount);
            };
            auto r29 = [&]() {
                return R30DrawPrimitiveR29Hook.stdcall<HRESULT>(
                    device, type, startVertex, primitiveCount);
            };
            return R31Dispatch(device, actual, r29, "R31/DrawPrimitive");
        }

        HRESULT __stdcall DrawIndexedPrimitiveDestR31(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            INT baseVertexIndex, UINT minVertexIndex, UINT numVertices,
            UINT startIndex, UINT primitiveCount)
        {
            auto actual = [&]() {
                return DrawIndexedPrimitiveHook.stdcall<HRESULT>(device, type,
                    baseVertexIndex, minVertexIndex, numVertices, startIndex,
                    primitiveCount);
            };
            auto r29 = [&]() {
                return R30DrawIndexedPrimitiveR29Hook.stdcall<HRESULT>(device, type,
                    baseVertexIndex, minVertexIndex, numVertices, startIndex,
                    primitiveCount);
            };
            return R31Dispatch(device, actual, r29,
                "R31/DrawIndexedPrimitive");
        }

        HRESULT __stdcall DrawPrimitiveUPDestR31(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data,
            UINT stride)
        {
            auto actual = [&]() {
                return DrawPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, primitiveCount, data, stride);
            };
            auto r29 = [&]() {
                return R30DrawPrimitiveUPR29Hook.stdcall<HRESULT>(
                    device, type, primitiveCount, data, stride);
            };
            return R31Dispatch(device, actual, r29, "R31/DrawPrimitiveUP");
        }

        HRESULT __stdcall DrawIndexedPrimitiveUPDestR31(
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
            auto r29 = [&]() {
                return R30DrawIndexedPrimitiveUPR29Hook.stdcall<HRESULT>(device,
                    type, minVertexIndex, numVertices, primitiveCount,
                    indexData, indexFormat, vertexData, stride);
            };
            return R31Dispatch(device, actual, r29,
                "R31/DrawIndexedPrimitiveUP");
        }

        void R31BlockCurrentVerifiedGeneration() noexcept
        {
            float ignored[16]{};
            std::uint32_t generation = 0, pose = 0;
            std::uintptr_t shader = 0;
            std::uint64_t serial = 0;
            if (OutRunVRRenderer::GetLastVerifiedWvp(
                    ignored, generation, pose, shader, serial))
                R31BlockedVerifiedGeneration = generation;
            R31EyeCache.valid = false;
        }

        void R31ResynchronizeShaderEpoch(IDirect3DDevice9* device) noexcept
        {
            IDirect3DVertexShader9* shader = nullptr;
            const HRESULT hr = device
                ? device->GetVertexShader(&shader) : D3DERR_INVALIDCALL;
            const std::uintptr_t identity = SUCCEEDED(hr)
                ? reinterpret_cast<std::uintptr_t>(shader) : 0;
            if (shader) shader->Release();

            const std::uintptr_t previous =
                CurrentVertexShaderIdentity.exchange(identity,
                    std::memory_order_acq_rel);
            if (previous != identity)
            {
                std::uint64_t serial = VertexShaderSerial.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
                if (serial == 0)
                    VertexShaderSerial.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        void R31MarkStateBlockCachesDirty() noexcept
        {
            R31BlockCurrentVerifiedGeneration();
            OutRunVRRenderer::R29InvalidateRendererStateAfterExternalRestore();
            R29Effect = {};
            R22ShadowState = {};
            R23LastStateSampleDrawSerial = 0;
            R23LastStateSampleEpoch = 0;
            R31EyeCache.valid = false;
            R31StateBlockResyncPending = true;
        }

        void R31FlushPendingStateBlockResync(IDirect3DDevice9* device) noexcept
        {
            if (!R31StateBlockResyncPending || !device)
                return;
            R31StateBlockResyncPending = false;
            R31ResynchronizeShaderEpoch(device);
            if (!R22PrimeShadowState(device))
            {
                R31StateBlockTrackingReliable.store(false,
                    std::memory_order_release);
                R31StateBlockCoverageLost.store(true,
                    std::memory_order_release);
            }
        }

        HRESULT __stdcall StateBlockApplyDestR31(IDirect3DStateBlock9* block)
        {
            const HRESULT hr = R31StateBlockApplyHook.stdcall<HRESULT>(block);
            if (!block)
                return hr;
            IDirect3DDevice9* device = nullptr;
            if (SUCCEEDED(block->GetDevice(&device)) && device)
            {
                const bool game = IsGameDevice(device);
                if (game)
                {
                    ++R31StateBlockApplies;
                    R31MarkStateBlockCachesDirty();
                    if (!R31FirstStateBlockLogged)
                    {
                        R31FirstStateBlockLogged = true;
                        spdlog::info(
                            "VR R31 STATE: StateBlock::Apply observed hr=0x{:08x}; caches invalidate immediately and live D3D state is lazily re-primed at the next actual draw",
                            static_cast<unsigned>(hr));
                    }
                }
                device->Release();
            }
            else
            {
                R31StateBlockCoverageLost.store(true,
                    std::memory_order_release);
                R31StateBlockTrackingReliable.store(false,
                    std::memory_order_release);
            }
            return hr;
        }

        bool R31EnsureStateBlockApplyHook(IDirect3DStateBlock9* block) noexcept
        {
            if (!block)
            {
                R31StateBlockCoverageLost.store(true,
                    std::memory_order_release);
                R31StateBlockTrackingReliable.store(false,
                    std::memory_order_release);
                return false;
            }
            void** vtable = *reinterpret_cast<void***>(block);
            if (!vtable)
            {
                R31StateBlockCoverageLost.store(true,
                    std::memory_order_release);
                R31StateBlockTrackingReliable.store(false,
                    std::memory_order_release);
                return false;
            }

            if (R31StateBlockApplyHook)
            {
                const bool reliable = R31CreateStateBlockHook &&
                    R31BeginStateBlockHook && R31EndStateBlockHook &&
                    !R31StateBlockCoverageLost.load(std::memory_order_acquire) &&
                    R31StateBlockApplyTarget ==
                        vtable[StateBlockApplyVtableIndex];
                if (!reliable && R31StateBlockApplyTarget !=
                        vtable[StateBlockApplyVtableIndex])
                {
                    R31StateBlockCoverageLost.store(true,
                        std::memory_order_release);
                    if (!R31FirstAlternateStateBlockLogged)
                    {
                        R31FirstAlternateStateBlockLogged = true;
                        spdlog::warn(
                            "VR R31 STATE: alternate StateBlock::Apply implementation observed; fast-path cache trust is disabled for the process");
                    }
                }
                R31StateBlockTrackingReliable.store(reliable,
                    std::memory_order_release);
                return reliable;
            }
            R31StateBlockApplyHook = safetyhook::create_inline(
                vtable[StateBlockApplyVtableIndex], StateBlockApplyDestR31,
                safetyhook::InlineHook::StartDisabled);
            if (!R31StateBlockApplyHook ||
                !R31StateBlockApplyHook.enable().has_value())
            {
                R31StateBlockApplyHook = {};
                R31StateBlockApplyTarget = nullptr;
                R31StateBlockCoverageLost.store(true,
                    std::memory_order_release);
                R31StateBlockTrackingReliable.store(false,
                    std::memory_order_release);
                spdlog::warn(
                    "VR R31 STATE: could not hook StateBlock::Apply; per-draw live WVP/shader/render-state validation remains active");
                return false;
            }
            R31StateBlockApplyTarget =
                vtable[StateBlockApplyVtableIndex];
            const bool reliable = R31CreateStateBlockHook &&
                R31BeginStateBlockHook && R31EndStateBlockHook &&
                !R31StateBlockCoverageLost.load(std::memory_order_acquire);
            R31StateBlockTrackingReliable.store(reliable,
                std::memory_order_release);
            return reliable;
        }

        HRESULT __stdcall CreateStateBlockDestR31(IDirect3DDevice9* device,
            D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** block)
        {
            const HRESULT hr = R31CreateStateBlockHook.stdcall<HRESULT>(
                device, type, block);
            if (SUCCEEDED(hr) && IsGameDevice(device) && block && *block)
                R31EnsureStateBlockApplyHook(*block);
            return hr;
        }

        HRESULT __stdcall BeginStateBlockDestR31(IDirect3DDevice9* device)
        {
            const HRESULT hr = R31BeginStateBlockHook.stdcall<HRESULT>(device);
            if (SUCCEEDED(hr) && IsGameDevice(device) && !InternalStereoPass)
            {
                R31StateBlockRecording = true;
                ++R31StateBlockRecordings;
                R31MarkStateBlockCachesDirty();
            }
            return hr;
        }

        HRESULT __stdcall EndStateBlockDestR31(IDirect3DDevice9* device,
            IDirect3DStateBlock9** block)
        {
            const HRESULT hr = R31EndStateBlockHook.stdcall<HRESULT>(device, block);
            if (IsGameDevice(device) &&
                (!InternalStereoPass || R31StateBlockRecording))
            {
                if (SUCCEEDED(hr))
                {
                    R31StateBlockRecording = false;
                }
                else if (R31StateBlockRecording)
                {
                    R31StateBlockCoverageLost.store(true,
                        std::memory_order_release);
                    R31StateBlockTrackingReliable.store(false,
                        std::memory_order_release);
                }
                R31MarkStateBlockCachesDirty();
                if (SUCCEEDED(hr) && block && *block)
                    R31EnsureStateBlockApplyHook(*block);
            }
            return hr;
        }

        void R31RollbackDrawHooks() noexcept
        {
            R31DrawIndexedPrimitiveUPR30Hook = {};
            R31DrawPrimitiveUPR30Hook = {};
            R31DrawIndexedPrimitiveR30Hook = {};
            R31DrawPrimitiveR30Hook = {};
        }

        bool R31EnableDrawHooks() noexcept
        {
            SafetyHookInline* hooks[]{
                &R31DrawPrimitiveR30Hook,
                &R31DrawIndexedPrimitiveR30Hook,
                &R31DrawPrimitiveUPR30Hook,
                &R31DrawIndexedPrimitiveUPR30Hook
            };
            for (auto* hook : hooks)
                if (!*hook || !hook->enable().has_value())
                    return false;
            return true;
        }

        DWORD WINAPI R31InstallThread(void*)
        {
            using State = OutRunVR::RuntimeEligibility::InstallState;
            R31InstallState.store(State::Pending, std::memory_order_release);

            for (int attempt = 0; attempt < 4800; ++attempt)
            {
                const auto r30 = R30InstallState.load(std::memory_order_acquire);
                const auto renderer = OutRunVRRenderer::R29RendererState();
                if (r30 == State::Failed || renderer == State::Failed)
                {
                    R31InstallState.store(State::Failed, std::memory_order_release);
                    HookManager::ReportAsyncResult("OpenXRVRStereoR31Perf", false);
                    spdlog::error(
                        "VR R31 PERF: R30 or renderer prerequisite failed; R30 remains authoritative");
                    return 0;
                }

                if (r30 == State::Ready && renderer == State::Ready)
                {
                    const auto disabled = safetyhook::InlineHook::StartDisabled;
                    R31DrawPrimitiveR30Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveDestR30),
                        DrawPrimitiveDestR31, disabled);
                    R31DrawIndexedPrimitiveR30Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveDestR30),
                        DrawIndexedPrimitiveDestR31, disabled);
                    R31DrawPrimitiveUPR30Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveUPDestR30),
                        DrawPrimitiveUPDestR31, disabled);
                    R31DrawIndexedPrimitiveUPR30Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveUPDestR30),
                        DrawIndexedPrimitiveUPDestR31, disabled);

                    if (!R31EnableDrawHooks())
                    {
                        R31RollbackDrawHooks();
                        R31InstallState.store(State::Failed,
                            std::memory_order_release);
                        HookManager::ReportAsyncResult(
                            "OpenXRVRStereoR31Perf", false);
                        return 0;
                    }

                    IDirect3DDevice9* const device =
                        StereoInstalledDevice.load(std::memory_order_acquire);
                    R31StateBlockTrackingReliable.store(false,
                        std::memory_order_release);
                    R31StateBlockCoverageLost.store(false,
                        std::memory_order_release);
                    if (device)
                    {
                        void** vtable = *reinterpret_cast<void***>(device);
                        if (vtable)
                        {
                            R31CreateStateBlockHook = safetyhook::create_inline(
                                vtable[CreateStateBlockVtableIndex],
                                CreateStateBlockDestR31, disabled);
                            R31BeginStateBlockHook = safetyhook::create_inline(
                                vtable[BeginStateBlockVtableIndex],
                                BeginStateBlockDestR31, disabled);
                            R31EndStateBlockHook = safetyhook::create_inline(
                                vtable[EndStateBlockVtableIndex],
                                EndStateBlockDestR31, disabled);
                            const bool stateHooks = R31CreateStateBlockHook &&
                                R31BeginStateBlockHook &&
                                R31EndStateBlockHook &&
                                // Arm End before Begin so a render-thread race
                                // can never observe an unmatched recording start.
                                R31EndStateBlockHook.enable().has_value() &&
                                R31BeginStateBlockHook.enable().has_value() &&
                                R31CreateStateBlockHook.enable().has_value();
                            if (!stateHooks)
                            {
                                R31StateBlockCoverageLost.store(true,
                                    std::memory_order_release);
                                R31CreateStateBlockHook = {};
                                R31BeginStateBlockHook = {};
                                R31EndStateBlockHook = {};
                                spdlog::warn(
                                    "VR R31 STATE: Begin/Create/End StateBlock hooks unavailable; every fast-path candidate will live-validate WVP, shader, render state and viewport");
                            }
                            else
                            {
                                spdlog::info(
                                    "VR R31 STATE: Begin/Create/End StateBlock recording hooks armed; per-draw validation remains active until Apply interception is proven");
                            }
                        }
                    }

                    R31InstallState.store(State::Ready, std::memory_order_release);
                    HookManager::ReportAsyncResult("OpenXRVRStereoR31Perf", true);
                    spdlog::info(
                        "VR R31 PERF: cached world stereo + draw-route telemetry READY; R30 HUD sentinel path superseded");
                    return 0;
                }
                Sleep(25);
            }

            R31InstallState.store(State::Failed, std::memory_order_release);
            HookManager::ReportAsyncResult("OpenXRVRStereoR31Perf", false);
            spdlog::error("VR R31 PERF: timed out waiting for R30");
            return 0;
        }

        class VRStereoR31PerfHook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRStereoR31Perf";
            }
            bool validate() override { return true; }
            bool apply() override
            {
                HANDLE thread = CreateThread(nullptr, 0,
                    R31InstallThread, nullptr, 0, nullptr);
                if (!thread)
                {
                    R31InstallState.store(
                        OutRunVR::RuntimeEligibility::InstallState::Failed,
                        std::memory_order_release);
                    return false;
                }
                CloseHandle(thread);
                return true;
            }
            static VRStereoR31PerfHook instance;
        };

        VRStereoR31PerfHook VRStereoR31PerfHook::instance;
    }

    bool IsGameStateBlockRecording() noexcept
    {
        return R31StateBlockRecording;
    }

    bool IsStateBlockTrackingReliable() noexcept
    {
        return R31StateBlockTrackingReliable.load(std::memory_order_acquire);
    }
}
