// R23/R25 recovery + first-seed coordinator.
//
// This remains the final game-side TU layered over R22. Recovery is intentionally
// passive: while the common gate is closed, the game's Clear executes exactly
// once against the real game targets. No mono/right-eye clear replay and no
// viewport/scissor re-application is allowed during passive observation.
//
// First-seed authority also lives here. R20 and R22 are still retained for the
// already-seeded stereo frame, but they no longer independently approve/cancel
// the first seed of a Present. The coordinator snapshots LIVE game state before
// a candidate clear, counts every top-level game draw (including gate-closed
// draws), and initializes VR surfaces only after a next-frame pose warmup and a
// new current-frame full color/depth baseline are both proven.

#include "stereo_renderer_r22.cpp"

namespace OutRunVRStereo
{
    namespace
    {
        SafetyHookInline R23ClearR22Hook{};
        SafetyHookInline R23DrawPrimitiveR22Hook{};
        SafetyHookInline R23DrawIndexedPrimitiveR22Hook{};
        SafetyHookInline R23DrawPrimitiveUPR22Hook{};
        SafetyHookInline R23DrawIndexedPrimitiveUPR22Hook{};
        SafetyHookInline R23SetRenderTargetR9Hook{};
        SafetyHookInline R23PresentR21Hook{};

        std::atomic<OutRunVR::RuntimeEligibility::InstallState> R23InstallState{
            OutRunVR::RuntimeEligibility::InstallState::Pending };

        std::uint64_t R23GameDrawSerial = 0;
        std::uint64_t R23LastStateSampleDrawSerial = 0;
        std::uint64_t R23LastStateSampleEpoch = 0;
        std::uint64_t R23LastPresentBeginScene = 0;

        std::uint64_t R23DepthClearEpoch = 0;
        std::uint64_t R23DepthClearGameDrawSerial = 0;
        std::uint64_t R23DepthClearGeneration = 0;
        float R23DepthClearZ = 1.0f;
        std::uint64_t R23StencilClearEpoch = 0;
        std::uint64_t R23StencilClearGameDrawSerial = 0;
        std::uint64_t R23StencilClearGeneration = 0;
        DWORD R23StencilClearValue = 0;

        bool R23ArmWarmupAtPresent = false;
        std::uint64_t R23PassiveClears = 0;
        std::uint64_t R23AuthoritativeSeeds = 0;
        std::uint64_t R23RecoveryVrSurfaceWrites = 0;
        std::uint64_t R23StateMismatches = 0;
        std::uint64_t R23StateCaptureFailures = 0;
        std::uint64_t R23BaselineRejects = 0;
        ULONGLONG R23LastSummaryMs = 0;
        ULONGLONG R23LastStateMismatchLogMs = 0;
        const char* R23LastRejectReason = "none";
        const char* R23LastHostReason = "unknown";
        bool R23FirstPassiveLogged = false;
        bool R23FirstWarmupArmedLogged = false;
        bool R23FirstSeedLogged = false;
        bool R23FirstColorBaselineLogged = false;
        bool R23FirstImplicitViewportResyncLogged = false;

        bool R23SnapshotSame(const R22ScissorSnapshot& a,
            const R22ScissorSnapshot& b) noexcept
        {
            if (!a.Valid() || !b.Valid())
                return false;
            return a.viewport.X == b.viewport.X &&
                a.viewport.Y == b.viewport.Y &&
                a.viewport.Width == b.viewport.Width &&
                a.viewport.Height == b.viewport.Height &&
                a.viewport.MinZ == b.viewport.MinZ &&
                a.viewport.MaxZ == b.viewport.MaxZ &&
                a.rect.left == b.rect.left && a.rect.top == b.rect.top &&
                a.rect.right == b.rect.right && a.rect.bottom == b.rect.bottom &&
                a.enabled == b.enabled;
        }

