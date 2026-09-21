// R20 production stabilization wrapper.
//
// Keep the validated R13/R9 safety stack intact, but fix the bootstrap rule
// that could leave the RenderFrame ring permanently at frameId=0 on OutRun.
// R9 required a *full-screen* main-target color clear on every Present before
// any stereo draw could start. Real OutRun render paths can use a rect/scissored
// main color clear.
//
// R23 review hardening makes the depth/stencil baseline rule common to every
// first stereo seed. A legacy R9 full-color clear and the relaxed R20 bootstrap
// must both prove current-generation depth/stencil synchronization before eye
// transforms become eligible.

#include "stereo_renderer_r13.cpp"
#include "../runtime_eligibility.hpp"

namespace OutRunVRStereo
{
    namespace
    {
        SafetyHookInline R20ClearR9Hook{};
        std::atomic<OutRunVR::RuntimeEligibility::InstallState> R20InstallState{
            OutRunVR::RuntimeEligibility::InstallState::Pending };
        // CI/diagnostic invariant: terminal installer failure is InstallState::Failed;
        // local worker code uses the State alias below without changing semantics.

        // Compatibility gate consumed by R21/R23 overlays. RuntimeEligibility is
        // authoritative; this mirror remains so older R20 call sites fail closed.
        std::atomic<bool> R20StereoEligibilityGate{false};

        bool R20FirstRelaxedSeedLogged = false;
        bool R20FirstLegacySeedValidatedLogged = false;
        std::uint64_t R20RelaxedSeeds = 0;
        std::uint64_t R20LegacySeedsRejected = 0;
        std::uint64_t R20BaselineCopyFailures = 0;
        std::uint64_t R20DepthGateRejects = 0;

        // Track Z and stencil independently. Never OR clear flags from different
        // moments into one synthetic baseline: each required buffer must belong
        // to this Present, this depth generation, and have no draw after clear.
        std::uint64_t R20DepthClearEpoch = 0;
        std::uint64_t R20DepthClearDrawSerial = 0;
        std::uint64_t R20DepthClearGeneration = 0;
        std::uint64_t R20StencilClearEpoch = 0;
        std::uint64_t R20StencilClearDrawSerial = 0;
        std::uint64_t R20StencilClearGeneration = 0;

        void R20ObserveFullDepthSeed(IDirect3DDevice9* device, DWORD count,
            const D3DRECT* rects, DWORD flags, HRESULT hr) noexcept
        {
            if (FAILED(hr) || !device || !IsGameDevice(device) || InternalStereoPass ||
                !TargetIsBackBuffer() || R9DeferredDepth || !R9CurrentDepthCanMirror() ||
                !R9MonoDepth || R9MonoBackupGap)
                return;

            const DWORD depthFlags = flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
            if (!depthFlags || !ClearCoversStereoBackbuffer(device, count, rects))
                return;

            if ((depthFlags & D3DCLEAR_ZBUFFER) != 0)
            {
                R20DepthClearEpoch = PresentEpoch;
                R20DepthClearDrawSerial = R9DrawCalls;
                R20DepthClearGeneration = R9MainDepthGeneration;
            }
            if ((depthFlags & D3DCLEAR_STENCIL) != 0)
            {
                R20StencilClearEpoch = PresentEpoch;
                R20StencilClearDrawSerial = R9DrawCalls;
                R20StencilClearGeneration = R9MainDepthGeneration;
            }
        }

        bool R20DepthHistorySafeForInitialSeed(IDirect3DDevice9* device) noexcept
        {
            if (!TrackedDepthStencil)
                return true;

            const bool depthSafe =
                R20DepthClearEpoch == PresentEpoch &&
                R20DepthClearDrawSerial == R9DrawCalls &&
                R20DepthClearGeneration == R9MainDepthGeneration &&
                R9MonoDepth != nullptr;

            bool stencilSafe = true;
            if (StencilTestActive(device))
            {
                stencilSafe =
                    R20StencilClearEpoch == PresentEpoch &&
                    R20StencilClearDrawSerial == R9DrawCalls &&
                    R20StencilClearGeneration == R9MainDepthGeneration;
            }

            const bool safe = depthSafe && stencilSafe;
            if (!safe)
                ++R20DepthGateRejects;
            return safe;
        }

