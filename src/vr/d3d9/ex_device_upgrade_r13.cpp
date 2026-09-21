// R13 hardening wrapper. cmake marks ex_device_upgrade.cpp HEADER_FILE_ONLY
// and compiles this TU instead. Optimization is disabled only while compiling
// the included compatibility implementation so InstallManagedResourceCompat is
// guaranteed to remain a hookable call boundary; R13 must remove the temporary
// Reset hook synchronously before CreateDeviceEx returns the device to the game.

#include <array>
#include <atomic>
#include <cstdint>

#include "r13_bridge.hpp"

#pragma optimize("", off)
#include "ex_device_upgrade.cpp"
#pragma optimize("", on)

namespace OutRunVRD3D9ExUpgradeR13
{
    namespace
    {
        constexpr std::size_t TextureLockRectVtableIndex = 19;
        constexpr std::size_t TextureUnlockRectVtableIndex = 20;
        constexpr std::size_t TrackedManagedTextureCapacity = 256;

        SafetyHookInline R13InstallCompatHook{};
        SafetyHookInline R13CreateTextureCallbackHook{};
        SafetyHookInline R13TextureLockRectHook{};
        SafetyHookInline R13TextureUnlockRectHook{};

        std::array<std::atomic<IDirect3DTexture9*>,
            TrackedManagedTextureCapacity> TrackedManagedTextures{};
        std::atomic<std::uint32_t> TrackedManagedTextureCursor{0};
        std::atomic<std::uint64_t> ManagedTextureLockCalls{0};
        std::atomic<std::uint64_t> ManagedTextureLockFailures{0};
        std::atomic<std::uint64_t> ManagedTextureUnlockCalls{0};
        std::atomic<bool> FirstManagedTextureLockLogged{false};
        std::atomic<bool> FirstManagedTextureLockFailureLogged{false};
        std::atomic<bool> ResetHookDisarmedLogged{false};
        std::atomic<bool> CompatHardeningLogged{false};

        void TrackManagedTexture(IDirect3DTexture9* texture) noexcept
        {
            if (!texture)
                return;
            const std::uint32_t slot =
                TrackedManagedTextureCursor.fetch_add(1, std::memory_order_acq_rel) %
                static_cast<std::uint32_t>(TrackedManagedTextureCapacity);
            TrackedManagedTextures[slot].store(texture, std::memory_order_release);
        }

        bool IsTrackedManagedTexture(IDirect3DTexture9* texture) noexcept
        {
            if (!texture)
                return false;
            for (auto& entry : TrackedManagedTextures)
            {
                if (entry.load(std::memory_order_acquire) == texture)
                    return true;
            }
            return false;
        }

        HRESULT __stdcall TextureLockRectR13(IDirect3DTexture9* texture,
            UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
        {
            const HRESULT hr = R13TextureLockRectHook.stdcall<HRESULT>(
                texture, level, locked, rect, flags);
            if (!IsTrackedManagedTexture(texture))
                return hr;

            ++ManagedTextureLockCalls;
            if (!FirstManagedTextureLockLogged.exchange(true))
            {
                spdlog::info(
                    "VR D3D9Ex R13: translated MANAGED texture LockRect observed level={} flags=0x{:08X} hr=0x{:08X}; lock semantics are now explicitly monitored",
                    level, static_cast<unsigned>(flags), static_cast<unsigned>(hr));
            }
            if (FAILED(hr))
            {
                ++ManagedTextureLockFailures;
                if (!FirstManagedTextureLockFailureLogged.exchange(true))
                {
                    spdlog::error(
                        "VR D3D9Ex R13: translated MANAGED texture LockRect FAILED hr=0x{:08X}; this resource requires CPU-shadow emulation before D3D9Ex can be considered production-safe",
                        static_cast<unsigned>(hr));
                }
            }
            return hr;
        }

        HRESULT __stdcall TextureUnlockRectR13(IDirect3DTexture9* texture,
            UINT level)
        {
            const HRESULT hr = R13TextureUnlockRectHook.stdcall<HRESULT>(
                texture, level);
            if (IsTrackedManagedTexture(texture))
                ++ManagedTextureUnlockCalls;
            return hr;
        }

        void EnsureTextureLockInstrumentation(IDirect3DTexture9* texture)
        {
            if (!texture ||
                (R13TextureLockRectHook && R13TextureUnlockRectHook))
                return;
            void** vtable = *reinterpret_cast<void***>(texture);
            if (!vtable)
                return;
            if (!R13TextureLockRectHook)
                R13TextureLockRectHook = safetyhook::create_inline(
                    vtable[TextureLockRectVtableIndex], TextureLockRectR13);
            if (!R13TextureUnlockRectHook)
                R13TextureUnlockRectHook = safetyhook::create_inline(
                    vtable[TextureUnlockRectVtableIndex], TextureUnlockRectR13);
            if (!R13TextureLockRectHook || !R13TextureUnlockRectHook)
                spdlog::warn(
                    "VR D3D9Ex R13: failed to install translated MANAGED texture LockRect/UnlockRect diagnostics");
        }

        HRESULT __stdcall CreateTextureCompatDestR13(
            IDirect3DDevice9* device, UINT width, UINT height, UINT levels,
            DWORD usage, D3DFORMAT format, D3DPOOL pool,
            IDirect3DTexture9** texture, HANDLE* sharedHandle)
        {
            const HRESULT hr = R13CreateTextureCallbackHook.stdcall<HRESULT>(
                device, width, height, levels, usage, format, pool,
                texture, sharedHandle);
            if (pool == D3DPOOL_MANAGED && SUCCEEDED(hr) && texture && *texture &&
                OutRunVRD3D9ExUpgrade::IsCompatDevice(device))
            {
                TrackManagedTexture(*texture);
                EnsureTextureLockInstrumentation(*texture);
            }
            return hr;
        }

        bool InstallManagedResourceCompatR13(IDirect3DDevice9Ex* deviceEx)
        {
            const bool installed = R13InstallCompatHook.call<bool>(deviceEx);
            if (!installed)
                return false;

            // Keep the temporary ResetEx shim armed through R14/R15 validation.
            // The final R15 wrapper performs the synchronous same-thread handoff
            // immediately before the promoted device is allowed to escape
            // CreateDeviceEx, so there is no externally visible Reset gap.

            if (!R13CreateTextureCallbackHook)
            {
                R13CreateTextureCallbackHook = safetyhook::create_inline(
                    reinterpret_cast<void*>(
                        &OutRunVRD3D9ExUpgrade::CreateTextureCompatDest),
                    CreateTextureCompatDestR13);
            }
            if (!R13CreateTextureCallbackHook)
            {
                spdlog::warn(
                    "VR D3D9Ex R13: failed to hook managed texture creation callback for Lock/Unlock diagnostics");
            }
            else if (!CompatHardeningLogged.exchange(true))
            {
                spdlog::info(
                    "VR D3D9Ex R13: compatibility hardening active; legacy ResetEx shim retained through final R15 validation and translated MANAGED texture Lock/Unlock diagnostics armed");
            }
            return true;
        }

        class D3D9ExR13HardeningHook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRD3D9ExR13Hardening";
            }
            bool validate() override { return Settings::VRPreferD3D9Ex; }
            bool apply() override
            {
                R13InstallCompatHook = safetyhook::create_inline(
                    reinterpret_cast<void*>(
                        &OutRunVRD3D9ExUpgrade::InstallManagedResourceCompat),
                    InstallManagedResourceCompatR13);
                if (!R13InstallCompatHook)
                {
                    spdlog::error(
                        "VR D3D9Ex R13: failed to intercept managed compatibility installation; experimental Ex promotion disabled for safety");
                    return false;
                }
                return true;
            }
            static D3D9ExR13HardeningHook instance;
        };