        bool R23CaptureActualGameState(IDirect3DDevice9* device,
            R22ScissorSnapshot& out, const char* site, bool force) noexcept
        {
            // Once StateBlock::Apply interception is actually proven,
            // setter + Apply boundaries own the shadow and effect draws no
            // longer need periodic GetViewport/GetScissorRect queries. Keep
            // forced live reads for SetRenderTarget implicit viewport changes.
            const bool effectForce =
                force && site && std::strcmp(site, "R28EffectDraw") == 0;
            if (R22ShadowState.Valid() &&
                R22StateBlockTrackingReliable.load(std::memory_order_acquire) &&
                (!force || effectForce))
            {
                out = R22ShadowState;
                return true;
            }

            // If Apply tracking is unavailable, retain the conservative
            // historical live-validation window.
            if (!force && R23LastStateSampleEpoch == PresentEpoch &&
                R22ShadowState.Valid() && !R22ShadowState.enabled &&
                R23GameDrawSerial - R23LastStateSampleDrawSerial < 16)
            {
                out = R22ShadowState;
                return true;
            }

            R22ScissorSnapshot actual{};
            if (!R22CaptureGameScissor(device, actual))
            {
                ++R23StateCaptureFailures;
                return false;
            }

            if (R22ShadowState.Valid() && !R23SnapshotSame(actual, R22ShadowState))
            {
                const bool implicitRenderTargetViewport =
                    site && std::strcmp(site, "SetRenderTarget") == 0;
                if (implicitRenderTargetViewport)
                {
                    if (!R23FirstImplicitViewportResyncLogged)
                    {
                        R23FirstImplicitViewportResyncLogged = true;
                        spdlog::info(
                            "VR R23 STATE: SetRenderTarget implicit viewport/scissor transition observed and shadow state resynchronized immediately");
                    }
                }
                else
                {
                    ++R23StateMismatches;
                    const ULONGLONG now = GetTickCount64();
                    if (R23LastStateMismatchLogMs == 0 ||
                        now - R23LastStateMismatchLogMs >= 5000)
                    {
                        R23LastStateMismatchLogMs = now;
                        spdlog::warn(
                            "VR R23/R25 STATE: actual/cache viewport-scissor mismatch site={} actualVP={},{},{}x{} actualScissor={} [{},{},{},{}] cacheVP={},{},{}x{} cacheScissor={} [{},{},{},{}]",
                            site,
                            actual.viewport.X, actual.viewport.Y,
                            actual.viewport.Width, actual.viewport.Height,
                            actual.enabled ? 1 : 0,
                            actual.rect.left, actual.rect.top,
                            actual.rect.right, actual.rect.bottom,
                            R22ShadowState.viewport.X, R22ShadowState.viewport.Y,
                            R22ShadowState.viewport.Width,
                            R22ShadowState.viewport.Height,
                            R22ShadowState.enabled ? 1 : 0,
                            R22ShadowState.rect.left, R22ShadowState.rect.top,
                            R22ShadowState.rect.right, R22ShadowState.rect.bottom);
                    }
                }
            }

            // LIVE device state is authoritative. This also repairs changes made
            // by SetRenderTarget and StateBlock::Apply that bypass tracked setters.
            R22ShadowState = actual;
            R23LastStateSampleDrawSerial = R23GameDrawSerial;
            R23LastStateSampleEpoch = PresentEpoch;
            out = actual;
            return true;
        }

        void R23Reject(const char* reason) noexcept
        {
            ++R23BaselineRejects;
            R23LastRejectReason = reason;
        }

        void R23ObservePassiveDepthClear(IDirect3DDevice9* device,
            const R22ScissorSnapshot& state, DWORD count, const D3DRECT* rects,
            DWORD flags, float z, DWORD stencil, HRESULT hr) noexcept
        {
            if (FAILED(hr) || !TargetIsBackBuffer() ||
                !R22GameClearCoversBackbuffer(count, rects, state) ||
                R9DeferredDepth || !R9CurrentDepthCanMirror())
                return;

            if ((flags & D3DCLEAR_ZBUFFER) != 0)
            {
                R23DepthClearEpoch = PresentEpoch;
                R23DepthClearGameDrawSerial = R23GameDrawSerial;
                R23DepthClearGeneration = R9MainDepthGeneration;
                R23DepthClearZ = z;
            }
            if ((flags & D3DCLEAR_STENCIL) != 0)
            {
                R23StencilClearEpoch = PresentEpoch;
                R23StencilClearGameDrawSerial = R23GameDrawSerial;
                R23StencilClearGeneration = R9MainDepthGeneration;
                R23StencilClearValue = stencil;
            }
        }

