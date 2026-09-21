// R34 final ResetEx replay health guard.
//
// R33 remains the draw/Reset implementation owner. R34 only adds the final
// compatibility boundary required by the R15 Ex overlay: a successful ResetEx
// is not enough to resume stereo if classic D3D9 state replay was incomplete.
// While that condition is active, every Present keeps the shared eligibility
// state fail-closed so a later baseline cannot accidentally re-enable stereo on
// stale ResetEx state. A later clean Reset clears the block.

#include "stereo_renderer_r33.cpp"

namespace OutRunVRD3D9ExUpgradeR13
{
    bool LastResetStateReplaySucceeded() noexcept;
}

namespace OutRunVRStereo
{
    namespace
    {
        SafetyHookInline R34ResetR33Hook{};
        SafetyHookInline R34PresentR33Hook{};
        SafetyHookInline R34DrawPrimitiveR33Hook{};
        SafetyHookInline R34DrawIndexedPrimitiveR33Hook{};
        SafetyHookInline R34DrawPrimitiveUPR33Hook{};
        SafetyHookInline R34DrawIndexedPrimitiveUPR33Hook{};
        std::atomic<OutRunVR::RuntimeEligibility::InstallState> R34InstallState{
            OutRunVR::RuntimeEligibility::InstallState::Pending };
        std::atomic<bool> R34ResetReplayBlocked{false};
        std::uint64_t R34ReplayBlocks = 0;
        std::uint64_t R34RasterGuardDraws = 0;
        bool R34FirstReplayBlockLogged = false;
        bool R34FirstRasterGuardLogged = false;

        void R34ForceResetReplayFailClosed(IDirect3DDevice9* device,
            const char* site) noexcept
        {
            if (!device || !IsGameDevice(device))
                return;

            OutRunVR::RuntimeEligibility::SetExternalSafetyBlock(true);
            R22FailClosedEligibility();
            R22ResetBaselineTracking();
            R22ShadowState = {};
            R33InvalidateDepthStencilCache();
            R29ArmMonoSafety();
            RightDepthSynchronized = false;
            RightStencilSynchronized = false;

            if (!R34FirstReplayBlockLogged)
            {
                R34FirstReplayBlockLogged = true;
                spdlog::error(
                    "VR R34 RESET: classic D3D9 state replay is unhealthy at {}; stereo remains fail-closed until a later clean ResetEx replay",
                    site ? site : "unknown");
            }
        }

        template <typename DrawCall>
        HRESULT R34GuardStereoRasterState(IDirect3DDevice9* device,
            DrawCall&& drawCall, const char* site) noexcept
        {
            if (!device || !IsGameDevice(device) || InternalStereoPass ||
                R31StateBlockRecording || !StereoWanted() ||
                !TargetIsBackBuffer())
            {
                return drawCall();
            }

            // SetRenderTarget resets both viewport and scissor rectangle in
            // D3D9. R33's direct fast/fallback dispatch can bypass R22's draw
            // wrapper, so establish the already-validated R22 replay scope at
            // the final draw boundary. Flush a pending StateBlock resync first
            // so the scope never snapshots stale shadow state.
            R31FlushPendingStateBlockResync(device);
            R22ReplayScope replay(device);
            if (!replay.stateValid)
            {
                R22FailClosedReplayState(device, site);
                return drawCall();
            }

            ++R34RasterGuardDraws;
            if (!R34FirstRasterGuardLogged)
            {
                R34FirstRasterGuardLogged = true;
                spdlog::info(
                    "VR R34 RASTER GUARD: final draw boundary now preserves viewport/scissor across R33 fast and R29 fallback eye-target switches");
            }
            return drawCall();
        }

