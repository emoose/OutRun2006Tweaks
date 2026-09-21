// R15 D3D9Ex compatibility correctness overlay.
//
// Keeps the validated R14 managed-texture implementation intact while fixing
// three review findings at the final Ex translation boundary:
//   * a failed partial-RECT upload may never fall back to a whole-mip copy;
//   * GenerateMipSubLevels keeps the coherent level-0 CPU backing while
//     invalidating generated lower-mip shadows;
//   * ResetEx replays the classic D3D9 baseline plus the state families that
//     R14/R13 did not explicitly restore, and publishes a health bit so the
//     stereo layer can remain fail-closed if that replay is incomplete.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "ex_device_upgrade_r14.cpp"
#include "../runtime_eligibility.hpp"

namespace OutRunVRStereo
{
    bool InstallStereoHooksSynchronously(IDirect3DDevice9* device) noexcept;
}

namespace OutRunVRD3D9ExUpgradeR13
{
    namespace
    {
        SafetyHookInline R15InstallCompatR14Hook{};
        SafetyHookInline R15TextureUnlockR14Hook{};
        SafetyHookInline R15GenerateMipR14Hook{};
        SafetyHookInline R15ResetCompatR13Hook{};

        std::atomic<bool> R15ResetStateHealthy{true};
        std::atomic<std::uint64_t> R15PartialUploadFailClosed{0};
        std::atomic<std::uint64_t> R15GeneratedMipInvalidations{0};
        std::atomic<std::uint64_t> R15ResetStateRestores{0};
        std::atomic<std::uint64_t> R15ResetStateFailures{0};
        bool R15FirstPartialRejectLogged = false;
        bool R15FirstMipInvalidateLogged = false;
        bool R15FirstResetFailureLogged = false;

        struct R15ClassicExtraBaseline
        {
            D3DMATRIX world{};
            D3DMATRIX view{};
            D3DMATRIX projection{};
            std::array<D3DMATRIX, 8> texture{};
            D3DMATERIAL9 material{};
            D3DCLIPSTATUS9 clipStatus{};
            UINT currentPalette = 0;
            float nPatchMode = 0.0f;
            bool clipStatusValid = false;
            bool paletteValid = false;
            bool ready = false;
        };

        R15ClassicExtraBaseline R15ClassicExtra{};
        IDirect3DStateBlock9* R15ClassicAllState = nullptr;
        std::mutex R15ClassicExtraMutex;