        bool R23DepthBaselineSafe(IDirect3DDevice9* device,
            bool& stencilSafe) noexcept
        {
            stencilSafe = true;
            if (!TrackedDepthStencil)
                return true;
            if (R9DeferredDepth || !R9CurrentDepthCanMirror())
                return false;

            const bool depthSafe =
                R23DepthClearEpoch == PresentEpoch &&
                R23DepthClearGameDrawSerial == R23GameDrawSerial &&
                R23DepthClearGeneration == R9MainDepthGeneration;
            if (!depthSafe)
                return false;

            if (SurfaceHasStencil(TrackedDepthStencil))
            {
                stencilSafe =
                    R23StencilClearEpoch == PresentEpoch &&
                    R23StencilClearGameDrawSerial == R23GameDrawSerial &&
                    R23StencilClearGeneration == R9MainDepthGeneration;
                if (StencilTestActive(device) && !stencilSafe)
                    return false;
            }
            return true;
        }

        bool R23HaveFreshCurrentFramePose() noexcept
        {
            if (OutRunVRRenderer::GetBeginSceneCallCount() <=
                R23LastPresentBeginScene)
                return false;
            OutRunVRRenderer::LatchedStereoFrame pose{};
            return OutRunVRRenderer::GetLatchedStereoFrame(pose) &&
                pose.valid && pose.poseSequence != 0;
        }

        bool R23RestoreActualGameState(IDirect3DDevice9* device,
            IDirect3DSurface9* savedRt, IDirect3DSurface9* savedDepth,
            const R22ScissorSnapshot& state) noexcept
        {
            bool ok = true;
            if (FAILED(SetRenderTargetHook.stdcall<HRESULT>(device, 0u, savedRt)))
                ok = false;
            if (FAILED(SetDepthStencilSurfaceHook.stdcall<HRESULT>(device, savedDepth)))
                ok = false;
            if (FAILED(device->SetViewport(&state.viewport)))
                ok = false;
            if (FAILED(device->SetScissorRect(&state.rect)))
                ok = false;
            if (FAILED(device->SetRenderState(D3DRS_SCISSORTESTENABLE,
                    state.enabled)))
                ok = false;
            return ok;
        }

        bool R23ClearPrivateDepth(IDirect3DDevice9* device,
            IDirect3DSurface9* rt, IDirect3DSurface9* depth,
            const R22ScissorSnapshot& state, bool stencilSafe) noexcept
        {
            if (!depth)
                return true;
            if (FAILED(SetRenderTargetHook.stdcall<HRESULT>(device, 0u, rt)) ||
                FAILED(SetDepthStencilSurfaceHook.stdcall<HRESULT>(device, depth)) ||
                FAILED(device->SetViewport(&state.viewport)) ||
                FAILED(device->SetScissorRect(&state.rect)) ||
                FAILED(device->SetRenderState(D3DRS_SCISSORTESTENABLE,
                    state.enabled)))
                return false;

            HRESULT hr = ClearHook.stdcall<HRESULT>(device, 0u, nullptr,
                D3DCLEAR_ZBUFFER, 0, R23DepthClearZ, 0);
            ++R23RecoveryVrSurfaceWrites;
            if (FAILED(hr))
                return false;

            if (SurfaceHasStencil(depth) && stencilSafe)
            {
                hr = ClearHook.stdcall<HRESULT>(device, 0u, nullptr,
                    D3DCLEAR_STENCIL, 0, 1.0f, R23StencilClearValue);
                ++R23RecoveryVrSurfaceWrites;
                if (FAILED(hr))
                    return false;
            }
            return true;
        }

