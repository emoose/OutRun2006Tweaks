#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <new>
#include <mutex>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "hook_mgr.hpp"
#include "plugin.hpp"

namespace Settings
{
    extern Setting<bool> VRPreferD3D9Ex;
}

namespace OutRunVRD3D9ExUpgrade
{
    namespace
    {
        using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
        using Direct3DCreate9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);

        constexpr std::size_t TestCooperativeLevelVtableIndex = 3;
        constexpr std::size_t EvictManagedResourcesVtableIndex = 5;
        constexpr std::size_t ResetVtableIndex = 16;
        constexpr std::size_t CreateTextureVtableIndex = 23;
        constexpr std::size_t CreateVolumeTextureVtableIndex = 24;
        constexpr std::size_t CreateCubeTextureVtableIndex = 25;
        constexpr std::size_t CreateVertexBufferVtableIndex = 26;
        constexpr std::size_t CreateIndexBufferVtableIndex = 27;

        Direct3DCreate9Fn OriginalDirect3DCreate9 = nullptr;
        std::atomic<bool> FirstUpgradeLogged{false};
        std::atomic<bool> FirstFallbackLogged{false};
        std::atomic<bool> ThirdPartyLogged{false};
        std::atomic<bool> FirstCreateFlagsLogged{false};
        std::atomic<bool> FirstPureDeviceFallbackLogged{false};
        std::atomic<bool> FirstOverlayUnavailableLogged{false};
        std::atomic<bool> FinalCompatOverlayReady{false};
        std::atomic<IDirect3DDevice9*> CompatDevice{nullptr};

        SafetyHookInline TestCooperativeLevelCompatHook{};
        SafetyHookInline EvictManagedResourcesCompatHook{};
        SafetyHookInline ResetCompatHook{};
        SafetyHookInline CreateTextureCompatHook{};
        SafetyHookInline CreateVolumeTextureCompatHook{};
        SafetyHookInline CreateCubeTextureCompatHook{};
        SafetyHookInline CreateVertexBufferCompatHook{};
        SafetyHookInline CreateIndexBufferCompatHook{};

        std::atomic<std::uint64_t> ManagedTextureCreates{0};
        std::atomic<std::uint64_t> ManagedVolumeTextureCreates{0};
        std::atomic<std::uint64_t> ManagedCubeTextureCreates{0};
        std::atomic<std::uint64_t> ManagedVertexBufferCreates{0};
        std::atomic<std::uint64_t> ManagedIndexBufferCreates{0};
        std::atomic<std::uint64_t> ManagedCreateFailures{0};
        std::atomic<std::uint64_t> ManagedTextureDynamicFallbacks{0};
        std::atomic<std::uint64_t> ResetExRedirects{0};
        std::atomic<std::uint64_t> CooperativeLevelTranslations{0};
        std::atomic<std::uint64_t> ClassicResetStateRestores{0};
        std::atomic<std::uint64_t> ClassicResetStateRestoreFailures{0};
        std::atomic<bool> CompatWindowed{true};
        std::atomic<HWND> CompatFocusWindow{nullptr};
        std::atomic<bool> FirstCooperativeTranslationLogged{false};
        std::atomic<bool> FirstResetStateRestoreFailureLogged{false};

        struct CompatStateValue
        {
            DWORD state = 0;
            DWORD value = 0;
        };
        struct CompatStageStateValue
        {
            DWORD stage = 0;
            DWORD state = 0;
            DWORD value = 0;
        };
        struct CompatClassicBaseline
        {
            std::vector<CompatStateValue> render;
            std::vector<CompatStageStateValue> textureStage;
            std::vector<CompatStageStateValue> sampler;
            bool ready = false;
        };
        CompatClassicBaseline ClassicBaseline{};
        std::mutex ClassicBaselineMutex;

        bool CaptureClassicBaseline(IDirect3DDevice9* device) noexcept
        {
            if (!device) return false;
            try
            {
                CompatClassicBaseline next{};
                next.render.reserve(192);
                for (DWORD state = 1; state <= 255; ++state)
                {
                    DWORD value = 0;
                    if (SUCCEEDED(device->GetRenderState(
                            static_cast<D3DRENDERSTATETYPE>(state), &value)))
                        next.render.push_back({ state, value });
                }
                for (DWORD stage = 0; stage < 8; ++stage)
                {
                    for (DWORD state = 1; state <= 32; ++state)
                    {
                        DWORD value = 0;
                        if (SUCCEEDED(device->GetTextureStageState(stage,
                                static_cast<D3DTEXTURESTAGESTATETYPE>(state),
                                &value)))
                            next.textureStage.push_back({ stage, state, value });
                    }
                }
                for (DWORD sampler = 0; sampler < 16; ++sampler)
                {
                    for (DWORD state = 1; state <= 16; ++state)
                    {
                        DWORD value = 0;
                        if (SUCCEEDED(device->GetSamplerState(sampler,
                                static_cast<D3DSAMPLERSTATETYPE>(state), &value)))
                            next.sampler.push_back({ sampler, state, value });
                    }
                }
                next.ready = !next.render.empty();
                std::lock_guard<std::mutex> lock(ClassicBaselineMutex);
                ClassicBaseline = std::move(next);
                return ClassicBaseline.ready;
            }
            catch (...)
            {
                return false;
            }
        }