        D3D9ExR13HardeningHook D3D9ExR13HardeningHook::instance;
    }

    bool IsCompatDevice(IDirect3DDevice9* device) noexcept
    {
        return OutRunVRD3D9ExUpgrade::IsCompatDevice(device);
    }

    void DisarmLegacyResetHook() noexcept
    {
        if (OutRunVRD3D9ExUpgrade::ResetCompatHook)
        {
            OutRunVRD3D9ExUpgrade::ResetCompatHook = {};
            if (!ResetHookDisarmedLogged.exchange(true))
            {
                spdlog::info(
                    "VR D3D9Ex R13: legacy compatibility Reset inline hook disarmed synchronously; stereo Reset callback is sole reset owner");
            }
        }
    }

    HRESULT NormalizeLegacyPresentResult(
        IDirect3DDevice9* device, HRESULT result) noexcept
    {
        if (!OutRunVRD3D9ExUpgrade::IsCompatDevice(device))
            return result;
        if (result == S_PRESENT_MODE_CHANGED)
            return D3DERR_DEVICELOST;
        if (result == S_PRESENT_OCCLUDED)
        {
            return OutRunVRD3D9ExUpgrade::CompatWindowed.load(
                std::memory_order_acquire)
                ? D3D_OK : D3DERR_DEVICELOST;
        }
        return result;
    }

    bool ResetCompatDevice(IDirect3DDevice9* device,
        D3DPRESENT_PARAMETERS* params, HRESULT& result) noexcept
    {
        result = D3DERR_INVALIDCALL;
        if (!params || !OutRunVRD3D9ExUpgrade::IsCompatDevice(device))
            return false;

        IDirect3DDevice9Ex* deviceEx = nullptr;
        const HRESULT qi = device->QueryInterface(
            __uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&deviceEx));
        if (FAILED(qi) || !deviceEx)
            return false;

        D3DDISPLAYMODEEX fullscreen{};
        D3DDISPLAYMODEEX* fullscreenPtr =
            OutRunVRD3D9ExUpgrade::BuildFullscreenMode(
                deviceEx, params, fullscreen);
        result = deviceEx->ResetEx(params, fullscreenPtr);
        deviceEx->Release();
        if (SUCCEEDED(result))
        {
            OutRunVRD3D9ExUpgrade::UpdateCompatPresentationState(device, params);
            OutRunVRD3D9ExUpgrade::RestoreClassicResetState(device);
        }
        ++OutRunVRD3D9ExUpgrade::ResetExRedirects;
        spdlog::info(
            "VR D3D9Ex R13: authoritative stereo Reset path called ResetEx + classic-state replay hr=0x{:08X}; no competing Reset inline hook",
            static_cast<unsigned>(result));
        return true;
    }
}