        bool R23InitializeStereoFromAuthoritativeBaseline(
            IDirect3DDevice9* device, const R22ScissorSnapshot& state,
            bool stencilSafe, DWORD colorRectCount,
            const D3DRECT* colorRects, D3DCOLOR color) noexcept
        {
            if (!device || AnyAuxRenderTargetActive() ||
                !EnsureStereoResources(device) || !R9EnsureMonoResources(device) ||
                !BackBuffer || !RightEyeSurface || !R9MonoSurface)
                return false;

            IDirect3DSurface9* savedRt = nullptr;
            IDirect3DSurface9* savedDepth = nullptr;
            const HRESULT rtHr = device->GetRenderTarget(0, &savedRt);
            const HRESULT depthHr = device->GetDepthStencilSurface(&savedDepth);
            const bool depthStateOk = SUCCEEDED(depthHr) || depthHr == D3DERR_NOTFOUND;
            if (FAILED(rtHr) || !savedRt || !depthStateOk)
            {
                if (savedRt) savedRt->Release();
                if (savedDepth) savedDepth->Release();
                return false;
            }

            bool ok = true;
            {
                InternalPassScope guard;

                auto replayColorClear = [&](IDirect3DSurface9* target) noexcept {
                    if (!target ||
                        FAILED(SetRenderTargetHook.stdcall<HRESULT>(
                            device, 0u, target)) ||
                        FAILED(SetDepthStencilSurfaceHook.stdcall<HRESULT>(
                            device, static_cast<IDirect3DSurface9*>(nullptr))) ||
                        FAILED(device->SetViewport(&state.viewport)) ||
                        FAILED(device->SetScissorRect(&state.rect)) ||
                        FAILED(device->SetRenderState(D3DRS_SCISSORTESTENABLE,
                            state.enabled)))
                        return false;

                    const HRESULT colorHr = ClearHook.stdcall<HRESULT>(
                        device, colorRectCount, colorRects,
                        D3DCLEAR_TARGET, color, 1.0f, 0u);
                    ++R23RecoveryVrSurfaceWrites;
                    return SUCCEEDED(colorHr);
                };

                // The authoritative candidate is already proven to cover the
                // complete game backbuffer. Replaying that exact clear into
                // the private eye/mono targets is equivalent to copying the
                // just-cleared backbuffer, but avoids two 3440x1440 full-surface
                // StretchRect copies per Present and cannot carry stale eye
                // color history into translucent right-eye passes.
                if (!replayColorClear(RightEyeSurface))
                    ok = false;
                if (ok && !replayColorClear(R9MonoSurface))
                    ok = false;

                if (ok && !R23FirstColorBaselineLogged)
                {
                    R23FirstColorBaselineLogged = true;
                    spdlog::info(
                        "VR C1.1 BASELINE: authoritative color clear replay seeds right/mono surfaces; two full-surface StretchRect baseline copies removed");
                }

                if (ok && TrackedDepthStencil)
                {
                    ok = R23ClearPrivateDepth(device, RightEyeSurface,
                        RightEyeDepth, state, stencilSafe);
                    if (ok)
                        ok = R23ClearPrivateDepth(device, R9MonoSurface,
                            R9MonoDepth, state, stencilSafe);
                }

                if (!R23RestoreActualGameState(device, savedRt, savedDepth, state))
                    ok = false;
            }

            savedRt->Release();
            if (savedDepth) savedDepth->Release();
            if (!ok)
                return false;

            R9StereoSeeded = true;
            R9MonoSeeded = true;
            R9MonoBackupGap = false;

            if (TrackedDepthStencil)
            {
                std::uint64_t serial = std::max(
                    R9MainDepthContentSerial, R9MonoDepthContentSerial);
                if (++serial == 0)
                    serial = 1;
                R9MainDepthContentSerial = serial;
                R9MonoDepthContentSerial = serial;
                RightDepthSynchronized = true;
                RightStencilSynchronized =
                    !SurfaceHasStencil(TrackedDepthStencil) || stencilSafe;
            }
            else
            {
                RightDepthSynchronized = true;
                RightStencilSynchronized = true;
            }

            OutRunVR::RuntimeEligibility::BaselineVerified();
            const bool allowed = OutRunVR::RuntimeEligibility::MayInjectStereo();
            R20StereoEligibilityGate.store(allowed, std::memory_order_release);
            if (!allowed)
            {
                R9StereoSeeded = false;
                R9MonoSeeded = false;
                return false;
            }

            ++R23AuthoritativeSeeds;
            if (!R23FirstSeedLogged)
            {
                R23FirstSeedLogged = true;
                spdlog::info(
                    "VR R23/R25 BASELINE: authoritative first seed opened only after live viewport/scissor + full game draw serial + current-generation depth + fresh current-frame pose; R20/R22 double approval removed");
            }
            return true;
        }

        bool R23RecoveryNeedsBaseline() noexcept
        {
            return OutRunVR::RuntimeEligibility::SafetyOverlayReady.load(
                       std::memory_order_acquire) &&
                OutRunVR::RuntimeEligibility::HostFresh.load(
                       std::memory_order_acquire) &&
                OutRunVR::RuntimeEligibility::RecoveryPending.load(
                    std::memory_order_acquire) &&
                !OutRunVR::RuntimeEligibility::StereoAllowed.load(
                    std::memory_order_acquire);
        }