        void ClearClassicBaseline() noexcept
        {
            std::lock_guard<std::mutex> lock(ClassicBaselineMutex);
            ClassicBaseline = {};
        }

        void UpdateCompatPresentationState(IDirect3DDevice9* device,
            const D3DPRESENT_PARAMETERS* params) noexcept
        {
            if (!device || !params) return;
            CompatWindowed.store(params->Windowed != FALSE,
                std::memory_order_release);
            HWND window = params->hDeviceWindow;
            if (!window)
            {
                D3DDEVICE_CREATION_PARAMETERS creation{};
                if (SUCCEEDED(device->GetCreationParameters(&creation)))
                    window = creation.hFocusWindow;
            }
            CompatFocusWindow.store(window, std::memory_order_release);
        }

        bool RestoreClassicResetState(IDirect3DDevice9* device) noexcept
        {
            if (!device) return false;
            std::uint32_t failures = 0;
            {
                std::lock_guard<std::mutex> lock(ClassicBaselineMutex);
                if (!ClassicBaseline.ready) return false;
                for (const auto& item : ClassicBaseline.render)
                    if (FAILED(device->SetRenderState(
                            static_cast<D3DRENDERSTATETYPE>(item.state),
                            item.value)))
                        ++failures;
                for (const auto& item : ClassicBaseline.textureStage)
                    if (FAILED(device->SetTextureStageState(item.stage,
                            static_cast<D3DTEXTURESTAGESTATETYPE>(item.state),
                            item.value)))
                        ++failures;
                for (const auto& item : ClassicBaseline.sampler)
                    if (FAILED(device->SetSamplerState(item.stage,
                            static_cast<D3DSAMPLERSTATETYPE>(item.state),
                            item.value)))
                        ++failures;
            }

            D3DCAPS9 caps{};
            const UINT streams = SUCCEEDED(device->GetDeviceCaps(&caps))
                ? std::min<UINT>(caps.MaxStreams, 16u) : 16u;
            // Classic D3D9 exposes eight fixed-function texture stages.
            // Do not count VS sampler aliases (16..19) or invalid pixel stages
            // as Reset replay failures.
            for (DWORD stage = 0; stage < 8; ++stage)
                if (FAILED(device->SetTexture(stage, nullptr))) ++failures;
            for (UINT stream = 0; stream < streams; ++stream)
            {
                if (FAILED(device->SetStreamSource(stream, nullptr, 0, 0)))
                    ++failures;
                if (FAILED(device->SetStreamSourceFreq(stream, 1)))
                    ++failures;
            }
            if (FAILED(device->SetIndices(nullptr))) ++failures;
            if (FAILED(device->SetVertexShader(nullptr))) ++failures;
            if (FAILED(device->SetPixelShader(nullptr))) ++failures;
            // A null declaration restores the fixed-function/default declaration
            // boundary expected after classic Reset. Some drivers reject it when
            // no declaration path exists, so treat that call as best-effort.
            device->SetVertexDeclaration(nullptr);

            IDirect3DSurface9* backBuffer = nullptr;
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(device->GetBackBuffer(0, 0,
                    D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer)
            {
                if (SUCCEEDED(backBuffer->GetDesc(&desc)) &&
                    desc.Width && desc.Height)
                {
                    D3DVIEWPORT9 viewport{};
                    viewport.Width = desc.Width;
                    viewport.Height = desc.Height;
                    viewport.MinZ = 0.0f;
                    viewport.MaxZ = 1.0f;
                    if (FAILED(device->SetViewport(&viewport))) ++failures;
                    RECT scissor{ 0, 0, static_cast<LONG>(desc.Width),
                        static_cast<LONG>(desc.Height) };
                    if (FAILED(device->SetScissorRect(&scissor))) ++failures;
                }
                backBuffer->Release();
            }

            if (failures == 0)
            {
                ++ClassicResetStateRestores;
                return true;
            }
            ++ClassicResetStateRestoreFailures;
            if (!FirstResetStateRestoreFailureLogged.exchange(true))
            {
                spdlog::warn(
                    "VR D3D9Ex compat: classic Reset state replay completed with {} rejected state writes; game/VR caches still re-prime from live state",
                    failures);
            }
            return false;
        }

        bool IsCompatDevice(IDirect3DDevice9* device) noexcept
        {
            return device && CompatDevice.load(std::memory_order_acquire) == device;
        }

        bool IsSystemModuleForAddress(const void* address, HMODULE& module) noexcept
        {
            module = nullptr;
            if (!address || !GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(address), &module) || !module)
                return false;

            wchar_t modulePath[MAX_PATH]{};
            wchar_t systemDir[MAX_PATH]{};
            const DWORD moduleLength = GetModuleFileNameW(module, modulePath, MAX_PATH);
            const UINT systemLength = GetSystemDirectoryW(systemDir, MAX_PATH);
            if (!moduleLength || moduleLength >= MAX_PATH || !systemLength || systemLength >= MAX_PATH)
                return false;

            const std::size_t dirLength = std::wcslen(systemDir);
            if (_wcsnicmp(modulePath, systemDir, dirLength) != 0)
                return false;
            const wchar_t separator = modulePath[dirLength];
            return separator == L'\\' || separator == L'/';
        }