        bool R15CaptureClassicExtraBaseline(IDirect3DDevice9* device) noexcept
        {
            if (!device)
                return false;

            R15ClassicExtraBaseline next{};
            IDirect3DStateBlock9* allState = nullptr;
            if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &allState)) || !allState)
                return false;

            if (FAILED(device->GetTransform(D3DTS_WORLD, &next.world)) ||
                FAILED(device->GetTransform(D3DTS_VIEW, &next.view)) ||
                FAILED(device->GetTransform(D3DTS_PROJECTION,
                    &next.projection)) ||
                FAILED(device->GetMaterial(&next.material)))
            {
                allState->Release();
                return false;
            }

            for (DWORD stage = 0; stage < next.texture.size(); ++stage)
            {
                if (FAILED(device->GetTransform(
                        static_cast<D3DTRANSFORMSTATETYPE>(
                            D3DTS_TEXTURE0 + stage),
                        &next.texture[stage])))
                {
                    allState->Release();
                    return false;
                }
            }

            next.clipStatusValid =
                SUCCEEDED(device->GetClipStatus(&next.clipStatus));
            next.paletteValid =
                SUCCEEDED(device->GetCurrentTexturePalette(
                    &next.currentPalette));
            next.nPatchMode = device->GetNPatchMode();
            next.ready = true;

            std::lock_guard<std::mutex> lock(R15ClassicExtraMutex);
            if (R15ClassicAllState)
                R15ClassicAllState->Release();
            R15ClassicAllState = allState;
            R15ClassicExtra = next;
            R15ResetStateHealthy.store(true, std::memory_order_release);
            OutRunVR::RuntimeEligibility::SetExternalSafetyBlock(false);
            return true;
        }

        UINT R15PixelFloatConstantCount(const D3DCAPS9& caps) noexcept
        {
            const UINT major = D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion);
            if (major >= 3)
                return 224;
            if (major >= 2)
                return 32;
            if (major >= 1)
                return 8;
            return 0;
        }

        bool R15ZeroClassicShaderConstants(IDirect3DDevice9* device) noexcept
        {
            if (!device)
                return false;

            D3DCAPS9 caps{};
            if (FAILED(device->GetDeviceCaps(&caps)))
                return false;

            bool ok = true;
            std::array<float, 256 * 4> zeroFloat{};
            std::array<int, 16 * 4> zeroInt{};
            std::array<BOOL, 16> zeroBool{};

            const UINT vertexFloatCount =
                std::min<UINT>(caps.MaxVertexShaderConst, 256u);
            if (vertexFloatCount != 0 &&
                FAILED(device->SetVertexShaderConstantF(
                    0, zeroFloat.data(), vertexFloatCount)))
                ok = false;

            const UINT vsMajor =
                D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion);
            if (vsMajor >= 2)
            {
                if (FAILED(device->SetVertexShaderConstantI(
                        0, zeroInt.data(), 16)))
                    ok = false;
                if (FAILED(device->SetVertexShaderConstantB(
                        0, zeroBool.data(), 16)))
                    ok = false;
            }

            const UINT pixelFloatCount = R15PixelFloatConstantCount(caps);
            if (pixelFloatCount != 0 &&
                FAILED(device->SetPixelShaderConstantF(
                    0, zeroFloat.data(), pixelFloatCount)))
                ok = false;

            const UINT psMajor =
                D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion);
            if (psMajor >= 2)
            {
                if (FAILED(device->SetPixelShaderConstantI(
                        0, zeroInt.data(), 16)))
                    ok = false;
                if (FAILED(device->SetPixelShaderConstantB(
                        0, zeroBool.data(), 16)))
                    ok = false;
            }
            return ok;
        }

        bool R15RestoreClassicAllState(IDirect3DDevice9* device) noexcept
        {
            if (!device)
                return false;
            IDirect3DStateBlock9* state = nullptr;
            {
                std::lock_guard<std::mutex> lock(R15ClassicExtraMutex);
                state = R15ClassicAllState;
                if (state) state->AddRef();
            }
            if (!state)
                return false;
            const HRESULT hr = state->Apply();
            state->Release();
            return SUCCEEDED(hr);
        }

        bool R15RestoreClassicExtraBaseline(IDirect3DDevice9* device) noexcept
        {
            if (!device)
                return false;

            R15ClassicExtraBaseline baseline{};
            {
                std::lock_guard<std::mutex> lock(R15ClassicExtraMutex);
                if (!R15ClassicExtra.ready)
                    return false;
                baseline = R15ClassicExtra;
            }

            bool ok = true;
            if (FAILED(device->SetTransform(D3DTS_WORLD, &baseline.world)))
                ok = false;
            if (FAILED(device->SetTransform(D3DTS_VIEW, &baseline.view)))
                ok = false;
            if (FAILED(device->SetTransform(
                    D3DTS_PROJECTION, &baseline.projection)))
                ok = false;
            for (DWORD stage = 0; stage < baseline.texture.size(); ++stage)
            {
                if (FAILED(device->SetTransform(
                        static_cast<D3DTRANSFORMSTATETYPE>(
                            D3DTS_TEXTURE0 + stage),
                        &baseline.texture[stage])))
                    ok = false;
            }
            if (FAILED(device->SetMaterial(&baseline.material)))
                ok = false;
            if (baseline.clipStatusValid &&
                FAILED(device->SetClipStatus(&baseline.clipStatus)))
                ok = false;
            if (baseline.paletteValid &&
                FAILED(device->SetCurrentTexturePalette(
                    baseline.currentPalette)))
                ok = false;
            if (FAILED(device->SetNPatchMode(baseline.nPatchMode)))
                ok = false;
            if (!R15ZeroClassicShaderConstants(device))
                ok = false;

            return ok;
        }

        bool InstallManagedResourceCompatR15(
            IDirect3DDevice9Ex* deviceEx) noexcept
        {
            const bool installed =
                R15InstallCompatR14Hook.call<bool>(deviceEx);
            if (!installed || !deviceEx)
                return installed;

            auto* const device = static_cast<IDirect3DDevice9*>(deviceEx);
            if (!R15CaptureClassicExtraBaseline(device))
            {
                R15ResetStateHealthy.store(false, std::memory_order_release);
                R14AbandonCompatDevice(device);
                OutRunVRD3D9ExUpgrade::ClearCompatHooks();
                {
                    std::lock_guard<std::mutex> lock(R15ClassicExtraMutex);
                    if (R15ClassicAllState)
                    {
                        R15ClassicAllState->Release();
                        R15ClassicAllState = nullptr;
                    }
                    R15ClassicExtra = {};
                }
                spdlog::error(
                    "VR R15 EX: could not capture fresh-device full classic-state baseline; Ex promotion rolled back transactionally");
                return false;
            }

            // All Ex compatibility layers are now validated. Transfer Reset
            // ownership and install the stereo base hooks on this same
            // CreateDevice caller thread before the device can be returned to
            // game code. If the handoff fails, reject the Ex device entirely.
            DisarmLegacyResetHook();
            if (!OutRunVRStereo::InstallStereoHooksSynchronously(device))
            {
                R15ResetStateHealthy.store(false, std::memory_order_release);
                OutRunVRD3D9ExUpgrade::SetFinalCompatOverlayReady(false);
                R14AbandonCompatDevice(device);
                OutRunVRD3D9ExUpgrade::ClearCompatHooks();
                {
                    std::lock_guard<std::mutex> lock(R15ClassicExtraMutex);
                    if (R15ClassicAllState)
                    {
                        R15ClassicAllState->Release();
                        R15ClassicAllState = nullptr;
                    }
                    R15ClassicExtra = {};
                }
                spdlog::error(
                    "VR R15 EX: synchronous CreateDevice-thread stereo handoff failed; Ex promotion rejected before device exposure");
                return false;
            }

            spdlog::info(
                "VR R15 EX: full D3DSBT_ALL baseline + synchronous Reset/stereo handoff ACTIVE; partial-RECT and generated-mip correctness overlay ACTIVE");
            return true;
        }

        HRESULT R15UploadLevel(R14ShadowEntry& entry, UINT level) noexcept
        {
            if (!entry.device || !entry.cpu || !entry.gpu)
                return E_FAIL;

            R14InternalUploadScope internal;
            IDirect3DSurface9* source = nullptr;
            IDirect3DSurface9* destination = nullptr;
            HRESULT hr = R14TextureGetSurfaceLevelHook.stdcall<HRESULT>(
                entry.cpu, level, &source);
            if (SUCCEEDED(hr))
            {
                hr = R14TextureGetSurfaceLevelHook.stdcall<HRESULT>(
                    entry.gpu, level, &destination);
            }
            if (SUCCEEDED(hr) && source && destination)
            {
                const RECT* rect = entry.lockRectValid[level]
                    ? &entry.lockRect[level] : nullptr;
                POINT point{};
                POINT* destinationPoint = nullptr;
                if (rect)
                {
                    point.x = rect->left;
                    point.y = rect->top;
                    destinationPoint = &point;
                }
                hr = entry.device->UpdateSurface(
                    source, rect, destination, destinationPoint);
            }
            else if (SUCCEEDED(hr))
            {
                hr = E_FAIL;
            }
            if (source) source->Release();
            if (destination) destination->Release();

            if (FAILED(hr))
            {
                if (!entry.lockRectValid[level])
                {
                    // Whole-mip locks own every texel in the CPU shadow, so the
                    // validated R14 whole-level fallback remains coherent.
                    hr = R14CopyWholeLevelByLock(entry, level);
                }
                else
                {
                    ++R15PartialUploadFailClosed;
                    if (!R15FirstPartialRejectLogged)
                    {
                        R15FirstPartialRejectLogged = true;
                        spdlog::error(
                            "VR R15 EX: partial-RECT UpdateSurface failed; whole-mip fallback suppressed to prevent unrelated texel corruption");
                    }
                }
            }
            return hr;
        }

        HRESULT __stdcall TextureUnlockRectR15(IDirect3DTexture9* texture,
            UINT level)
        {
            const R14EntryPtr entry = R14Find(texture);
            if (!entry || level >= R14MaxTrackedLevels)
                return R14TextureUnlockR13Hook.stdcall<HRESULT>(texture, level);

            IDirect3DTexture9* cpuToRelease = nullptr;
            HRESULT result = D3D_OK;
            bool retired = false;
            {
                std::unique_lock<std::mutex> lock(entry->mutex);
                const std::uint32_t bit = 1u << level;
                if (entry->mode == R14ShadowMode::DirectOnly ||
                    (entry->lockedMask & bit) == 0)
                {
                    lock.unlock();
                    return R14TextureUnlockR13Hook.stdcall<HRESULT>(
                        texture, level);
                }

                const bool readOnly = (entry->readOnlyMask & bit) != 0;
                const HRESULT unlockHr = entry->cpu->UnlockRect(level);
                entry->lockedMask &= ~bit;
                entry->readOnlyMask &= ~bit;

                if (FAILED(unlockHr))
                {
                    entry->validMask &= ~bit;
                    entry->retirePending = true;
                    result = unlockHr;
                }
                else if (entry->externalWriteDuringLock)
                {
                    result = D3DERR_INVALIDCALL;
                }
                else if (!readOnly)
                {
                    result = R15UploadLevel(*entry, level);
                    entry->lockRectValid[level] = false;
                    if (SUCCEEDED(result))
                    {
                        entry->dirtyMask &= ~bit;
                        ++R14ShadowUploads;
                    }
                    else
                    {
                        entry->validMask &= ~bit;
                        ++R14ShadowUploadFailed;
                        entry->retirePending = true;
                        if (!R14FirstUploadFailureLogged.exchange(true))
                        {
                            spdlog::error(
                                "VR R15 EX: exact mip CPU-shadow upload failed hr=0x{:08x}; shadow retires instead of overwriting unrelated GPU contents",
                                static_cast<unsigned>(result));
                        }
                    }
                }

                if (entry->retirePending && entry->lockedMask == 0)
                {
                    cpuToRelease = R14DetachShadowLocked(*entry);
                    retired = true;
                }
            }

            if (cpuToRelease) cpuToRelease->Release();
            if (retired)
                R14LogRetiredShadow("R15 coherency path retired at Unlock");
            return result;
        }

        void R15InvalidateGeneratedMipShadows(
            IDirect3DTexture9* texture) noexcept
        {
            const R14EntryPtr entry = R14Find(texture);
            if (!entry)
                return;

            bool concurrentWrite = false;
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                if (entry->mode == R14ShadowMode::DirectOnly)
                    return;

                // GenerateMipSubLevels derives lower GPU mips from GPU level 0.
                // Level 0 itself remains coherent with the SYSTEMMEM shadow when
                // no CPU Lock is in flight; only generated lower mips become
                // unknown from the shadow's point of view.
                entry->validMask &= 1u;
                entry->dirtyMask &= 1u;
                for (std::uint32_t mip = 1;
                     mip < R14MaxTrackedLevels; ++mip)
                    entry->lockRectValid[mip] = false;

                concurrentWrite = entry->lockedMask != 0;
                if (concurrentWrite)
                {
                    entry->externalWriteDuringLock = true;
                    entry->retirePending = true;
                }
            }

            ++R15GeneratedMipInvalidations;
            if (!R15FirstMipInvalidateLogged)
            {
                R15FirstMipInvalidateLogged = true;
                spdlog::info(
                    "VR R15 EX: GenerateMipSubLevels preserves coherent level-0 CPU backing and invalidates only generated lower-mip shadows");
            }
            if (concurrentWrite &&
                !R14FirstConcurrentWriteLogged.exchange(true))
            {
                spdlog::error(
                    "VR R15 EX: GenerateMipSubLevels overlapped a CPU-shadow Lock; shadow will retire at the matching Unlock");
            }
        }

        void __stdcall TextureGenerateMipSubLevelsDestR15(
            IDirect3DTexture9* texture)
        {
            // Bypass R14's conservative whole-shadow retirement while retaining
            // its underlying device-method trampoline.
            R14TextureGenerateMipSubLevelsHook.stdcall<void>(texture);
            if (R14InternalUploadDepth == 0)
                R15InvalidateGeneratedMipShadows(texture);
        }

        bool ResetCompatDeviceR15(IDirect3DDevice9* device,
            D3DPRESENT_PARAMETERS* params, HRESULT& result) noexcept
        {
            R15ResetStateHealthy.store(false, std::memory_order_release);
            if (IsCompatDevice(device))
                OutRunVR::RuntimeEligibility::SetExternalSafetyBlock(true);
            const bool handled = R15ResetCompatR13Hook.call<bool>(
                device, params, result);
            if (!handled || FAILED(result))
                return handled;

            // R13 already performed ResetEx and its classic render/stage/sampler
            // replay. Re-run that replay so its bool is authoritative, then
            // restore state families that R13 did not cover.
            const bool coreHealthy =
                OutRunVRD3D9ExUpgrade::RestoreClassicResetState(device);
            const bool allStateHealthy = R15RestoreClassicAllState(device);
            const bool extraHealthy =
                R15RestoreClassicExtraBaseline(device);
            const bool healthy = coreHealthy && allStateHealthy && extraHealthy;
            R15ResetStateHealthy.store(healthy, std::memory_order_release);
            OutRunVR::RuntimeEligibility::SetExternalSafetyBlock(!healthy);

            if (healthy)
            {
                ++R15ResetStateRestores;
            }
            else
            {
                ++R15ResetStateFailures;
                if (!R15FirstResetFailureLogged)
                {
                    R15FirstResetFailureLogged = true;
                    spdlog::error(
                        "VR R15 EX: ResetEx succeeded but classic D3D9 state replay was incomplete; stereo must remain fail-closed");
                }
            }
            return true;
        }

        void R15RollbackHooks() noexcept
        {
            R15ResetCompatR13Hook = {};
            R15GenerateMipR14Hook = {};
            R15TextureUnlockR14Hook = {};
            R15InstallCompatR14Hook = {};
        }

        class VRD3D9ExUpgradeR15Hook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRD3D9ExUpgradeR15";
            }
            bool validate() override
            {
                return Settings::VRPreferD3D9Ex;
            }
            bool apply() override
            {
                OutRunVRD3D9ExUpgrade::SetFinalCompatOverlayReady(false);
                if (!R14InstallCompatR13Hook || !R14CreateTextureR13Hook ||
                    !R14TextureLockR13Hook || !R14TextureUnlockR13Hook)
                {
                    spdlog::error(
                        "VR R15 EX: lower R14 compatibility overlay is unavailable; Ex promotion remains disabled");
                    return false;
                }
                const auto disabled = safetyhook::InlineHook::StartDisabled;
                R15InstallCompatR14Hook = safetyhook::create_inline(
                    reinterpret_cast<void*>(&InstallManagedResourceCompatR14),
                    InstallManagedResourceCompatR15, disabled);
                R15TextureUnlockR14Hook = safetyhook::create_inline(
                    reinterpret_cast<void*>(&TextureUnlockRectR14),
                    TextureUnlockRectR15, disabled);
                R15GenerateMipR14Hook = safetyhook::create_inline(
                    reinterpret_cast<void*>(&TextureGenerateMipSubLevelsDestR14),
                    TextureGenerateMipSubLevelsDestR15, disabled);
                R15ResetCompatR13Hook = safetyhook::create_inline(
                    reinterpret_cast<void*>(&ResetCompatDevice),
                    ResetCompatDeviceR15, disabled);

                SafetyHookInline* hooks[]{
                    &R15InstallCompatR14Hook,
                    &R15TextureUnlockR14Hook,
                    &R15GenerateMipR14Hook,
                    &R15ResetCompatR13Hook
                };
                for (auto* hook : hooks)
                {
                    if (!*hook || !hook->enable().has_value())
                    {
                        R15RollbackHooks();
                        OutRunVRD3D9ExUpgrade::SetFinalCompatOverlayReady(false);
                        spdlog::error(
                            "VR R15 EX: correctness overlay hook transaction failed; R14 remains authoritative");
                        return false;
                    }
                }

                OutRunVRD3D9ExUpgrade::SetFinalCompatOverlayReady(true);
                spdlog::info(
                    "VR R15 EX: partial-RECT fail-close + generated-mip lifetime + ResetEx classic-state replay READY; final promotion gate ARMED");
                return true;
            }

            static VRD3D9ExUpgradeR15Hook instance;
        };

        VRD3D9ExUpgradeR15Hook VRD3D9ExUpgradeR15Hook::instance;
    }

    bool LastResetStateReplaySucceeded() noexcept
    {
        return R15ResetStateHealthy.load(std::memory_order_acquire);
    }
}