        HRESULT __stdcall ClearDestR23(IDirect3DDevice9* device, DWORD count,
            const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
            DWORD stencil)
        {
            if (!IsGameDevice(device) || InternalStereoPass)
                return R23ClearR22Hook.stdcall<HRESULT>(
                    device, count, rects, flags, color, z, stencil);

            R22ScissorSnapshot actual{};
            const bool stateOk = R23CaptureActualGameState(
                device, actual, "Clear", true);

            // Once the authoritative seed is already open, keep the validated
            // R22/R20/R9 replay path for subsequent clears in the same Present.
            if (R9StereoSeeded &&
                OutRunVR::RuntimeEligibility::MayInjectStereo())
            {
                return R23ClearR22Hook.stdcall<HRESULT>(
                    device, count, rects, flags, color, z, stencil);
            }

            // Before first seed (including recovery), execute the GAME clear
            // exactly once. Do not enter R20/R9 mono/right replay here.
            const bool mainBefore = TargetIsBackBuffer();
            const bool mrtActive = AnyAuxRenderTargetActive();
            const bool wantsStereo = StereoWanted();
            const HRESULT hr = ClearHook.stdcall<HRESULT>(
                device, count, rects, flags, color, z, stencil);
            ++R23PassiveClears;

            if (!R23FirstPassiveLogged)
            {
                R23FirstPassiveLogged = true;
                spdlog::info(
                    "VR R23/R25 RECOVERY: passive original Clear only; VR surface writes=0 until next-frame authoritative initialization");
            }

            if (FAILED(hr) || !stateOk || !mainBefore || !wantsStereo)
                return hr;

            const bool fullGameClear = R22GameClearCoversBackbuffer(
                count, rects, actual);
            R23ObservePassiveDepthClear(device, actual, count, rects,
                flags, z, stencil, hr);

            if ((flags & D3DCLEAR_TARGET) == 0)
                return hr;
            if (mrtActive)
            {
                R23Reject("mrt-active");
                return hr;
            }
            if (!fullGameClear)
            {
                R23Reject("partial-viewport-scissor-or-rect");
                return hr;
            }

            bool stencilSafe = true;
            if (!R23DepthBaselineSafe(device, stencilSafe))
            {
                R23Reject("depth-stencil-or-game-draw-serial");
                return hr;
            }

            const bool recovering = R23RecoveryNeedsBaseline();
            if (recovering &&
                !OutRunVR::RuntimeEligibility::PoseWarmupAllowed())
            {
                // This frame stays completely monoscopic. Arm only the next
                // BeginScene so it can retain a fresh pose without injecting it.
                R23ArmWarmupAtPresent = true;
                if (!R23FirstWarmupArmedLogged)
                {
                    R23FirstWarmupArmedLogged = true;
                    spdlog::info(
                        "VR R23/R25 BASELINE: passive candidate recorded epoch={} gameDrawSerial={} depthGeneration={}; next Present will arm pose warmup, not stereo",
                        PresentEpoch, R23GameDrawSerial, R9MainDepthGeneration);
                }
                return hr;
            }

            const bool maySeedNow =
                (OutRunVR::RuntimeEligibility::PoseWarmupAllowed() ||
                 OutRunVR::RuntimeEligibility::StereoAllowed.load(
                    std::memory_order_acquire)) &&
                OutRunVR::RuntimeEligibility::HostFresh.load(
                    std::memory_order_acquire);
            if (!maySeedNow)
            {
                R23Reject("eligibility-not-ready");
                return hr;
            }
            if (!R23HaveFreshCurrentFramePose())
            {
                R23Reject("fresh-current-frame-pose-missing");
                return hr;
            }

            if (!R23InitializeStereoFromAuthoritativeBaseline(
                    device, actual, stencilSafe, count, rects, color))
                R23Reject("eye-mono-initialization-failed");
            return hr;
        }

        void R23BeforeTopLevelDraw(IDirect3DDevice9* device) noexcept
        {
            if (!IsGameDevice(device) || InternalStereoPass)
                return;
            ++R23GameDrawSerial;
            R22ScissorSnapshot ignored{};
            R23CaptureActualGameState(device, ignored, "Draw", false);
        }