        D3DDISPLAYMODEEX* BuildFullscreenMode(
            IDirect3DDevice9Ex* deviceEx,
            D3DPRESENT_PARAMETERS* params,
            D3DDISPLAYMODEEX& fullscreen) noexcept
        {
            if (!params || params->Windowed)
                return nullptr;

            fullscreen = {};
            fullscreen.Size = sizeof(fullscreen);
            fullscreen.Width = params->BackBufferWidth;
            fullscreen.Height = params->BackBufferHeight;
            fullscreen.RefreshRate = params->FullScreen_RefreshRateInHz;
            fullscreen.Format = params->BackBufferFormat;
            fullscreen.ScanLineOrdering = D3DSCANLINEORDERING_UNKNOWN;

            if (deviceEx)
            {
                D3DDEVICE_CREATION_PARAMETERS creation{};
                D3DDISPLAYROTATION rotation = D3DDISPLAYROTATION_IDENTITY;
                D3DDISPLAYMODEEX current{};
                current.Size = sizeof(current);
                if (SUCCEEDED(deviceEx->GetCreationParameters(&creation)) &&
                    SUCCEEDED(deviceEx->GetDisplayModeEx(0, &current, &rotation)))
                {
                    if (!fullscreen.Width) fullscreen.Width = current.Width;
                    if (!fullscreen.Height) fullscreen.Height = current.Height;
                    if (fullscreen.Format == D3DFMT_UNKNOWN) fullscreen.Format = current.Format;
                    if (!fullscreen.RefreshRate) fullscreen.RefreshRate = current.RefreshRate;
                    fullscreen.ScanLineOrdering = current.ScanLineOrdering;
                }
            }
            if (fullscreen.ScanLineOrdering == D3DSCANLINEORDERING_UNKNOWN)
                fullscreen.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            return &fullscreen;
        }

