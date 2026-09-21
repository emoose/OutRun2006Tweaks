// R13 renderer-pose hardening wrapper. The validated renderer is included in
// this TU; cmake marks outrun_renderer.cpp HEADER_FILE_ONLY.

#include "vr/d3d9/r13_bridge.hpp"
#include "outrun_renderer.cpp"

namespace OutRunVRRenderer
{
    namespace
    {
        SafetyHookInline R13WvpCallbackHook{};
        std::atomic<bool> R13WvpHookReady{false};
        std::uint64_t R13OffscreenWvpBypasses = 0;
        std::uint64_t R13ScreenSpaceWvpBypasses = 0;
        std::uint64_t R13UnknownProjectionWvpBypasses = 0;
        std::uint64_t R13PolicyMismatchBypasses = 0;
        std::uint64_t R13FragileEffectWvpBypasses = 0;
        bool R13FirstOffscreenBypassLogged = false;
        bool R13FirstScreenSpaceBypassLogged = false;
        bool R13FirstUnknownProjectionLogged = false;
        bool R13FirstPolicyMismatchLogged = false;
        bool R13FirstPerspectiveWorldLogged = false;
        bool R13FirstFragileEffectLogged = false;
        bool R13FirstFragileStateReadFailureLogged = false;

        OutRunVR::PassPolicy::RenderSemantic R13CurrentRenderSemantic(
            float& projectionM34, float& projectionM44) noexcept
        {
            projectionM34 = 0.0f;
            projectionM44 = 0.0f;

            const auto passSnapshot = OutRunVRStereo::CurrentPoseInjectionSnapshot();
            const auto targetPolicy = passSnapshot.policy;
            const bool mainBackbufferPosePass =
                passSnapshot.legacyMainBackbufferInvariant;

            if (targetPolicy != OutRunVR::PassPolicy::PoseInjectionPolicy::MainBackbuffer)
            {
                return OutRunVR::PassPolicy::ClassifyRenderSemanticChecked(
                    targetPolicy, mainBackbufferPosePass,
                    OutRunVR::PassPolicy::ProjectionClass::Unknown);
            }

            float projection[16]{};
            auto projectionClass = OutRunVR::PassPolicy::ProjectionClass::Unknown;
            if (GetRendererBaseProjection(projection))
            {
                // D3DMATRIX memory layout: [11] == _34, [15] == _44.
                // OutRun's D3DXMatrixPerspectiveFovRH path normally reports
                // _34=-1/_44=0; orthographic screen-space reports _34=0/_44=1.
                projectionM34 = projection[11];
                projectionM44 = projection[15];
                projectionClass = OutRunVR::PassPolicy::ClassifyProjectionSignature(
                    projectionM34, projectionM44);
            }

            return OutRunVR::PassPolicy::ClassifyRenderSemanticChecked(
                targetPolicy, mainBackbufferPosePass, projectionClass);
        }