        HRESULT __stdcall DrawPrimitiveDestR23(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
        {
            R23BeforeTopLevelDraw(device);
            return R23DrawPrimitiveR22Hook.stdcall<HRESULT>(
                device, type, startVertex, primitiveCount);
        }

        HRESULT __stdcall DrawIndexedPrimitiveDestR23(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, INT baseVertexIndex, UINT minVertexIndex,
            UINT numVertices, UINT startIndex, UINT primitiveCount)
        {
            R23BeforeTopLevelDraw(device);
            return R23DrawIndexedPrimitiveR22Hook.stdcall<HRESULT>(device, type,
                baseVertexIndex, minVertexIndex, numVertices, startIndex,
                primitiveCount);
        }

        HRESULT __stdcall DrawPrimitiveUPDestR23(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data,
            UINT stride)
        {
            R23BeforeTopLevelDraw(device);
            return R23DrawPrimitiveUPR22Hook.stdcall<HRESULT>(
                device, type, primitiveCount, data, stride);
        }

        HRESULT __stdcall DrawIndexedPrimitiveUPDestR23(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT minVertexIndex, UINT numVertices,
            UINT primitiveCount, const void* indexData, D3DFORMAT indexFormat,
            const void* vertexData, UINT stride)
        {
            R23BeforeTopLevelDraw(device);
            return R23DrawIndexedPrimitiveUPR22Hook.stdcall<HRESULT>(device, type,
                minVertexIndex, numVertices, primitiveCount, indexData,
                indexFormat, vertexData, stride);
        }

        HRESULT __stdcall SetRenderTargetDestR23(IDirect3DDevice9* device,
            DWORD index, IDirect3DSurface9* surface)
        {
            const HRESULT hr = R23SetRenderTargetR9Hook.stdcall<HRESULT>(
                device, index, surface);
            if (SUCCEEDED(hr) && index == 0 && IsGameDevice(device) &&
                !InternalStereoPass)
            {
                R22ScissorSnapshot actual{};
                if (!R23CaptureActualGameState(device, actual,
                        "SetRenderTarget", true))
                {
                    R22ShadowState = {};
                    R23LastStateSampleDrawSerial = 0;
                    R23LastStateSampleEpoch = 0;
                    R22FailClosedReplayState(
                        device, "R23/SetRenderTarget/live-state-capture");
                }
            }
            return hr;
        }

        const char* R23DiagnoseHostFreshness(std::int64_t& ageMs) noexcept
        {
            ageMs = -1;
            if (!SharedState || SharedState->magic != OutRunVR::SharedMagic ||
                SharedState->protocolVersion != OutRunVR::SharedProtocolVersion ||
                SharedState->structSize != sizeof(OutRunVR::SharedPoseState))
                return "mapping-or-header";

            OutRunVR::SharedPoseState snap{};
            bool stable = false;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                const std::uint32_t before = SharedState->sequence;
                if (before & 1u) continue;
                MemoryBarrier();
                std::memcpy(&snap, SharedState, sizeof(snap));
                MemoryBarrier();
                const std::uint32_t after = SharedState->sequence;
                if (before == after && !(after & 1u))
                {
                    stable = true;
                    break;
                }
            }
            if (!stable) return "seqlock-busy";
            if (!snap.hostPid) return "host-pid-missing";
            if ((snap.flags & OutRunVR::HostAlive) == 0) return "host-alive-off";
            if ((snap.flags & OutRunVR::SessionVisible) == 0) return "session-visible-off";
            if ((snap.flags & OutRunVR::HostShouldRender) == 0) return "should-render-off";
            if (snap.sampleQpc <= 0) return "qpc-invalid";

            LARGE_INTEGER now{}, frequency{};
            if (!QueryPerformanceCounter(&now) ||
                !QueryPerformanceFrequency(&frequency) ||
                frequency.QuadPart <= 0 || now.QuadPart < snap.sampleQpc)
                return "qpc-invalid";
            ageMs = ((now.QuadPart - snap.sampleQpc) * 1000) /
                frequency.QuadPart;
            if (ageMs > OutRunVR::RuntimeEligibility::HostStaleMs)
                return "sample-stale";
            return "fresh";
        }

        void R23MaybeLogSummary() noexcept
        {
            if (!Settings::VRTelemetry)
                return;
            const ULONGLONG now = GetTickCount64();
            if (now - R23LastSummaryMs < 5000)
                return;
            R23LastSummaryMs = now;
            spdlog::info(
                "VR R23/R25 summary: epoch={} gameDrawSerial={} passiveClear={} authoritativeSeed={} recoveryVrWrites={} stateMismatch={} stateCaptureFail={} baselineReject={} lastReject={} warmup={} stereoAllowed={} recoveryPending={} hostReason={}",
                PresentEpoch, R23GameDrawSerial, R23PassiveClears,
                R23AuthoritativeSeeds, R23RecoveryVrSurfaceWrites,
                R23StateMismatches, R23StateCaptureFailures,
                R23BaselineRejects, R23LastRejectReason,
                OutRunVR::RuntimeEligibility::PoseWarmupAllowed() ? 1 : 0,
                OutRunVR::RuntimeEligibility::StereoAllowed.load(
                    std::memory_order_acquire) ? 1 : 0,
                OutRunVR::RuntimeEligibility::RecoveryPending.load(
                    std::memory_order_acquire) ? 1 : 0,
                R23LastHostReason);
        }