        HRESULT __stdcall TestCooperativeLevelCompatDest(
            IDirect3DDevice9* device)
        {
            if (!IsCompatDevice(device))
                return TestCooperativeLevelCompatHook.stdcall<HRESULT>(device);

            IDirect3DDevice9Ex* deviceEx = nullptr;
            if (FAILED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
                    reinterpret_cast<void**>(&deviceEx))) || !deviceEx)
                return TestCooperativeLevelCompatHook.stdcall<HRESULT>(device);

            HWND window = CompatFocusWindow.load(std::memory_order_acquire);
            if (!window) window = GetDesktopWindow();
            const HRESULT state = deviceEx->CheckDeviceState(window);
            deviceEx->Release();
            ++CooperativeLevelTranslations;

            HRESULT translated = D3D_OK;
            if (state == S_PRESENT_MODE_CHANGED)
                translated = D3DERR_DEVICENOTRESET;
            else if (state == S_PRESENT_OCCLUDED)
                translated = CompatWindowed.load(std::memory_order_acquire)
                    ? D3D_OK : D3DERR_DEVICELOST;
            else if (state == D3DERR_DEVICELOST)
                translated = D3DERR_DEVICELOST;
            else if (state == D3DERR_DEVICEHUNG ||
                state == D3DERR_DEVICEREMOVED)
                translated = D3DERR_DRIVERINTERNALERROR;
            else if (FAILED(state))
                translated = D3DERR_DRIVERINTERNALERROR;

            if (!FirstCooperativeTranslationLogged.exchange(true))
                spdlog::info(
                    "VR D3D9Ex compat: TestCooperativeLevel is translated from CheckDeviceState for legacy lost-device recovery");
            return translated;
        }

        HRESULT __stdcall EvictManagedResourcesCompatDest(
            IDirect3DDevice9* device)
        {
            if (IsCompatDevice(device))
                return D3D_OK;
            return EvictManagedResourcesCompatHook.stdcall<HRESULT>(device);
        }

        HRESULT __stdcall ResetCompatDest(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
        {
            if (!IsCompatDevice(device) || !params)
                return ResetCompatHook.stdcall<HRESULT>(device, params);

            IDirect3DDevice9Ex* deviceEx = nullptr;
            const HRESULT qi = device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&deviceEx));
            if (FAILED(qi) || !deviceEx)
                return ResetCompatHook.stdcall<HRESULT>(device, params);

            D3DDISPLAYMODEEX fullscreen{};
            D3DDISPLAYMODEEX* fullscreenPtr = BuildFullscreenMode(deviceEx, params, fullscreen);
            const HRESULT hr = deviceEx->ResetEx(params, fullscreenPtr);
            deviceEx->Release();
            if (SUCCEEDED(hr))
            {
                UpdateCompatPresentationState(device, params);
                RestoreClassicResetState(device);
            }
            ++ResetExRedirects;
            spdlog::info(
                "VR D3D9Ex compat: IDirect3DDevice9::Reset redirected to ResetEx hr=0x{:08X}; emulated MANAGED resources remain persistent",
                static_cast<unsigned>(hr));
            return hr;
        }

        HRESULT __stdcall CreateVertexBufferCompatDest(
            IDirect3DDevice9* device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
            IDirect3DVertexBuffer9** buffer, HANDLE* sharedHandle)
        {
            if (!IsCompatDevice(device) || pool != D3DPOOL_MANAGED)
                return CreateVertexBufferCompatHook.stdcall<HRESULT>(
                    device, length, usage, fvf, pool, buffer, sharedHandle);

            const HRESULT hr = CreateVertexBufferCompatHook.stdcall<HRESULT>(
                device, length, usage, fvf, D3DPOOL_DEFAULT, buffer, sharedHandle);
            ++ManagedVertexBufferCreates;
            if (FAILED(hr)) ++ManagedCreateFailures;
            if (ManagedVertexBufferCreates.load() == 1)
            {
                spdlog::info(
                    "VR D3D9Ex compat: MANAGED vertex buffer translated to DEFAULT (length={} usage=0x{:08X} hr=0x{:08X})",
                    length, static_cast<unsigned>(usage), static_cast<unsigned>(hr));
            }
            return hr;
        }

        HRESULT __stdcall CreateIndexBufferCompatDest(
            IDirect3DDevice9* device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
            IDirect3DIndexBuffer9** buffer, HANDLE* sharedHandle)
        {
            if (!IsCompatDevice(device) || pool != D3DPOOL_MANAGED)
                return CreateIndexBufferCompatHook.stdcall<HRESULT>(
                    device, length, usage, format, pool, buffer, sharedHandle);

            const HRESULT hr = CreateIndexBufferCompatHook.stdcall<HRESULT>(
                device, length, usage, format, D3DPOOL_DEFAULT, buffer, sharedHandle);
            ++ManagedIndexBufferCreates;
            if (FAILED(hr)) ++ManagedCreateFailures;
            if (ManagedIndexBufferCreates.load() == 1)
            {
                spdlog::info(
                    "VR D3D9Ex compat: MANAGED index buffer translated to DEFAULT (length={} usage=0x{:08X} fmt={} hr=0x{:08X})",
                    length, static_cast<unsigned>(usage), static_cast<int>(format), static_cast<unsigned>(hr));
            }
            return hr;
        }

        template <typename CreateFn>
        HRESULT CreateManagedTextureCompat(
            const char* label,
            std::atomic<std::uint64_t>& counter,
            DWORD usage,
            bool allowNonDynamicFallback,
            CreateFn&& create)
        {
            const DWORD dynamicUsage = usage | D3DUSAGE_DYNAMIC;
            HRESULT hr = create(dynamicUsage, D3DPOOL_DEFAULT);
            ++counter;
            if (SUCCEEDED(hr))
            {
                if (counter.load() == 1)
                {
                    spdlog::info(
                        "VR D3D9Ex compat: MANAGED {} translated to lockable DEFAULT|DYNAMIC (usage 0x{:08X}->0x{:08X})",
                        label, static_cast<unsigned>(usage), static_cast<unsigned>(dynamicUsage));
                }
                return hr;
            }

            // R14 provides an independent CPU shadow for 2D textures, so those
            // resources can safely fall back to non-dynamic DEFAULT. Cube/volume
            // textures do not yet have that shadow contract: fail creation rather
            // than return an object whose later Lock* semantics silently differ
            // from legacy MANAGED.
            if (!allowNonDynamicFallback)
            {
                ++ManagedCreateFailures;
                spdlog::warn(
                    "VR D3D9Ex compat: MANAGED {} requires a non-dynamic fallback that cannot preserve Lock semantics; failing closed hr=0x{:08X}",
                    label, static_cast<unsigned>(hr));
                return hr;
            }
            ++ManagedTextureDynamicFallbacks;
            hr = create(usage, D3DPOOL_DEFAULT);
            if (FAILED(hr)) ++ManagedCreateFailures;
            spdlog::warn(
                "VR D3D9Ex compat: {} DYNAMIC retry path used; final hr=0x{:08X} usage=0x{:08X}. A later Lock* on a non-dynamic DEFAULT texture may require shadow emulation.",
                label, static_cast<unsigned>(hr), static_cast<unsigned>(usage));
            return hr;
        }

        HRESULT __stdcall CreateTextureCompatDest(
            IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage,
            D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** texture, HANDLE* sharedHandle)
        {
            if (!IsCompatDevice(device) || pool != D3DPOOL_MANAGED)
                return CreateTextureCompatHook.stdcall<HRESULT>(
                    device, width, height, levels, usage, format, pool, texture, sharedHandle);

            return CreateManagedTextureCompat("texture", ManagedTextureCreates, usage, true,
                [&](DWORD translatedUsage, D3DPOOL translatedPool)
                {
                    return CreateTextureCompatHook.stdcall<HRESULT>(
                        device, width, height, levels, translatedUsage, format,
                        translatedPool, texture, sharedHandle);
                });
        }

        HRESULT __stdcall CreateVolumeTextureCompatDest(
            IDirect3DDevice9* device, UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
            D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9** texture, HANDLE* sharedHandle)
        {
            if (!IsCompatDevice(device) || pool != D3DPOOL_MANAGED)
                return CreateVolumeTextureCompatHook.stdcall<HRESULT>(
                    device, width, height, depth, levels, usage, format, pool, texture, sharedHandle);

            return CreateManagedTextureCompat("volume texture", ManagedVolumeTextureCreates, usage, false,
                [&](DWORD translatedUsage, D3DPOOL translatedPool)
                {
                    return CreateVolumeTextureCompatHook.stdcall<HRESULT>(
                        device, width, height, depth, levels, translatedUsage, format,
                        translatedPool, texture, sharedHandle);
                });
        }

        HRESULT __stdcall CreateCubeTextureCompatDest(
            IDirect3DDevice9* device, UINT edgeLength, UINT levels, DWORD usage,
            D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9** texture, HANDLE* sharedHandle)
        {
            if (!IsCompatDevice(device) || pool != D3DPOOL_MANAGED)
                return CreateCubeTextureCompatHook.stdcall<HRESULT>(
                    device, edgeLength, levels, usage, format, pool, texture, sharedHandle);

            return CreateManagedTextureCompat("cube texture", ManagedCubeTextureCreates, usage, false,
                [&](DWORD translatedUsage, D3DPOOL translatedPool)
                {
                    return CreateCubeTextureCompatHook.stdcall<HRESULT>(
                        device, edgeLength, levels, translatedUsage, format,
                        translatedPool, texture, sharedHandle);
                });
        }

        void ClearCompatHooks() noexcept
        {
            CompatDevice.store(nullptr, std::memory_order_release);
            CompatFocusWindow.store(nullptr, std::memory_order_release);
            TestCooperativeLevelCompatHook = {};
            EvictManagedResourcesCompatHook = {};
            ResetCompatHook = {};
            CreateTextureCompatHook = {};
            CreateVolumeTextureCompatHook = {};
            CreateCubeTextureCompatHook = {};
            CreateVertexBufferCompatHook = {};
            CreateIndexBufferCompatHook = {};
            ClearClassicBaseline();
        }

        bool InstallManagedResourceCompat(IDirect3DDevice9Ex* deviceEx)
        {
            if (!deviceEx)
                return false;

            auto* baseDevice = static_cast<IDirect3DDevice9*>(deviceEx);
            IDirect3DDevice9* const existing =
                CompatDevice.load(std::memory_order_acquire);
            if (existing && existing != baseDevice)
            {
                spdlog::warn(
                    "VR D3D9Ex compat: a second promoted game device was requested; keeping the first compatibility owner and forcing the new device back to classic D3D9");
                return false;
            }
            if (!CaptureClassicBaseline(baseDevice))
            {
                spdlog::error(
                    "VR D3D9Ex compat: could not capture the fresh-device classic state baseline; rejecting Ex promotion");
                return false;
            }
            void** vtable = *reinterpret_cast<void***>(baseDevice);
            if (!vtable)
                return false;

            const auto disabled = safetyhook::InlineHook::StartDisabled;
            TestCooperativeLevelCompatHook = safetyhook::create_inline(
                vtable[TestCooperativeLevelVtableIndex],
                TestCooperativeLevelCompatDest, disabled);
            EvictManagedResourcesCompatHook = safetyhook::create_inline(
                vtable[EvictManagedResourcesVtableIndex],
                EvictManagedResourcesCompatDest, disabled);
            ResetCompatHook = safetyhook::create_inline(
                vtable[ResetVtableIndex], ResetCompatDest, disabled);
            CreateTextureCompatHook = safetyhook::create_inline(vtable[CreateTextureVtableIndex], CreateTextureCompatDest, disabled);
            CreateVolumeTextureCompatHook = safetyhook::create_inline(vtable[CreateVolumeTextureVtableIndex], CreateVolumeTextureCompatDest, disabled);
            CreateCubeTextureCompatHook = safetyhook::create_inline(vtable[CreateCubeTextureVtableIndex], CreateCubeTextureCompatDest, disabled);
            CreateVertexBufferCompatHook = safetyhook::create_inline(vtable[CreateVertexBufferVtableIndex], CreateVertexBufferCompatDest, disabled);
            CreateIndexBufferCompatHook = safetyhook::create_inline(vtable[CreateIndexBufferVtableIndex], CreateIndexBufferCompatDest, disabled);

            SafetyHookInline* hooks[]{
                &TestCooperativeLevelCompatHook, &EvictManagedResourcesCompatHook,
                &ResetCompatHook, &CreateTextureCompatHook,
                &CreateVolumeTextureCompatHook, &CreateCubeTextureCompatHook,
                &CreateVertexBufferCompatHook, &CreateIndexBufferCompatHook
            };
            for (auto* hook : hooks)
            {
                if (!*hook || !hook->enable().has_value())
                {
                    spdlog::error(
                        "VR D3D9Ex compat: compatibility hook transaction was partial; rejecting Ex device");
                    ClearCompatHooks();
                    return false;
                }
            }

            CompatDevice.store(baseDevice, std::memory_order_release);
            spdlog::info(
                "VR D3D9Ex compat: legacy contract hooks installed (TestCooperativeLevel/EvictManaged/Reset/resources); fresh-device state baseline captured; MANAGED 2D -> R14 shadow, cube/volume fail closed if DYNAMIC is unavailable");
            return true;
        }

        class Direct3D9ExCompat final : public IDirect3D9
        {
        public:
            Direct3D9ExCompat(IDirect3D9Ex* ex, IDirect3D9* fallback) noexcept
                : ex_(ex), fallback_(fallback)
            {
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
            {
                if (!object) return E_POINTER;
                *object = nullptr;
                if (riid == IID_IUnknown || riid == IID_IDirect3D9)
                {
                    *object = static_cast<IDirect3D9*>(this);
                    AddRef();
                    return S_OK;
                }
                if (riid == __uuidof(IDirect3D9Ex) && ex_)
                    return ex_->QueryInterface(riid, object);
                return fallback_ ? fallback_->QueryInterface(riid, object) : E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override
            {
                return static_cast<ULONG>(InterlockedIncrement(&refs_));
            }

            ULONG STDMETHODCALLTYPE Release() override
            {
                const LONG refs = InterlockedDecrement(&refs_);
                if (refs == 0)
                {
                    if (fallback_) fallback_->Release();
                    if (ex_) ex_->Release();
                    delete this;
                    return 0;
                }
                return static_cast<ULONG>(refs);
            }

            HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* initializeFunction) override
            {
                return fallback_->RegisterSoftwareDevice(initializeFunction);
            }
            UINT STDMETHODCALLTYPE GetAdapterCount() override { return fallback_->GetAdapterCount(); }
            HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* i) override
            { return fallback_->GetAdapterIdentifier(a, f, i); }
            UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT f) override
            { return fallback_->GetAdapterModeCount(a, f); }
            HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* mode) override
            { return fallback_->EnumAdapterModes(a, f, m, mode); }
            HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* mode) override
            { return fallback_->GetAdapterDisplayMode(a, mode); }
            HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT bf, BOOL w) override
            { return fallback_->CheckDeviceType(a, t, af, bf, w); }
            HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE r, D3DFORMAT cf) override
            { return fallback_->CheckDeviceFormat(a, t, af, u, r, cf); }
            HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE t, D3DFORMAT sf, BOOL w,
                D3DMULTISAMPLE_TYPE mt, DWORD* q) override
            { return fallback_->CheckDeviceMultiSampleType(a, t, sf, w, mt, q); }
            HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE t, D3DFORMAT af,
                D3DFORMAT rf, D3DFORMAT df) override
            { return fallback_->CheckDepthStencilMatch(a, t, af, rf, df); }
            HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d) override
            { return fallback_->CheckDeviceFormatConversion(a, t, s, d); }
            HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9* caps) override
            { return fallback_->GetDeviceCaps(a, t, caps); }
            HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override
            { return fallback_->GetAdapterMonitor(a); }

            HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow,
                DWORD behaviorFlags, D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** device) override
            {
                if (!device || !params) return D3DERR_INVALIDCALL;
                *device = nullptr;

                // D3DPRESENT_PARAMETERS is an in/out contract. Preserve the
                // game's original request so a failed Ex attempt cannot poison
                // the classic fallback with mutated presentation values.
                const D3DPRESENT_PARAMETERS originalParams = *params;
                auto classicFallback = [&]() -> HRESULT
                {
                    *params = originalParams;
                    return fallback_->CreateDevice(adapter, type, focusWindow,
                        behaviorFlags, params, device);
                };

                // A wrapper may outlive a failed first Ex promotion attempt.
                // Re-check the final overlay for every CreateDevice call so a
                // later retry cannot re-enter a compatibility stack that R15
                // deliberately disarmed after a transactional failure.
                if (!FinalCompatOverlayReady.load(std::memory_order_acquire))
                {
                    if (!FirstOverlayUnavailableLogged.exchange(true))
                        spdlog::warn(
                            "VR D3D9Ex startup: final R15 compatibility overlay became unavailable after wrapper creation; subsequent CreateDevice stays on classic D3D9");
                    return classicFallback();
                }

                if (!FirstCreateFlagsLogged.exchange(true))
                {
                    spdlog::info(
                        "VR D3D9Ex startup: CreateDevice flags=0x{:08X} multithreaded={} pureDevice={}",
                        static_cast<unsigned>(behaviorFlags),
                        (behaviorFlags & D3DCREATE_MULTITHREADED) != 0,
                        (behaviorFlags & D3DCREATE_PUREDEVICE) != 0);
                }
                if ((behaviorFlags & D3DCREATE_PUREDEVICE) != 0)
                {
                    if (!FirstPureDeviceFallbackLogged.exchange(true))
                        spdlog::warn(
                            "VR D3D9Ex startup: D3DCREATE_PUREDEVICE detected; Ex promotion skipped and classic D3D9 retained");
                    return classicFallback();
                }

                D3DDISPLAYMODEEX fullscreen{};
                D3DDISPLAYMODEEX* fullscreenPtr = nullptr;
                if (!params->Windowed)
                {
                    fullscreen.Size = sizeof(fullscreen);
                    fullscreen.Width = params->BackBufferWidth;
                    fullscreen.Height = params->BackBufferHeight;
                    fullscreen.RefreshRate = params->FullScreen_RefreshRateInHz;
                    fullscreen.Format = params->BackBufferFormat;
                    fullscreen.ScanLineOrdering = D3DSCANLINEORDERING_UNKNOWN;
                    if (ex_)
                    {
                        D3DDISPLAYROTATION rotation = D3DDISPLAYROTATION_IDENTITY;
                        D3DDISPLAYMODEEX current{};
                        current.Size = sizeof(current);
                        if (SUCCEEDED(ex_->GetAdapterDisplayModeEx(adapter, &current, &rotation)))
                        {
                            if (!fullscreen.Width) fullscreen.Width = current.Width;
                            if (!fullscreen.Height) fullscreen.Height = current.Height;
                            if (fullscreen.Format == D3DFMT_UNKNOWN) fullscreen.Format = current.Format;
                            if (!fullscreen.RefreshRate) fullscreen.RefreshRate = current.RefreshRate;
                            fullscreen.ScanLineOrdering = current.ScanLineOrdering;
                        }
                    }
                    if (fullscreen.ScanLineOrdering == D3DSCANLINEORDERING_UNKNOWN)
                        fullscreen.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
                    fullscreenPtr = &fullscreen;
                }

                IDirect3DDevice9Ex* deviceEx = nullptr;
                HRESULT hr = ex_ ? ex_->CreateDeviceEx(adapter, type, focusWindow, behaviorFlags,
                    params, fullscreenPtr, &deviceEx) : E_FAIL;
                if (SUCCEEDED(hr) && deviceEx)
                {
                    if (!InstallManagedResourceCompat(deviceEx))
                    {
                        deviceEx->Release();
                        deviceEx = nullptr;
                        if (!FirstFallbackLogged.exchange(true))
                        {
                            spdlog::warn(
                                "VR D3D9Ex upgrade: Ex device created but managed-resource compatibility layer could not be installed; falling back to original D3D9 device");
                        }
                        return classicFallback();
                    }

                    *device = static_cast<IDirect3DDevice9*>(deviceEx);
                    UpdateCompatPresentationState(*device, params);
                    if (!FirstUpgradeLogged.exchange(true))
                    {
                        spdlog::info(
                            "VR D3D9Ex upgrade: game CreateDevice promoted to CreateDeviceEx with managed-resource compatibility; existing 4-slot zero-copy eye transport is eligible");
                    }
                    return hr;
                }

                if (!FirstFallbackLogged.exchange(true))
                {
                    spdlog::warn(
                        "VR D3D9Ex upgrade: CreateDeviceEx failed HRESULT=0x{:08X}; falling back to original IDirect3D9::CreateDevice",
                        static_cast<unsigned>(hr));
                }
                return classicFallback();
            }

        private:
            volatile LONG refs_ = 1;
            IDirect3D9Ex* ex_ = nullptr;
            IDirect3D9* fallback_ = nullptr;
        };

        IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion)
        {
            if (!OriginalDirect3DCreate9)
                return nullptr;

            IDirect3D9* fallback = OriginalDirect3DCreate9(sdkVersion);
            if (!fallback || !Settings::VRPreferD3D9Ex)
                return fallback;
            if (!FinalCompatOverlayReady.load(std::memory_order_acquire))
            {
                if (!FirstOverlayUnavailableLogged.exchange(true))
                    spdlog::error(
                        "VR D3D9Ex startup: final R15 compatibility overlay is not ready; Ex promotion skipped to preserve classic startup");
                return fallback;
            }

            HMODULE provider = nullptr;
            if (!IsSystemModuleForAddress(reinterpret_cast<const void*>(OriginalDirect3DCreate9), provider))
            {
                if (!ThirdPartyLogged.exchange(true))
                    spdlog::warn("VR D3D9Ex upgrade: third-party d3d9 provider detected; device upgrade skipped to preserve wrapper compatibility");
                return fallback;
            }

            const auto createEx = reinterpret_cast<Direct3DCreate9ExFn>(
                GetProcAddress(provider, "Direct3DCreate9Ex"));
            if (!createEx)
                return fallback;

            IDirect3D9Ex* ex = nullptr;
            const HRESULT hr = createEx(sdkVersion, &ex);
            if (FAILED(hr) || !ex)
                return fallback;

            auto* wrapper = new (std::nothrow) Direct3D9ExCompat(ex, fallback);
            if (!wrapper)
            {
                ex->Release();
                return fallback;
            }
            return wrapper;
        }

        bool PatchDirect3DCreate9Import() noexcept
        {
            auto* base = reinterpret_cast<std::uint8_t*>(Module::ExeHandle);
            if (!base) return false;
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
            const auto& importDirectory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!importDirectory.VirtualAddress || !importDirectory.Size) return false;

            auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDirectory.VirtualAddress);
            for (; descriptor->Name; ++descriptor)
            {
                const char* moduleName = reinterpret_cast<const char*>(base + descriptor->Name);
                if (_stricmp(moduleName, "d3d9.dll") != 0) continue;
                if (!descriptor->FirstThunk || !descriptor->OriginalFirstThunk) return false;

                auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
                auto* thunks = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++thunks)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                    auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(import->Name), "Direct3DCreate9") != 0)
                        continue;

                    OriginalDirect3DCreate9 = reinterpret_cast<Direct3DCreate9Fn>(thunks->u1.Function);
                    DWORD oldProtect = 0;
                    if (!VirtualProtect(&thunks->u1.Function, sizeof(thunks->u1.Function), PAGE_READWRITE, &oldProtect))
                        return false;
                    thunks->u1.Function = reinterpret_cast<ULONG_PTR>(&HookedDirect3DCreate9);
                    FlushInstructionCache(GetCurrentProcess(), &thunks->u1.Function, sizeof(thunks->u1.Function));
                    DWORD ignored = 0;
                    VirtualProtect(&thunks->u1.Function, sizeof(thunks->u1.Function), oldProtect, &ignored);
                    return true;
                }
            }
            return false;
        }
    }

    void SetFinalCompatOverlayReady(bool ready) noexcept
    {
        FinalCompatOverlayReady.store(ready, std::memory_order_release);
    }

    class D3D9ExUpgradeHook final : public Hook
    {
    public:
        std::string_view description() override { return "OpenXRVRD3D9ExUpgrade"; }
        void declare_settings() override { Settings::VRPreferD3D9Ex.needs_restart(); }
        bool validate() override { return Settings::VRPreferD3D9Ex; }
        bool apply() override
        {
            if (!PatchDirect3DCreate9Import())
            {
                spdlog::warn("VR D3D9Ex upgrade: Direct3DCreate9 IAT entry was not found/patched; original D3D9 path remains active");
                return true;
            }
            spdlog::info("VR D3D9Ex upgrade: Direct3DCreate9 IAT hook armed before game device creation");
            return true;
        }

        static D3D9ExUpgradeHook instance;
    };

    D3D9ExUpgradeHook D3D9ExUpgradeHook::instance;
}