        bool R13FragileEffectNeedsZeroDisparity(IDirect3DDevice9* device) noexcept
        {
            if (!device)
                return false;

            DWORD alphaBlend = FALSE;
            DWORD alphaTest = FALSE;
            DWORD zWrite = TRUE;
            DWORD zEnable = D3DZB_TRUE;
            DWORD cullMode = D3DCULL_CCW;
            if (FAILED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend)) ||
                FAILED(device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest)) ||
                FAILED(device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite)) ||
                FAILED(device->GetRenderState(D3DRS_ZENABLE, &zEnable)) ||
                FAILED(device->GetRenderState(D3DRS_CULLMODE, &cullMode)))
            {
                // A failed state read is not enough evidence to demote all world
                // geometry. Keep the existing world classifier authoritative,
                // but leave a diagnostic breadcrumb for hardware testing.
                if (!R13FirstFragileStateReadFailureLogged)
                {
                    R13FirstFragileStateReadFailureLogged = true;
                    spdlog::warn(
                        "VR R13 effect policy: render-state snapshot unavailable; retaining normal world-stereo classification");
                }
                return false;
            }

            const auto policy = OutRunVR::PassPolicy::ClassifyEffectStereo(
                alphaBlend != FALSE,
                alphaTest != FALSE,
                zWrite != FALSE,
                zEnable != D3DZB_FALSE,
                cullMode == D3DCULL_NONE);
            return !OutRunVR::PassPolicy::AllowsEffectWorldStereo(policy);
        }

        HRESULT __stdcall SetVertexShaderConstantFDestR13(
            IDirect3DDevice9* device, UINT startRegister, const float* constantData, UINT vector4fCount)
        {
            // R13 creates its callback hook disabled, publishes the trampoline,
            // then explicitly enables it. If a render thread enters while the
            // enable transaction is completing, the already-owned trampoline
            // is safe to call and preserves the validated base renderer path.
            if (!R13WvpHookReady.load(std::memory_order_acquire))
            {
                InvalidateVerifiedWvp();
                if (R13WvpCallbackHook)
                {
                    return R13WvpCallbackHook.stdcall<HRESULT>(
                        device, startRegister, constantData, vector4fCount);
                }
                return SetVertexShaderConstantFHook.stdcall<HRESULT>(
                    device, startRegister, constantData, vector4fCount);
            }

            if (IsGameDevice(device) && constantData &&
                !OutRunVRStereo::IsInternalStereoPassActive() &&
                UploadTouchesOutRunWvp(startRegister, vector4fCount))
            {
                float projectionM34 = 0.0f;
                float projectionM44 = 0.0f;
                const auto semantic = R13CurrentRenderSemantic(
                    projectionM34, projectionM44);

                if (!OutRunVR::PassPolicy::AllowsWorldStereo(semantic))
                {
                    // Emulator-inspired fail-closed classification: only a
                    // perspective + main-backbuffer + verified c64 upload may
                    // enter the head-tracked world path. Reflections/shadows,
                    // orthographic HUD/UI, policy disagreement, and unknown
                    // projections keep stock game matrices and cannot seed
                    // stereo world replay.
                    InvalidateVerifiedWvp();

                    if (semantic == OutRunVR::PassPolicy::RenderSemantic::Auxiliary)
                    {
                        ++R13OffscreenWvpBypasses;
                        if (!R13FirstOffscreenBypassLogged)
                        {
                            R13FirstOffscreenBypassLogged = true;
                            spdlog::info(
                                "VR R13: auxiliary/offscreen c64 WVP kept stock; HMD transform is main-backbuffer-only");
                        }
                    }
                    else if (semantic == OutRunVR::PassPolicy::RenderSemantic::ScreenSpace2D)
                    {
                        ++R13ScreenSpaceWvpBypasses;
                        if (!R13FirstScreenSpaceBypassLogged)
                        {
                            R13FirstScreenSpaceBypassLogged = true;
                            spdlog::info(
                                "VR R13 emulator policy: orthographic/screen-space c64 WVP kept stock; HUD/UI stays zero-disparity (_34={:.3f} _44={:.3f})",
                                projectionM34, projectionM44);
                        }
                    }
                    else if (semantic == OutRunVR::PassPolicy::RenderSemantic::PolicyMismatch)
                    {
                        ++R13PolicyMismatchBypasses;
                        if (!R13FirstPolicyMismatchLogged)
                        {
                            R13FirstPolicyMismatchLogged = true;
                            spdlog::error(
                                "VR R13 emulator policy: main-pass classifiers disagreed; c64 WVP kept stock fail-closed");
                        }
                    }
                    else
                    {
                        ++R13UnknownProjectionWvpBypasses;
                        if (!R13FirstUnknownProjectionLogged)
                        {
                            R13FirstUnknownProjectionLogged = true;
                            spdlog::warn(
                                "VR R13 emulator policy: unknown main-backbuffer projection kept stock fail-closed (_34={:.3f} _44={:.3f})",
                                projectionM34, projectionM44);
                        }
                    }

                    return SetVertexShaderConstantFHook.stdcall<HRESULT>(
                        device, startRegister, constantData, vector4fCount);
                }

                // Projected shadows and camera-facing alpha panels are prepared
                // against OutRun's stock camera. Applying an HMD WVP to them can
                // make the SBS halves disagree even though the opaque world is
                // correct. Keep these fragile effect classes zero-disparity by
                // deliberately preventing this WVP upload from becoming the
                // verified world draw. ExecuteStereoDraw will still replay the
                // object to BOTH eyes with the unmodified game WVP.
                if (R13FragileEffectNeedsZeroDisparity(device))
                {
                    InvalidateVerifiedWvp();
                    ++R13FragileEffectWvpBypasses;
                    if (!R13FirstFragileEffectLogged)
                    {
                        R13FirstFragileEffectLogged = true;
                        spdlog::info(
                            "VR R13 effect policy: translucent shadow/billboard/panel pass kept stock and duplicated zero-disparity in both SBS eyes");
                    }
                    return SetVertexShaderConstantFHook.stdcall<HRESULT>(
                        device, startRegister, constantData, vector4fCount);
                }

                if (!R13FirstPerspectiveWorldLogged)
                {
                    R13FirstPerspectiveWorldLogged = true;
                    spdlog::info(
                        "VR R13 emulator policy: main-backbuffer perspective class confirmed; c64 remains subject to authoritative WorldView*Projection verification");
                }
            }

            return R13WvpCallbackHook.stdcall<HRESULT>(
                device, startRegister, constantData, vector4fCount);
        }

        DWORD WINAPI R13RendererInstallThread(void*)
        {
            R13WvpHookReady.store(false, std::memory_order_release);
            for (int attempt = 0; attempt < 4800; ++attempt)
            {
                const std::uint32_t rendererState =
                    RendererInstallState.load(std::memory_order_acquire);
                if (rendererState == RendererInstallFailed)
                {
                    R13WvpHookReady.store(false, std::memory_order_release);
                    RendererInjectionAllowed.store(false, std::memory_order_release);
                    InvalidateVerifiedWvp();
                    spdlog::error(
                        "VR R13: base renderer hook transaction failed; WVP injection remains disabled");
                    return 0;
                }
                if (rendererState == RendererInstallReady)
                {
                    R13WvpCallbackHook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&SetVertexShaderConstantFDest),
                        SetVertexShaderConstantFDestR13,
                        safetyhook::InlineHook::StartDisabled);

                    bool enabled = false;
                    if (R13WvpCallbackHook)
                    {
                        const auto enableResult = R13WvpCallbackHook.enable();
                        enabled = enableResult.has_value();
                    }

                    if (enabled)
                    {
                        R13WvpHookReady.store(true, std::memory_order_release);
                        spdlog::info(
                            "VR R13: renderer WVP target+projection+effect classification guard armed via atomic renderer install handoff; disabled-first trampoline publish complete");
                    }
                    else
                    {
                        R13WvpHookReady.store(false, std::memory_order_release);
                        R13WvpCallbackHook = {};
                        RendererInjectionAllowed.store(false, std::memory_order_release);
                        InvalidateVerifiedWvp();
                        spdlog::error(
                            "VR R13: failed to create/enable renderer c64 callback hook; WVP injection disabled atomically to fail closed");
                    }
                    return 0;
                }
                Sleep(25);
            }
            R13WvpHookReady.store(false, std::memory_order_release);
            RendererInjectionAllowed.store(false, std::memory_order_release);
            InvalidateVerifiedWvp();
            spdlog::warn(
                "VR R13: renderer transaction did not become ready; WVP injection disabled fail-closed");
            return 0;
        }

        class VRRendererR13HardeningHook final : public Hook
        {
        public:
            std::string_view description() override { return "OpenXRVRRendererR13Hardening"; }
            bool validate() override { return true; }
            bool apply() override
            {
                R13WvpHookReady.store(false, std::memory_order_release);
                HANDLE thread = CreateThread(nullptr, 0, R13RendererInstallThread, nullptr, 0, nullptr);
                if (!thread)
                {
                    // Without the R13 classifier the base c64 injector must not
                    // continue in a less-safe mode. Treat installer-thread
                    // creation failure the same as hook-install failure.
                    RendererInjectionAllowed.store(false, std::memory_order_release);
                    InvalidateVerifiedWvp();
                    spdlog::error(
                        "VR R13: failed to create renderer hardening installer thread; WVP injection disabled fail-closed: {}",
                        GetLastError());
                    return false;
                }
                CloseHandle(thread);
                return true;
            }
            static VRRendererR13HardeningHook instance;
        };

        VRRendererR13HardeningHook VRRendererR13HardeningHook::instance;
    }
}