        HRESULT __stdcall PresentDestR23(IDirect3DDevice9* device,
            const RECT* sourceRect, const RECT* destRect,
            HWND destWindowOverride, const RGNDATA* dirtyRegion)
        {
            const bool gameDevice = IsGameDevice(device) && !InternalStereoPass;
            const HRESULT hr = R23PresentR21Hook.stdcall<HRESULT>(device,
                sourceRect, destRect, destWindowOverride, dirtyRegion);
            if (!gameDevice)
                return hr;

            std::int64_t ageMs = -1;
            const char* reason = R23DiagnoseHostFreshness(ageMs);
            if (std::strcmp(reason, R23LastHostReason) != 0)
            {
                R23LastHostReason = reason;
                spdlog::info(
                    "VR R23/R25 HOST: freshnessReason={} ageMs={} runtimeHostFresh={} renderable={} (hard freshness and transient shouldRender are separate gates)",
                    reason, ageMs,
                    OutRunVR::RuntimeEligibility::HostFresh.load(
                        std::memory_order_acquire) ? 1 : 0,
                    OutRunVR::RuntimeEligibility::HostRenderable.load(
                        std::memory_order_acquire) ? 1 : 0);
            }

            if (R23ArmWarmupAtPresent)
            {
                if (SUCCEEDED(hr) &&
                    OutRunVR::RuntimeEligibility::HostFresh.load(
                        std::memory_order_acquire) &&
                    OutRunVR::RuntimeEligibility::RecoveryPending.load(
                        std::memory_order_acquire))
                {
                    OutRunVR::RuntimeEligibility::ArmRecoveryPoseWarmup();
                    spdlog::info(
                        "VR R23/R25 RESUME: verified passive baseline armed pose warmup after Present; stereo remains closed until a new next-frame authoritative clear");
                }
                R23ArmWarmupAtPresent = false;
            }

            R23LastPresentBeginScene = OutRunVRRenderer::GetBeginSceneCallCount();
            if (!OutRunVR::RuntimeEligibility::HostFresh.load(
                    std::memory_order_acquire))
                R20StereoEligibilityGate.store(false, std::memory_order_release);
            R23MaybeLogSummary();
            return hr;
        }

        void R23RollbackHooks() noexcept
        {
            R23PresentR21Hook = {};
            R23SetRenderTargetR9Hook = {};
            R23DrawIndexedPrimitiveUPR22Hook = {};
            R23DrawPrimitiveUPR22Hook = {};
            R23DrawIndexedPrimitiveR22Hook = {};
            R23DrawPrimitiveR22Hook = {};
            R23ClearR22Hook = {};
        }

        bool R23EnableHooks() noexcept
        {
            SafetyHookInline* hooks[]{
                &R23ClearR22Hook,
                &R23DrawPrimitiveR22Hook,
                &R23DrawIndexedPrimitiveR22Hook,
                &R23DrawPrimitiveUPR22Hook,
                &R23DrawIndexedPrimitiveUPR22Hook,
                &R23SetRenderTargetR9Hook,
                &R23PresentR21Hook
            };
            for (auto* hook : hooks)
            {
                if (!*hook || !hook->enable().has_value())
                    return false;
            }
            return true;
        }

