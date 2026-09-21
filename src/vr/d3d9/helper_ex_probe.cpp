#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cstdint>
#include <filesystem>

#include <spdlog/spdlog.h>

#include "hook_mgr.hpp"
#include "game_addrs.hpp"
#include "vr/ipc/protocol.hpp"

namespace OutRunVRHelperExProbe
{
    namespace
    {
        using Direct3DCreate9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);

        template <typename T>
        void ReleaseCom(T*& p) noexcept
        {
            if (p) { p->Release(); p = nullptr; }
        }

        bool WaitForQuery(IDirect3DQuery9* query, DWORD timeoutMs) noexcept
        {
            if (!query) return false;
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            for (;;)
            {
                const HRESULT hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
                if (hr == S_OK) return true;
                if (hr != S_FALSE || GetTickCount64() >= deadline) return false;
                Sleep(0);
            }
        }

        bool GameDeviceIsEx(IDirect3DDevice9* device) noexcept
        {
            IDirect3DDevice9Ex* ex = nullptr;
            const bool result = device && SUCCEEDED(device->QueryInterface(
                __uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex))) && ex;
            ReleaseCom(ex);
            return result;
        }

        struct SharedPoseView
        {
            HANDLE mapping = nullptr;
            OutRunVR::SharedPoseState* state = nullptr;
            ~SharedPoseView()
            {
                if (state) UnmapViewOfFile(state);
                if (mapping) CloseHandle(mapping);
            }
            bool Open() noexcept
            {
                if (state) return true;
                mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, OutRunVR::SharedMemoryName);
                if (!mapping) return false;
                state = static_cast<OutRunVR::SharedPoseState*>(MapViewOfFile(
                    mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OutRunVR::SharedPoseState)));
                if (!state)
                {
                    CloseHandle(mapping);
                    mapping = nullptr;
                    return false;
                }
                return true;
            }
            bool Valid() const noexcept
            {
                return state && state->magic == OutRunVR::SharedMagic &&
                    state->protocolVersion == OutRunVR::SharedProtocolVersion &&
                    state->structSize == sizeof(OutRunVR::SharedPoseState);
            }
        };

        HMODULE LoadSystemD3D9()
        {
            wchar_t systemDir[MAX_PATH]{};
            const UINT length = GetSystemDirectoryW(systemDir, MAX_PATH);
            if (!length || length >= MAX_PATH) return nullptr;
            std::filesystem::path path(systemDir);
            path /= L"d3d9.dll";
            return LoadLibraryW(path.c_str());
        }

        DWORD WINAPI ProbeThread(void*)
        {
            IDirect3DDevice9* gameDevice = nullptr;
            for (int i = 0; i < 1200; ++i)
            {
                gameDevice = Game::D3DDevice_ptr ? *Game::D3DDevice_ptr : nullptr;
                if (gameDevice) break;
                Sleep(100);
            }
            if (!gameDevice)
                return 0;
            if (GameDeviceIsEx(gameDevice))
            {
                spdlog::info("VR helper D3D9Ex probe: skipped because game device already supports native D3D9Ex zero-copy transport");
                return 0;
            }

            SharedPoseView shared;
            for (int i = 0; i < 200; ++i)
            {
                if (shared.Open() && shared.Valid() && shared.state->hostPid &&
                    (shared.state->flags & OutRunVR::HostAdapterLuidValid))
                    break;
                Sleep(50);
            }
            if (!shared.Valid() || !shared.state->hostPid ||
                (shared.state->flags & OutRunVR::HostAdapterLuidValid) == 0)
            {
                spdlog::warn("VR helper D3D9Ex probe: host adapter LUID was not available; helper bridge feasibility unknown");
                return 0;
            }

            HMODULE d3d9Module = LoadSystemD3D9();
            if (!d3d9Module)
            {
                spdlog::warn("VR helper D3D9Ex probe: system d3d9.dll could not be loaded");
                return 0;
            }
            const auto create9Ex = reinterpret_cast<Direct3DCreate9ExFn>(
                GetProcAddress(d3d9Module, "Direct3DCreate9Ex"));
            if (!create9Ex)
            {
                spdlog::warn("VR helper D3D9Ex probe: Direct3DCreate9Ex export unavailable");
                FreeLibrary(d3d9Module);
                return 0;
            }

            IDirect3D9Ex* d3dEx = nullptr;
            if (FAILED(create9Ex(D3D_SDK_VERSION, &d3dEx)) || !d3dEx)
            {
                spdlog::warn("VR helper D3D9Ex probe: Direct3DCreate9Ex failed");
                FreeLibrary(d3d9Module);
                return 0;
            }

            const LUID wanted{
                shared.state->hostAdapterLuidLow,
                static_cast<LONG>(shared.state->hostAdapterLuidHigh)
            };
            UINT adapter = D3DADAPTER_DEFAULT;
            bool found = false;
            const UINT adapterCount = d3dEx->GetAdapterCount();
            for (UINT i = 0; i < adapterCount; ++i)
            {
                LUID candidate{};
                if (SUCCEEDED(d3dEx->GetAdapterLUID(i, &candidate)) &&
                    candidate.LowPart == wanted.LowPart && candidate.HighPart == wanted.HighPart)
                {
                    adapter = i;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                spdlog::warn("VR helper D3D9Ex probe: no D3D9Ex adapter matched OpenXR LUID {:08X}:{:08X}",
                    static_cast<std::uint32_t>(wanted.HighPart), wanted.LowPart);
                ReleaseCom(d3dEx);
                FreeLibrary(d3d9Module);
                return 0;
            }

            D3DDEVICE_CREATION_PARAMETERS gameCreation{};
            gameDevice->GetCreationParameters(&gameCreation);
            HWND window = gameCreation.hFocusWindow ? gameCreation.hFocusWindow : GetDesktopWindow();
            D3DPRESENT_PARAMETERS pp{};
            pp.Windowed = TRUE;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.hDeviceWindow = window;
            pp.BackBufferWidth = 1;
            pp.BackBufferHeight = 1;
            pp.BackBufferFormat = D3DFMT_A8R8G8B8;
            pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

            IDirect3DDevice9Ex* helper = nullptr;
            HRESULT deviceHr = d3dEx->CreateDeviceEx(
                adapter, D3DDEVTYPE_HAL, window,
                D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
                &pp, nullptr, &helper);
            if (FAILED(deviceHr) || !helper)
            {
                deviceHr = d3dEx->CreateDeviceEx(
                    adapter, D3DDEVTYPE_HAL, window,
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
                    &pp, nullptr, &helper);
            }
            if (FAILED(deviceHr) || !helper)
            {
                spdlog::warn("VR helper D3D9Ex probe: helper CreateDeviceEx failed HRESULT=0x{:08X}",
                    static_cast<unsigned>(deviceHr));
                ReleaseCom(d3dEx);
                FreeLibrary(d3d9Module);
                return 0;
            }

            IDirect3DTexture9* texture = nullptr;
            IDirect3DSurface9* surface = nullptr;
            IDirect3DQuery9* fence = nullptr;
            HANDLE sharedHandle = nullptr;
            HRESULT hr = helper->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_A8B8G8R8, D3DPOOL_DEFAULT, &texture, &sharedHandle);
            if (SUCCEEDED(hr) && texture)
                hr = texture->GetSurfaceLevel(0, &surface);
            if (SUCCEEDED(hr) && surface)
                hr = helper->ColorFill(surface, nullptr, D3DCOLOR_ARGB(0xFF, 0x7B, 0x7B, 0x7B));
            if (SUCCEEDED(hr))
                hr = helper->CreateQuery(D3DQUERYTYPE_EVENT, &fence);
            if (SUCCEEDED(hr) && fence)
                hr = fence->Issue(D3DISSUE_END);
            if (FAILED(hr) || !sharedHandle || !WaitForQuery(fence, 100))
            {
                spdlog::warn("VR helper D3D9Ex probe: shared verification texture creation/upload failed HRESULT=0x{:08X}",
                    static_cast<unsigned>(hr));
                ReleaseCom(fence); ReleaseCom(surface); ReleaseCom(texture);
                ReleaseCom(helper); ReleaseCom(d3dEx); FreeLibrary(d3d9Module);
                return 0;
            }

            const std::uint32_t token =
                (static_cast<std::uint32_t>(GetTickCount()) ^ GetCurrentProcessId() ^ 0x48585042u) | 1u;
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared.state->clientAdapterLuidLow),
                static_cast<LONG>(wanted.LowPart));
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared.state->clientAdapterLuidHigh),
                static_cast<LONG>(wanted.HighPart));
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared.state->hostInteropProbeAckToken), 0);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared.state->clientInteropProbeHandle),
                static_cast<LONG>(reinterpret_cast<std::uintptr_t>(sharedHandle)));
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared.state->clientInteropProbeToken),
                static_cast<LONG>(token));

            bool verified = false;
            for (int i = 0; i < 200; ++i)
            {
                if (shared.state->hostInteropProbeAckToken == token)
                {
                    verified = true;
                    break;
                }
                Sleep(10);
            }

            if (verified)
            {
                spdlog::info(
                    "VR helper D3D9Ex probe: VERIFIED helper D3D9Ex -> D3D11 sharing on OpenXR adapter; FEAR-style CPU exact-eye bridge is supported on this system");
            }
            else
            {
                spdlog::warn(
                    "VR helper D3D9Ex probe: host did not acknowledge helper shared texture within 2s; SBS/Desktop Duplication remains the safe fallback");
            }

            // Keep the verified handle fields only long enough for the host to inspect them.
            // A plain-D3D9 game cannot use this texture as its direct frame source yet.
            InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared.state->clientInteropProbeHandle),
                0, static_cast<LONG>(reinterpret_cast<std::uintptr_t>(sharedHandle)));
            InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared.state->clientInteropProbeToken),
                0, static_cast<LONG>(token));

            ReleaseCom(fence);
            ReleaseCom(surface);
            ReleaseCom(texture);
            ReleaseCom(helper);
            ReleaseCom(d3dEx);
            FreeLibrary(d3d9Module);
            return 0;
        }
    }

    class HelperExProbeHook final : public Hook
    {
    public:
        std::string_view description() override { return "OpenXRVRHelperD3D9ExProbe"; }
        bool validate() override { return true; }
        bool apply() override
        {
            HANDLE thread = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
            if (!thread)
            {
                spdlog::warn("VR helper D3D9Ex probe: failed to start probe thread: {}", GetLastError());
                return false;
            }
            CloseHandle(thread);
            return true;
        }
        static HelperExProbeHook instance;
    };

    HelperExProbeHook HelperExProbeHook::instance;
}