        HRESULT __stdcall DrawPrimitiveDestR34(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
        {
            auto call = [&]() {
                return R34DrawPrimitiveR33Hook.stdcall<HRESULT>(
                    device, type, startVertex, primitiveCount);
            };
            return R34GuardStereoRasterState(
                device, call, "R34/DrawPrimitive/raster-state");
        }

        HRESULT __stdcall DrawIndexedPrimitiveDestR34(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            INT baseVertexIndex, UINT minVertexIndex, UINT numVertices,
            UINT startIndex, UINT primitiveCount)
        {
            auto call = [&]() {
                return R34DrawIndexedPrimitiveR33Hook.stdcall<HRESULT>(
                    device, type, baseVertexIndex, minVertexIndex,
                    numVertices, startIndex, primitiveCount);
            };
            return R34GuardStereoRasterState(
                device, call, "R34/DrawIndexedPrimitive/raster-state");
        }

        HRESULT __stdcall DrawPrimitiveUPDestR34(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data,
            UINT stride)
        {
            auto call = [&]() {
                return R34DrawPrimitiveUPR33Hook.stdcall<HRESULT>(
                    device, type, primitiveCount, data, stride);
            };
            return R34GuardStereoRasterState(
                device, call, "R34/DrawPrimitiveUP/raster-state");
        }

        HRESULT __stdcall DrawIndexedPrimitiveUPDestR34(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            UINT minVertexIndex, UINT numVertices, UINT primitiveCount,
            const void* indexData, D3DFORMAT indexFormat,
            const void* vertexData, UINT stride)
        {
            auto call = [&]() {
                return R34DrawIndexedPrimitiveUPR33Hook.stdcall<HRESULT>(
                    device, type, minVertexIndex, numVertices, primitiveCount,
                    indexData, indexFormat, vertexData, stride);
            };
            return R34GuardStereoRasterState(
                device, call, "R34/DrawIndexedPrimitiveUP/raster-state");
        }

        HRESULT __stdcall ResetDestR34(IDirect3DDevice9* device,
            D3DPRESENT_PARAMETERS* params)
        {
            const bool gameDevice = IsGameDevice(device);
            const HRESULT hr = R34ResetR33Hook.stdcall<HRESULT>(device, params);

            if (!gameDevice)
                return hr;

            if (!OutRunVRD3D9ExUpgradeR13::IsCompatDevice(device))
            {
                R34ResetReplayBlocked.store(false, std::memory_order_release);
                OutRunVR::RuntimeEligibility::SetExternalSafetyBlock(false);
                return hr;
            }

            const bool healthy = SUCCEEDED(hr) &&
                OutRunVRD3D9ExUpgradeR13::LastResetStateReplaySucceeded();
            R34ResetReplayBlocked.store(!healthy, std::memory_order_release);
            OutRunVR::RuntimeEligibility::SetExternalSafetyBlock(!healthy);
            if (!healthy)
            {
                ++R34ReplayBlocks;
                R34ForceResetReplayFailClosed(device, "Reset");
            }
            return hr;
        }

        HRESULT __stdcall PresentDestR34(IDirect3DDevice9* device,
            const RECT* sourceRect, const RECT* destRect,
            HWND destWindowOverride, const RGNDATA* dirtyRegion)
        {
            const bool blocked = IsGameDevice(device) &&
                R34ResetReplayBlocked.load(std::memory_order_acquire);
            if (blocked)
                R34ForceResetReplayFailClosed(device, "Present/pre");

            const HRESULT hr = R34PresentR33Hook.stdcall<HRESULT>(device,
                sourceRect, destRect, destWindowOverride, dirtyRegion);

            if (blocked)
                R34ForceResetReplayFailClosed(device, "Present/post");
            return hr;
        }

        void R34RollbackHooks() noexcept
        {
            R34DrawIndexedPrimitiveUPR33Hook = {};
            R34DrawPrimitiveUPR33Hook = {};
            R34DrawIndexedPrimitiveR33Hook = {};
            R34DrawPrimitiveR33Hook = {};
            R34PresentR33Hook = {};
            R34ResetR33Hook = {};
        }

        bool R34EnableHooks() noexcept
        {
            SafetyHookInline* hooks[]{
                &R34ResetR33Hook,
                &R34PresentR33Hook,
                &R34DrawPrimitiveR33Hook,
                &R34DrawIndexedPrimitiveR33Hook,
                &R34DrawPrimitiveUPR33Hook,
                &R34DrawIndexedPrimitiveUPR33Hook
            };
            for (auto* hook : hooks)
            {
                if (!*hook || !hook->enable().has_value())
                    return false;
            }
            return true;
        }

        DWORD WINAPI R34InstallThread(void*)
        {
            using State = OutRunVR::RuntimeEligibility::InstallState;
            R34InstallState.store(State::Pending, std::memory_order_release);

            for (int attempt = 0; attempt < 4800; ++attempt)
            {
                const auto r33 = R33InstallState.load(std::memory_order_acquire);
                if (r33 == State::Failed)
                {
                    R34InstallState.store(State::Failed,
                        std::memory_order_release);
                    HookManager::ReportAsyncResult(
                        "OpenXRVRStereoR34ResetGuard", false);
                    return 0;
                }

                if (r33 == State::Ready)
                {
                    const auto disabled = safetyhook::InlineHook::StartDisabled;
                    R34ResetR33Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&ResetDestR33),
                        ResetDestR34, disabled);
                    R34PresentR33Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&PresentDestR33),
                        PresentDestR34, disabled);
                    R34DrawPrimitiveR33Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveDestR33),
                        DrawPrimitiveDestR34, disabled);
                    R34DrawIndexedPrimitiveR33Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveDestR33),
                        DrawIndexedPrimitiveDestR34, disabled);
                    R34DrawPrimitiveUPR33Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveUPDestR33),
                        DrawPrimitiveUPDestR34, disabled);
                    R34DrawIndexedPrimitiveUPR33Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveUPDestR33),
                        DrawIndexedPrimitiveUPDestR34, disabled);

                    if (!R34EnableHooks())
                    {
                        R34RollbackHooks();
                        R34InstallState.store(State::Failed,
                            std::memory_order_release);
                        HookManager::ReportAsyncResult(
                            "OpenXRVRStereoR34ResetGuard", false);
                        spdlog::error(
                            "VR R34: Reset/Present/draw raster guard transaction failed; R33 remains authoritative");
                        return 0;
                    }

                    IDirect3DDevice9* const installedDevice =
                        StereoInstalledDevice.load(std::memory_order_acquire);
                    if (installedDevice &&
                        OutRunVRD3D9ExUpgradeR13::IsCompatDevice(installedDevice))
                    {
                        const bool healthy =
                            OutRunVRD3D9ExUpgradeR13::LastResetStateReplaySucceeded();
                        R34ResetReplayBlocked.store(!healthy,
                            std::memory_order_release);
                        OutRunVR::RuntimeEligibility::SetExternalSafetyBlock(
                            !healthy);
                        if (!healthy)
                            R34ForceResetReplayFailClosed(
                                installedDevice, "Install/state-sync");
                    }

                    R34InstallState.store(State::Ready,
                        std::memory_order_release);
                    HookManager::ReportAsyncResult(
                        "OpenXRVRStereoR34ResetGuard", true);
                    spdlog::info(
                        "VR R34 RESET GUARD: R15 classic-state replay health now gates post-Reset stereo eligibility; raster guard preserves viewport/scissor across final stereo draw dispatch");
                    return 0;
                }
                Sleep(25);
            }

            R34InstallState.store(State::Failed, std::memory_order_release);
            HookManager::ReportAsyncResult(
                "OpenXRVRStereoR34ResetGuard", false);
            return 0;
        }

        class VRStereoR34ResetGuardHook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRStereoR34ResetGuard";
            }
            bool validate() override { return true; }
            bool apply() override
            {
                HANDLE thread = CreateThread(
                    nullptr, 0, R34InstallThread, nullptr, 0, nullptr);
                if (!thread)
                {
                    R34InstallState.store(
                        OutRunVR::RuntimeEligibility::InstallState::Failed,
                        std::memory_order_release);
                    return false;
                }
                CloseHandle(thread);
                return true;
            }

            static VRStereoR34ResetGuardHook instance;
        };

        VRStereoR34ResetGuardHook VRStereoR34ResetGuardHook::instance;
    }
}