        DWORD WINAPI R23RecoveryInstallThread(void*)
        {
            using State = OutRunVR::RuntimeEligibility::InstallState;
            R23InstallState.store(State::Pending, std::memory_order_release);
            for (int attempt = 0; attempt < 4800; ++attempt)
            {
                if (R9InstallState.load(std::memory_order_acquire) == R9InstallFailed ||
                    R13InstallState.load(std::memory_order_acquire) == R13InstallFailed ||
                    OutRunVR::RuntimeEligibility::IsFailed(R20InstallState) ||
                    OutRunVR::RuntimeEligibility::IsFailed(R21InstallState) ||
                    OutRunVR::RuntimeEligibility::IsFailed(R22InstallState))
                {
                    R23InstallState.store(State::Failed, std::memory_order_release);
                    OutRunVR::RuntimeEligibility::MarkSafetyOverlayUnavailable();
                    R20StereoEligibilityGate.store(false, std::memory_order_release);
                    HookManager::ReportAsyncResult(
                        "OpenXRVRRecoveryBaselineR23", false);
                    spdlog::error(
                        "VR R23/R25: prerequisite overlay FAILED; authoritative recovery remains fail-closed");
                    return 0;
                }

                if (OutRunVR::RuntimeEligibility::IsReady(R22InstallState))
                {
                    const auto disabled = safetyhook::InlineHook::StartDisabled;
                    R23ClearR22Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&ClearDestR22), ClearDestR23,
                        disabled);
                    R23DrawPrimitiveR22Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveDestR22),
                        DrawPrimitiveDestR23, disabled);
                    R23DrawIndexedPrimitiveR22Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveDestR22),
                        DrawIndexedPrimitiveDestR23, disabled);
                    R23DrawPrimitiveUPR22Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveUPDestR22),
                        DrawPrimitiveUPDestR23, disabled);
                    R23DrawIndexedPrimitiveUPR22Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveUPDestR22),
                        DrawIndexedPrimitiveUPDestR23, disabled);
                    R23SetRenderTargetR9Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&SetRenderTargetDestR9),
                        SetRenderTargetDestR23, disabled);
                    R23PresentR21Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&PresentDestR21), PresentDestR23,
                        disabled);

                    if (!R23EnableHooks())
                    {
                        R23RollbackHooks();
                        R23InstallState.store(State::Failed, std::memory_order_release);
                        OutRunVR::RuntimeEligibility::MarkSafetyOverlayUnavailable();
                        R20StereoEligibilityGate.store(false, std::memory_order_release);
                        HookManager::ReportAsyncResult(
                            "OpenXRVRRecoveryBaselineR23", false);
                        spdlog::error(
                            "VR R23/R25: disabled-first recovery/state/draw/present transaction failed; stereo remains fail-closed");
                        return 0;
                    }

                    R22ScissorSnapshot initial{};
                    if (IDirect3DDevice9* device =
                            StereoInstalledDevice.load(std::memory_order_acquire))
                        R23CaptureActualGameState(device, initial, "install", true);

                    OutRunVR::RuntimeEligibility::MarkSafetyOverlayInstalled();
                    R20StereoEligibilityGate.store(false, std::memory_order_release);
                    R23LastPresentBeginScene =
                        OutRunVRRenderer::GetBeginSceneCallCount();
                    R23InstallState.store(State::Ready, std::memory_order_release);
                    HookManager::ReportAsyncResult(
                        "OpenXRVRRecoveryBaselineR23", true);
                    spdlog::info(
                        "VR R23 GAME: single-owner live-state recovery baseline ACTIVE; passive Clear=original-once, top-level gameDrawSerial includes gate-closed draws, first seed waits for next-frame fresh pose");
                    return 0;
                }
                Sleep(25);
            }

            R23InstallState.store(State::Failed, std::memory_order_release);
            OutRunVR::RuntimeEligibility::MarkSafetyOverlayUnavailable();
            R20StereoEligibilityGate.store(false, std::memory_order_release);
            HookManager::ReportAsyncResult(
                "OpenXRVRRecoveryBaselineR23", false);
            spdlog::error(
                "VR R23/R25: timed out waiting for R22 safety overlay; stereo kept fail-closed");
            return 0;
        }

        class VRRecoveryBaselineR23Hook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRRecoveryBaselineR23";
            }
            bool validate() override { return true; }
            bool apply() override
            {
                using State = OutRunVR::RuntimeEligibility::InstallState;
                R23InstallState.store(State::Pending, std::memory_order_release);
                OutRunVR::RuntimeEligibility::MarkSafetyOverlayUnavailable();
                R20StereoEligibilityGate.store(false, std::memory_order_release);

                HANDLE thread = CreateThread(nullptr, 0,
                    R23RecoveryInstallThread, nullptr, 0, nullptr);
                if (!thread)
                {
                    R23InstallState.store(State::Failed, std::memory_order_release);
                    OutRunVR::RuntimeEligibility::MarkSafetyOverlayUnavailable();
                    R20StereoEligibilityGate.store(false, std::memory_order_release);
                    return false;
                }
                CloseHandle(thread);
                return true;
            }
            static VRRecoveryBaselineR23Hook instance;
        };

        VRRecoveryBaselineR23Hook VRRecoveryBaselineR23Hook::instance;
    }

    std::uint64_t GetTopLevelDrawSerial() noexcept
    {
        return R23GameDrawSerial;
    }
}