        void R20CancelInitialSeed(IDirect3DDevice9* device) noexcept
        {
            R9StereoSeeded = false;
            R9MonoSeeded = false;
            if (TrackedDepthStencil)
            {
                RightDepthSynchronized = false;
                if (StencilTestActive(device))
                    RightStencilSynchronized = false;
            }
            OutRunVR::RuntimeEligibility::StereoAllowed.store(false, std::memory_order_release);
            OutRunVR::RuntimeEligibility::RecoveryPending.store(true, std::memory_order_release);
            R20StereoEligibilityGate.store(false, std::memory_order_release);
        }

        void R20AcceptVerifiedBaseline() noexcept
        {
            OutRunVR::RuntimeEligibility::BaselineVerified();
            const bool allowed = OutRunVR::RuntimeEligibility::MayInjectStereo();
            R20StereoEligibilityGate.store(allowed, std::memory_order_release);
        }

        bool R20BuildCompleteColorBaseline(IDirect3DDevice9* device) noexcept
        {
            if (!device || !BackBuffer || !RightEyeSurface || !R9MonoSurface)
                return false;

            InternalPassScope guard;
            const HRESULT rightHr = device->StretchRect(
                BackBuffer, nullptr, RightEyeSurface, nullptr, D3DTEXF_NONE);
            const HRESULT monoHr = SUCCEEDED(rightHr)
                ? device->StretchRect(
                    BackBuffer, nullptr, R9MonoSurface, nullptr, D3DTEXF_NONE)
                : rightHr;
            if (FAILED(rightHr) || FAILED(monoHr))
            {
                ++R20BaselineCopyFailures;
                return false;
            }
            return true;
        }

        HRESULT __stdcall ClearDestR20(IDirect3DDevice9* device, DWORD count,
            const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
            DWORD stencil)
        {
            const bool seededBefore = R9StereoSeeded;
            const HRESULT hr = R20ClearR9Hook.stdcall<HRESULT>(
                device, count, rects, flags, color, z, stencil);

            R20ObserveFullDepthSeed(device, count, rects, flags, hr);

            if (!OutRunVR::RuntimeEligibility::HostFresh.load(std::memory_order_acquire))
            {
                R20CancelInitialSeed(device);
                return hr;
            }

            if (FAILED(hr) || !OutRunVR::RuntimeEligibility::IsReady(R20InstallState) ||
                !IsGameDevice(device) || InternalStereoPass)
                return hr;

            const bool legacyFirstSeed = !seededBefore && R9StereoSeeded;
            if (legacyFirstSeed)
            {
                if (!R20DepthHistorySafeForInitialSeed(device))
                {
                    ++R20LegacySeedsRejected;
                    R20CancelInitialSeed(device);
                    return hr;
                }

                R20AcceptVerifiedBaseline();
                if (!OutRunVR::RuntimeEligibility::MayInjectStereo())
                {
                    R20CancelInitialSeed(device);
                    return hr;
                }

                if (!R20FirstLegacySeedValidatedLogged)
                {
                    R20FirstLegacySeedValidatedLogged = true;
                    spdlog::info(
                        "VR R20/R23: legacy full-color first seed accepted only after common depth/stencil generation+draw-serial validation");
                }
                return hr;
            }

            if ((flags & D3DCLEAR_TARGET) == 0 || R9StereoSeeded)
                return hr;

            if (!StereoWanted() || !TargetIsBackBuffer() || R9DeferredDepth ||
                !R9CurrentDepthCanMirror() || !R9MonoSurface ||
                R9MonoBackupGap || FrameStereoIncomplete ||
                FrameFailureReason != OutRunVR::StereoFailureNone)
                return hr;

            if (!R20DepthHistorySafeForInitialSeed(device))
                return hr;

            if (!R20BuildCompleteColorBaseline(device))
            {
                R9MonoBackupGap = true;
                R9Poison(OutRunVR::StereoFailureResourceUnavailable,
                    "R20/bootstrap-color-baseline");
                return hr;
            }

            R20AcceptVerifiedBaseline();
            if (!OutRunVR::RuntimeEligibility::MayInjectStereo())
                return hr;

            R9StereoSeeded = true;
            R9MonoSeeded = true;
            R9MonoBackupGap = false;
            ++R20RelaxedSeeds;

            if (TrackedDepthStencil)
            {
                std::uint64_t baseline = std::max(
                    R9MainDepthContentSerial, R9MonoDepthContentSerial);
                if (++baseline == 0)
                    baseline = 1;
                R9MainDepthContentSerial = baseline;
                R9MonoDepthContentSerial = baseline;
                RightDepthSynchronized = true;
                if (StencilTestActive(device))
                    RightStencilSynchronized = true;
            }

            if (!R20FirstRelaxedSeedLogged)
            {
                R20FirstRelaxedSeedLogged = true;
                spdlog::info(
                    "VR R20/R23: relaxed bootstrap accepted after complete right+mono color baseline and common depth/stencil generation/draw-serial validation (rectCount={}, flags=0x{:08x})",
                    count, static_cast<unsigned>(flags));
            }
            return hr;
        }

        DWORD WINAPI R20InstallThread(void*)
        {
            using State = OutRunVR::RuntimeEligibility::InstallState;
            R20InstallState.store(State::Pending, std::memory_order_release);
            for (int attempt = 0; attempt < 4800; ++attempt)
            {
                const std::uint32_t r9 = R9InstallState.load(std::memory_order_acquire);
                const std::uint32_t r13 = R13InstallState.load(std::memory_order_acquire);

                if (r9 == R9InstallFailed || r13 == R13InstallFailed)
                {
                    R20InstallState.store(State::Failed, std::memory_order_release);
                    spdlog::error(
                        "VR R20: base R9/R13 transaction failed; relaxed bootstrap not installed");
                    return 0;
                }

                if (r9 == R9InstallReady && r13 == R13InstallReady)
                {
                    R20ClearR9Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&ClearDestR9), ClearDestR20,
                        safetyhook::InlineHook::StartDisabled);
                    const bool enabled = R20ClearR9Hook &&
                        R20ClearR9Hook.enable().has_value();
                    if (!enabled)
                    {
                        R20ClearR9Hook = {};
                        R20InstallState.store(State::Failed, std::memory_order_release);
                        spdlog::error(
                            "VR R20: failed to enable R9 clear bootstrap transaction; base fail-closed policy remains active");
                        return 0;
                    }
                    R20InstallState.store(State::Ready, std::memory_order_release);
                    spdlog::info(
                        "VR R20 PRODUCTION: first stereo seed requires current-generation Z/stencil clears; disabled-first bootstrap transaction READY");
                    return 0;
                }
                Sleep(25);
            }

            R20InstallState.store(State::Failed, std::memory_order_release);
            spdlog::warn(
                "VR R20: timed out waiting for R9/R13 renderer transaction; bootstrap overlay FAILED");
            return 0;
        }

        class VRProductionBootstrapR20Hook : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRProductionBootstrapR20";
            }

            bool validate() override { return true; }

            bool apply() override
            {
                using State = OutRunVR::RuntimeEligibility::InstallState;
                R20InstallState.store(State::Pending, std::memory_order_release);
                HANDLE thread = CreateThread(nullptr, 0, R20InstallThread, nullptr, 0, nullptr);
                if (!thread)
                {
                    R20InstallState.store(State::Failed, std::memory_order_release);
                    spdlog::error(
                        "VR R20: failed to create bootstrap installer thread: {}",
                        GetLastError());
                    return false;
                }
                CloseHandle(thread);
                return true;
            }

            static VRProductionBootstrapR20Hook instance;
        };

        VRProductionBootstrapR20Hook VRProductionBootstrapR20Hook::instance;
    }
}
