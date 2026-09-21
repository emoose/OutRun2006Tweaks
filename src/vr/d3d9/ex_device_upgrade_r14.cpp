// R14 D3D9Ex managed-texture compatibility overlay.
//
// D3D9Ex has no D3DPOOL_MANAGED. The R13 compatibility layer translates legacy
// MANAGED 2D textures to DEFAULT|DYNAMIC, but a few drivers/resources still
// reject LockRect on those translated GPU textures. R14 keeps a SYSTEMMEM CPU
// shadow for every successfully translated MANAGED 2D texture and services the
// game's LockRect/UnlockRect against that shadow.
//
// R31 hardening removes the old 256-entry overwrite ring. Entries now live until
// the tracked texture's final COM Release, and a shared entry keeps an in-flight
// Lock/Unlock operation stable while the registry is changed. Borrowing a
// level surface is allowed while its mutating entry points are observed; actual
// external writes (surface LockRect/GetDC, StretchRect, ColorFill, Update*, or
// mip generation) retire the shadow instead of risking a stale CPU overwrite.

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ex_device_upgrade_r13.cpp"

namespace OutRunVRD3D9ExUpgradeR13
{
    namespace
    {
        constexpr std::uint32_t R14MaxTrackedLevels = 32;
        constexpr std::size_t R14TextureReleaseVtableIndex = 2;
        constexpr std::size_t R14TextureGenerateMipSubLevelsVtableIndex = 16;
        constexpr std::size_t R14TextureGetSurfaceLevelVtableIndex = 18;
        constexpr std::size_t R14TextureAddDirtyRectVtableIndex = 21;
        constexpr std::size_t R14DeviceUpdateSurfaceVtableIndex = 30;
        constexpr std::size_t R14DeviceUpdateTextureVtableIndex = 31;
        constexpr std::size_t R14DeviceStretchRectVtableIndex = 34;
        constexpr std::size_t R14DeviceColorFillVtableIndex = 35;
        constexpr std::size_t R14SurfaceLockRectVtableIndex = 13;
        constexpr std::size_t R14SurfaceGetDCVtableIndex = 15;

        SafetyHookInline R14CreateTextureR13Hook{};
        SafetyHookInline R14TextureLockR13Hook{};
        SafetyHookInline R14TextureUnlockR13Hook{};
        SafetyHookInline R14InstallCompatR13Hook{};
        SafetyHookInline R14TextureReleaseHook{};
        SafetyHookInline R14TextureGenerateMipSubLevelsHook{};
        SafetyHookInline R14TextureGetSurfaceLevelHook{};
        SafetyHookInline R14TextureAddDirtyRectHook{};
        SafetyHookInline R14DeviceUpdateSurfaceHook{};
        SafetyHookInline R14DeviceUpdateTextureHook{};
        SafetyHookInline R14DeviceStretchRectHook{};
        SafetyHookInline R14DeviceColorFillHook{};
        SafetyHookInline R14SurfaceLockRectHook{};
        SafetyHookInline R14SurfaceGetDCHook{};

        void* R14TextureReleaseTarget = nullptr;
        void* R14TextureGenerateMipSubLevelsTarget = nullptr;
        void* R14TextureGetSurfaceLevelTarget = nullptr;
        void* R14TextureAddDirtyRectTarget = nullptr;
        void* R14DeviceUpdateSurfaceTarget = nullptr;
        void* R14DeviceUpdateTextureTarget = nullptr;
        void* R14DeviceStretchRectTarget = nullptr;
        void* R14DeviceColorFillTarget = nullptr;
        void* R14SurfaceLockRectTarget = nullptr;
        void* R14SurfaceGetDCTarget = nullptr;

        enum class R14ShadowMode : std::uint8_t
        {
            Shadow,
            DirectOnly
        };

        thread_local std::uint32_t R14InternalReleaseDepth = 0;
        constexpr std::uint64_t R14ShadowBudgetBytes =
            384ull * 1024ull * 1024ull;
        std::atomic<std::uint64_t> R14ShadowBytes{0};
        std::atomic<std::uint64_t> R14ShadowBudgetRejects{0};

        struct R14ShadowEntry
        {
            std::mutex mutex;
            IDirect3DTexture9* gpu = nullptr; // identity only; registry owns no ref
            IDirect3DTexture9* cpu = nullptr;
            IDirect3DDevice9* device = nullptr;
            R14ShadowMode mode = R14ShadowMode::Shadow;
            std::uint32_t validMask = 0;
            std::uint32_t dirtyMask = 0;
            std::uint32_t lockedMask = 0;
            std::uint32_t readOnlyMask = 0;
            RECT lockRect[R14MaxTrackedLevels]{};
            bool lockRectValid[R14MaxTrackedLevels]{};
            bool retirePending = false;
            bool externalWriteDuringLock = false;
            std::uint64_t shadowBytes = 0;

            ~R14ShadowEntry()
            {
                ++R14InternalReleaseDepth;
                if (cpu) cpu->Release();
                if (device) device->Release();
                if (shadowBytes)
                    R14ShadowBytes.fetch_sub(
                        shadowBytes, std::memory_order_acq_rel);
                --R14InternalReleaseDepth;
            }
        };

        using R14EntryPtr = std::shared_ptr<R14ShadowEntry>;
        std::mutex R14RegistryMutex;
        std::mutex R14ResourceHookMutex;
        std::unordered_map<IDirect3DTexture9*, R14EntryPtr> R14Shadows;
        IDirect3DDevice9* R14RegistryDevice = nullptr; // identity only
        thread_local std::uint32_t R14InternalUploadDepth = 0;

        std::atomic<std::uint64_t> R14ShadowCreated{0};
        std::atomic<std::uint64_t> R14ShadowCreateFailed{0};
        std::atomic<std::uint64_t> R14DirectOnlyFallbacks{0};
        std::atomic<std::uint64_t> R14ShadowReleased{0};
        std::atomic<std::uint64_t> R14ShadowRetired{0};
        std::atomic<std::uint64_t> R14DeviceReplacementRetires{0};
        std::atomic<std::uint64_t> R14ShadowLocks{0};
        std::atomic<std::uint64_t> R14ShadowUploads{0};
        std::atomic<std::uint64_t> R14ShadowUploadFailed{0};
        std::atomic<bool> R14FirstActiveLogged{false};
        std::atomic<bool> R14FirstFallbackLogged{false};
        std::atomic<bool> R14FirstHardFailureLogged{false};
        std::atomic<bool> R14FirstUploadFailureLogged{false};
        std::atomic<bool> R14FirstRetireLogged{false};
        std::atomic<bool> R14FirstConcurrentWriteLogged{false};

        struct R14InternalUploadScope
        {
            R14InternalUploadScope() noexcept { ++R14InternalUploadDepth; }
            ~R14InternalUploadScope()
            {
                if (R14InternalUploadDepth) --R14InternalUploadDepth;
            }
        };

        R14EntryPtr R14Find(IDirect3DTexture9* texture) noexcept
        {
            if (!texture) return {};
            std::lock_guard<std::mutex> lock(R14RegistryMutex);
            const auto found = R14Shadows.find(texture);
            return found == R14Shadows.end() ? R14EntryPtr{} : found->second;
        }

        void R14Erase(IDirect3DTexture9* texture) noexcept
        {
            R14EntryPtr released;
            {
                std::lock_guard<std::mutex> lock(R14RegistryMutex);
                const auto found = R14Shadows.find(texture);
                if (found == R14Shadows.end())
                    return;
                released = std::move(found->second);
                R14Shadows.erase(found);
            }
            ++R14ShadowReleased;
        }

        IDirect3DTexture9* R14DetachShadowLocked(
            R14ShadowEntry& entry) noexcept
        {
            IDirect3DTexture9* cpu = entry.cpu;
            entry.cpu = nullptr;
            if (entry.shadowBytes)
            {
                R14ShadowBytes.fetch_sub(
                    entry.shadowBytes, std::memory_order_acq_rel);
                entry.shadowBytes = 0;
            }
            entry.mode = R14ShadowMode::DirectOnly;
            entry.validMask = 0;
            entry.dirtyMask = 0;
            entry.lockedMask = 0;
            entry.readOnlyMask = 0;
            entry.retirePending = false;
            entry.externalWriteDuringLock = false;
            for (std::uint32_t level = 0; level < R14MaxTrackedLevels; ++level)
                entry.lockRectValid[level] = false;
            return cpu;
        }

        void R14LogRetiredShadow(const char* reason) noexcept;

        void R14AdoptCompatDevice(IDirect3DDevice9* device) noexcept
        {
            if (!device) return;

            std::vector<R14EntryPtr> oldDeviceEntries;
            {
                std::lock_guard<std::mutex> lock(R14RegistryMutex);
                if (!R14RegistryDevice)
                {
                    R14RegistryDevice = device;
                    return;
                }
                if (R14RegistryDevice == device)
                    return;

                // Snapshot without removing records. A texture may still be
                // CPU-shadow locked while its owning device is being replaced;
                // its matching Unlock must continue to find the same entry.
                try
                {
                    oldDeviceEntries.reserve(R14Shadows.size());
                    for (const auto& item : R14Shadows)
                    {
                        if (item.second && item.second->device == R14RegistryDevice)
                            oldDeviceEntries.push_back(item.second);
                    }
                }
                catch (...)
                {
                    // Keep the old identity authoritative so new resources fail
                    // open to R13 rather than mixing two shadow generations.
                    spdlog::error(
                        "VR R14 EX: could not snapshot old-device shadows during replacement; new-device shadows remain disabled");
                    return;
                }
                R14RegistryDevice = device;
            }

            std::size_t retiredNow = 0;
            std::size_t pendingUnlock = 0;
            for (const auto& entry : oldDeviceEntries)
            {
                IDirect3DTexture9* cpu = nullptr;
                {
                    std::lock_guard<std::mutex> lock(entry->mutex);
                    if (entry->mode == R14ShadowMode::DirectOnly)
                        continue;
                    entry->retirePending = true;
                    if (entry->lockedMask == 0)
                    {
                        cpu = R14DetachShadowLocked(*entry);
                        ++retiredNow;
                    }
                    else
                    {
                        ++pendingUnlock;
                    }
                }
                if (cpu) cpu->Release();
                if (cpu) R14LogRetiredShadow("compat device replacement");
            }

            ++R14DeviceReplacementRetires;
            spdlog::info(
                "VR R14 EX: compat device replacement retired {} old texture shadows immediately and deferred {} locked shadows until matching Unlock",
                retiredNow, pendingUnlock);
        }

        void R14AbandonCompatDevice(IDirect3DDevice9* device) noexcept
        {
            if (!device) return;
            std::lock_guard<std::mutex> lock(R14RegistryMutex);
            if (R14RegistryDevice == device && R14Shadows.empty())
                R14RegistryDevice = nullptr;
        }

        void R14LogRetiredShadow(const char* reason) noexcept
        {
            ++R14ShadowRetired;
            if (!R14FirstRetireLogged.exchange(true))
            {
                spdlog::warn(
                    "VR R14 EX: first CPU shadow retired reason={}; this texture now stays on one coherent direct-GPU path",
                    reason ? reason : "unknown");
            }
        }

        bool R14RetireShadow(const R14EntryPtr& entry,
            const char* reason) noexcept
        {
            if (!entry) return true;
            IDirect3DTexture9* cpu = nullptr;
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                if (entry->mode == R14ShadowMode::DirectOnly)
                    return true;
                if (entry->lockedMask != 0)
                    return false;
                cpu = R14DetachShadowLocked(*entry);
            }
            if (cpu) cpu->Release();
            R14LogRetiredShadow(reason);
            return true;
        }

        void R14MarkExternalGpuWrite(IDirect3DTexture9* texture,
            const char* reason) noexcept
        {
            const R14EntryPtr entry = R14Find(texture);
            if (!entry) return;

            IDirect3DTexture9* cpu = nullptr;
            bool retired = false;
            bool concurrent = false;
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                if (entry->mode == R14ShadowMode::DirectOnly)
                    return;
                concurrent = entry->lockedMask != 0;
                entry->validMask = 0;
                entry->externalWriteDuringLock |= concurrent;
                entry->retirePending = true;
                if (!concurrent)
                {
                    cpu = R14DetachShadowLocked(*entry);
                    retired = true;
                }
            }
            if (cpu) cpu->Release();
            if (retired) R14LogRetiredShadow(reason);
            if (concurrent && !R14FirstConcurrentWriteLogged.exchange(true))
            {
                spdlog::error(
                    "VR R14 EX: external GPU texture write overlapped a CPU-shadow Lock; upload is suppressed and the shadow will retire at the final Unlock");
            }
        }

        std::uint64_t R14EstimateShadowBytes(
            IDirect3DTexture9* gpu) noexcept
        {
            if (!gpu) return 0;
            const UINT levels = gpu->GetLevelCount();
            std::uint64_t total = 0;
            for (UINT level = 0;
                 level < levels && level < R14MaxTrackedLevels; ++level)
            {
                D3DSURFACE_DESC d{};
                if (FAILED(gpu->GetLevelDesc(level, &d)) ||
                    d.Width == 0 || d.Height == 0)
                    return 0;

                std::uint64_t bytes = 0;
                if (d.Format == D3DFMT_DXT1)
                {
                    bytes = ((static_cast<std::uint64_t>(d.Width) + 3) / 4) *
                        ((static_cast<std::uint64_t>(d.Height) + 3) / 4) * 8;
                }
                else if (d.Format == D3DFMT_DXT2 ||
                         d.Format == D3DFMT_DXT3 ||
                         d.Format == D3DFMT_DXT4 ||
                         d.Format == D3DFMT_DXT5)
                {
                    bytes = ((static_cast<std::uint64_t>(d.Width) + 3) / 4) *
                        ((static_cast<std::uint64_t>(d.Height) + 3) / 4) * 16;
                }
                else
                {
                    std::uint64_t bpp = 4;
                    if (d.Format == D3DFMT_A16B16G16R16 ||
                        d.Format == D3DFMT_A16B16G16R16F)
                        bpp = 8;
                    else if (d.Format == D3DFMT_A32B32G32R32F)
                        bpp = 16;
                    else if (d.Format == D3DFMT_R5G6B5 ||
                             d.Format == D3DFMT_A1R5G5B5 ||
                             d.Format == D3DFMT_A4R4G4B4 ||
                             d.Format == D3DFMT_A8L8)
                        bpp = 2;
                    else if (d.Format == D3DFMT_A8 ||
                             d.Format == D3DFMT_L8)
                        bpp = 1;
                    bytes = static_cast<std::uint64_t>(d.Width) *
                        static_cast<std::uint64_t>(d.Height) * bpp;
                }

                if (bytes > R14ShadowBudgetBytes ||
                    total > R14ShadowBudgetBytes - bytes)
                    return R14ShadowBudgetBytes + 1;
                total += bytes;
            }
            return total;
        }

        bool R14ReserveShadowBytes(
            std::uint64_t bytes) noexcept
        {
            if (!bytes || bytes > R14ShadowBudgetBytes)
                return false;
            std::uint64_t current =
                R14ShadowBytes.load(std::memory_order_acquire);
            for (;;)
            {
                if (current > R14ShadowBudgetBytes - bytes)
                    return false;
                if (R14ShadowBytes.compare_exchange_weak(
                        current, current + bytes,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                    return true;
            }
        }

        bool R14Track(IDirect3DDevice9* device, IDirect3DTexture9* gpu,
            IDirect3DTexture9* cpu) noexcept
        {
            if (!device || !gpu || !cpu) return false;
            try
            {
                auto entry = std::make_shared<R14ShadowEntry>();
                entry->gpu = gpu;
                entry->device = device;
                entry->shadowBytes = R14EstimateShadowBytes(gpu);
                if (!R14ReserveShadowBytes(entry->shadowBytes))
                {
                    entry->shadowBytes = 0;
                    ++R14ShadowBudgetRejects;
                    return false;
                }
                const UINT levels = gpu->GetLevelCount();
                entry->validMask = levels >= R14MaxTrackedLevels
                    ? 0xFFFFFFFFu : ((1u << levels) - 1u);
                device->AddRef();

                std::lock_guard<std::mutex> lock(R14RegistryMutex);
                if (!R14RegistryDevice)
                    R14RegistryDevice = device;
                if (R14RegistryDevice != device)
                    return false;
                const auto inserted = R14Shadows.emplace(gpu, entry);
                if (!inserted.second)
                    return false;
                // Ownership transfers only after insertion succeeds. On every
                // failure path the caller still owns and releases cpu exactly once.
                entry->cpu = cpu;
            }
            catch (...)
            {
                return false;
            }

            ++R14ShadowCreated;
            if (!R14FirstActiveLogged.exchange(true))
            {
                spdlog::info(
                    "VR R14 EX: lifetime-bound MANAGED 2D CPU-shadow path ACTIVE; no live or locked texture can be evicted by a fixed-capacity ring");
            }
            return true;
        }

        bool R14TrackDirectOnly(IDirect3DDevice9* device,
            IDirect3DTexture9* gpu) noexcept
        {
            if (!device || !gpu) return false;
            try
            {
                auto entry = std::make_shared<R14ShadowEntry>();
                entry->gpu = gpu;
                entry->device = device;
                entry->mode = R14ShadowMode::DirectOnly;
                entry->validMask = 0;
                device->AddRef();

                std::lock_guard<std::mutex> lock(R14RegistryMutex);
                if (!R14RegistryDevice)
                    R14RegistryDevice = device;
                if (R14RegistryDevice != device)
                    return false;
                const auto inserted = R14Shadows.emplace(gpu, entry);
                if (!inserted.second)
                    return false;
            }
            catch (...)
            {
                return false;
            }
            ++R14DirectOnlyFallbacks;
            return true;
        }

        HRESULT R14CreateCpuShadow(IDirect3DDevice9* device,
            IDirect3DTexture9* gpu, IDirect3DTexture9*& shadow) noexcept
        {
            shadow = nullptr;
            if (!device || !gpu) return D3DERR_INVALIDCALL;
            D3DSURFACE_DESC desc{};
            const HRESULT descHr = gpu->GetLevelDesc(0, &desc);
            if (FAILED(descHr) || desc.Width == 0 || desc.Height == 0)
                return FAILED(descHr) ? descHr : D3DERR_INVALIDCALL;
            const UINT levels = gpu->GetLevelCount();
            if (levels == 0 || levels > R14MaxTrackedLevels)
                return D3DERR_NOTAVAILABLE;

            const std::uint64_t estimate = R14EstimateShadowBytes(gpu);
            const std::uint64_t current =
                R14ShadowBytes.load(std::memory_order_acquire);
            if (!estimate || estimate > R14ShadowBudgetBytes ||
                current > R14ShadowBudgetBytes - estimate)
            {
                ++R14ShadowBudgetRejects;
                return D3DERR_OUTOFVIDEOMEMORY;
            }

            const HRESULT hr = device->CreateTexture(
                desc.Width, desc.Height, levels, 0, desc.Format,
                D3DPOOL_SYSTEMMEM, &shadow, nullptr);
            return SUCCEEDED(hr) && shadow ? D3D_OK : hr;
        }

        ULONG __stdcall TextureReleaseDestR14(IDirect3DTexture9* texture)
        {
            const ULONG remaining =
                R14TextureReleaseHook.stdcall<ULONG>(texture);
            if (remaining == 0 && R14InternalReleaseDepth == 0)
                R14Erase(texture);
            return remaining;
        }

        bool R14EnsureSurfaceHooks(IDirect3DSurface9* surface) noexcept;

        HRESULT __stdcall TextureGetSurfaceLevelDestR14(
            IDirect3DTexture9* texture, UINT level, IDirect3DSurface9** surface)
        {
            const R14EntryPtr entry = R14Find(texture);
            const HRESULT hr = R14TextureGetSurfaceLevelHook.stdcall<HRESULT>(
                texture, level, surface);
            if (!entry || FAILED(hr) || !surface || !*surface)
                return hr;

            // Merely borrowing a level surface is not a write. Keep the CPU
            // shadow alive when we can observe the surface's mutating entry
            // points; otherwise retire before exposing an untracked alias.
            if (R14EnsureSurfaceHooks(*surface))
                return hr;
            if (R14RetireShadow(entry, "untracked external level surface"))
                return hr;

            (*surface)->Release();
            *surface = nullptr;
            return D3DERR_INVALIDCALL;
        }

        void __stdcall TextureGenerateMipSubLevelsDestR14(
            IDirect3DTexture9* texture)
        {
            R14TextureGenerateMipSubLevelsHook.stdcall<void>(texture);
            if (R14InternalUploadDepth == 0)
            {
                R14MarkExternalGpuWrite(texture,
                    "external GenerateMipSubLevels");
            }
        }

        HRESULT __stdcall TextureAddDirtyRectDestR14(
            IDirect3DTexture9* texture, const RECT* rect)
        {
            const R14EntryPtr entry = R14Find(texture);
            if (entry)
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                if (entry->mode == R14ShadowMode::Shadow &&
                    !entry->retirePending)
                {
                    // Every writable shadow Unlock is explicitly uploaded.
                    // Preserve MANAGED AddDirtyRect success semantics without
                    // relying on level-zero UpdateTexture dirty propagation.
                    return D3D_OK;
                }
            }
            return R14TextureAddDirtyRectHook.stdcall<HRESULT>(texture, rect);
        }

        HRESULT __stdcall UpdateSurfaceDestR14(IDirect3DDevice9* device,
            IDirect3DSurface9* source, const RECT* sourceRect,
            IDirect3DSurface9* destination, const POINT* destinationPoint)
        {
            const HRESULT hr = R14DeviceUpdateSurfaceHook.stdcall<HRESULT>(
                device, source, sourceRect, destination, destinationPoint);
            if (SUCCEEDED(hr) && destination && R14InternalUploadDepth == 0)
            {
                IDirect3DTexture9* texture = nullptr;
                if (SUCCEEDED(destination->GetContainer(
                        __uuidof(IDirect3DTexture9),
                        reinterpret_cast<void**>(&texture))) && texture)
                {
                    R14MarkExternalGpuWrite(texture, "external UpdateSurface");
                    texture->Release();
                }
            }
            return hr;
        }

        HRESULT __stdcall UpdateTextureDestR14(IDirect3DDevice9* device,
            IDirect3DBaseTexture9* source,
            IDirect3DBaseTexture9* destination)
        {
            const HRESULT hr = R14DeviceUpdateTextureHook.stdcall<HRESULT>(
                device, source, destination);
            if (SUCCEEDED(hr) && destination && R14InternalUploadDepth == 0)
            {
                IDirect3DTexture9* texture = nullptr;
                if (SUCCEEDED(destination->QueryInterface(
                        __uuidof(IDirect3DTexture9),
                        reinterpret_cast<void**>(&texture))) && texture)
                {
                    R14MarkExternalGpuWrite(texture, "external UpdateTexture");
                    texture->Release();
                }
            }
            return hr;
        }

        void R14MarkSurfaceExternalWrite(IDirect3DSurface9* surface,
            const char* reason) noexcept
        {
            if (!surface) return;
            IDirect3DTexture9* texture = nullptr;
            if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9),
                    reinterpret_cast<void**>(&texture))) && texture)
            {
                R14MarkExternalGpuWrite(texture, reason);
                texture->Release();
            }
        }

        HRESULT __stdcall SurfaceLockRectDestR14(IDirect3DSurface9* surface,
            D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
        {
            const HRESULT hr = R14SurfaceLockRectHook.stdcall<HRESULT>(
                surface, locked, rect, flags);
            if (SUCCEEDED(hr) && (flags & D3DLOCK_READONLY) == 0)
                R14MarkSurfaceExternalWrite(surface, "external level-surface LockRect");
            return hr;
        }

        HRESULT __stdcall SurfaceGetDCDestR14(IDirect3DSurface9* surface,
            HDC* dc)
        {
            const HRESULT hr = R14SurfaceGetDCHook.stdcall<HRESULT>(surface, dc);
            if (SUCCEEDED(hr))
                R14MarkSurfaceExternalWrite(surface, "external level-surface GetDC");
            return hr;
        }

        bool R14EnsureSurfaceHooks(IDirect3DSurface9* surface) noexcept
        {
            if (!surface) return false;
            std::lock_guard<std::mutex> installLock(R14ResourceHookMutex);
            void** vtable = *reinterpret_cast<void***>(surface);
            if (!vtable) return false;
            if (R14SurfaceLockRectHook && R14SurfaceGetDCHook)
            {
                return R14SurfaceLockRectTarget ==
                        vtable[R14SurfaceLockRectVtableIndex] &&
                    R14SurfaceGetDCTarget == vtable[R14SurfaceGetDCVtableIndex];
            }
            const auto disabled = safetyhook::InlineHook::StartDisabled;
            R14SurfaceLockRectHook = safetyhook::create_inline(
                vtable[R14SurfaceLockRectVtableIndex], SurfaceLockRectDestR14,
                disabled);
            R14SurfaceGetDCHook = safetyhook::create_inline(
                vtable[R14SurfaceGetDCVtableIndex], SurfaceGetDCDestR14,
                disabled);
            if (!R14SurfaceLockRectHook || !R14SurfaceGetDCHook ||
                !R14SurfaceLockRectHook.enable().has_value() ||
                !R14SurfaceGetDCHook.enable().has_value())
            {
                R14SurfaceGetDCHook = {};
                R14SurfaceLockRectHook = {};
                R14SurfaceLockRectTarget = nullptr;
                R14SurfaceGetDCTarget = nullptr;
                return false;
            }
            R14SurfaceLockRectTarget = vtable[R14SurfaceLockRectVtableIndex];
            R14SurfaceGetDCTarget = vtable[R14SurfaceGetDCVtableIndex];
            return true;
        }

        HRESULT __stdcall StretchRectDestR14(IDirect3DDevice9* device,
            IDirect3DSurface9* source, const RECT* sourceRect,
            IDirect3DSurface9* destination, const RECT* destinationRect,
            D3DTEXTUREFILTERTYPE filter)
        {
            const HRESULT hr = R14DeviceStretchRectHook.stdcall<HRESULT>(
                device, source, sourceRect, destination, destinationRect, filter);
            if (SUCCEEDED(hr) && destination && R14InternalUploadDepth == 0)
                R14MarkSurfaceExternalWrite(destination, "external StretchRect");
            return hr;
        }

        HRESULT __stdcall ColorFillDestR14(IDirect3DDevice9* device,
            IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color)
        {
            const HRESULT hr = R14DeviceColorFillHook.stdcall<HRESULT>(
                device, surface, rect, color);
            if (SUCCEEDED(hr) && surface && R14InternalUploadDepth == 0)
                R14MarkSurfaceExternalWrite(surface, "external ColorFill");
            return hr;
        }

        bool InstallManagedResourceCompatR14(IDirect3DDevice9Ex* deviceEx)
        {
            const bool installed =
                R14InstallCompatR13Hook.call<bool>(deviceEx);
            if (installed && deviceEx)
            {
                R14AdoptCompatDevice(
                    static_cast<IDirect3DDevice9*>(deviceEx));
            }
            return installed;
        }

        bool R14EnsureResourceHooks(IDirect3DDevice9* device,
            IDirect3DTexture9* texture) noexcept
        {
            if (!device || !texture) return false;
            std::lock_guard<std::mutex> installLock(R14ResourceHookMutex);

            void** textureVtable = *reinterpret_cast<void***>(texture);
            void** deviceVtable = *reinterpret_cast<void***>(device);
            if (!textureVtable || !deviceVtable)
                return false;

            if (R14TextureReleaseHook &&
                R14TextureGenerateMipSubLevelsHook &&
                R14TextureGetSurfaceLevelHook &&
                R14TextureAddDirtyRectHook && R14DeviceUpdateSurfaceHook &&
                R14DeviceUpdateTextureHook && R14DeviceStretchRectHook &&
                R14DeviceColorFillHook)
            {
                // A replacement device may be supplied by a wrapper with a
                // different implementation vtable. Never claim coverage from
                // hooks installed on the old implementation.
                return R14TextureReleaseTarget ==
                        textureVtable[R14TextureReleaseVtableIndex] &&
                    R14TextureGenerateMipSubLevelsTarget ==
                        textureVtable[
                            R14TextureGenerateMipSubLevelsVtableIndex] &&
                    R14TextureGetSurfaceLevelTarget ==
                        textureVtable[R14TextureGetSurfaceLevelVtableIndex] &&
                    R14TextureAddDirtyRectTarget ==
                        textureVtable[R14TextureAddDirtyRectVtableIndex] &&
                    R14DeviceUpdateSurfaceTarget ==
                        deviceVtable[R14DeviceUpdateSurfaceVtableIndex] &&
                    R14DeviceUpdateTextureTarget ==
                        deviceVtable[R14DeviceUpdateTextureVtableIndex] &&
                    R14DeviceStretchRectTarget ==
                        deviceVtable[R14DeviceStretchRectVtableIndex] &&
                    R14DeviceColorFillTarget ==
                        deviceVtable[R14DeviceColorFillVtableIndex];
            }

            const auto disabled = safetyhook::InlineHook::StartDisabled;
            R14TextureReleaseHook = safetyhook::create_inline(
                textureVtable[R14TextureReleaseVtableIndex],
                TextureReleaseDestR14, disabled);
            R14TextureGenerateMipSubLevelsHook = safetyhook::create_inline(
                textureVtable[R14TextureGenerateMipSubLevelsVtableIndex],
                TextureGenerateMipSubLevelsDestR14, disabled);
            R14TextureGetSurfaceLevelHook = safetyhook::create_inline(
                textureVtable[R14TextureGetSurfaceLevelVtableIndex],
                TextureGetSurfaceLevelDestR14, disabled);
            R14TextureAddDirtyRectHook = safetyhook::create_inline(
                textureVtable[R14TextureAddDirtyRectVtableIndex],
                TextureAddDirtyRectDestR14, disabled);
            R14DeviceUpdateSurfaceHook = safetyhook::create_inline(
                deviceVtable[R14DeviceUpdateSurfaceVtableIndex],
                UpdateSurfaceDestR14, disabled);
            R14DeviceUpdateTextureHook = safetyhook::create_inline(
                deviceVtable[R14DeviceUpdateTextureVtableIndex],
                UpdateTextureDestR14, disabled);
            R14DeviceStretchRectHook = safetyhook::create_inline(
                deviceVtable[R14DeviceStretchRectVtableIndex],
                StretchRectDestR14, disabled);
            R14DeviceColorFillHook = safetyhook::create_inline(
                deviceVtable[R14DeviceColorFillVtableIndex],
                ColorFillDestR14, disabled);

            SafetyHookInline* hooks[]{
                &R14TextureReleaseHook,
                &R14TextureGenerateMipSubLevelsHook,
                &R14TextureGetSurfaceLevelHook,
                &R14TextureAddDirtyRectHook,
                &R14DeviceUpdateSurfaceHook,
                &R14DeviceUpdateTextureHook,
                &R14DeviceStretchRectHook,
                &R14DeviceColorFillHook
            };
            for (auto* hook : hooks)
            {
                if (!*hook || !hook->enable().has_value())
                {
                    R14DeviceColorFillHook = {};
                    R14DeviceStretchRectHook = {};
                    R14DeviceUpdateTextureHook = {};
                    R14DeviceUpdateSurfaceHook = {};
                    R14TextureAddDirtyRectHook = {};
                    R14TextureGetSurfaceLevelHook = {};
                    R14TextureGenerateMipSubLevelsHook = {};
                    R14TextureReleaseHook = {};
                    R14TextureReleaseTarget = nullptr;
                    R14TextureGenerateMipSubLevelsTarget = nullptr;
                    R14TextureGetSurfaceLevelTarget = nullptr;
                    R14TextureAddDirtyRectTarget = nullptr;
                    R14DeviceUpdateSurfaceTarget = nullptr;
                    R14DeviceUpdateTextureTarget = nullptr;
                    R14DeviceStretchRectTarget = nullptr;
                    R14DeviceColorFillTarget = nullptr;
                    return false;
                }
            }
            R14TextureReleaseTarget =
                textureVtable[R14TextureReleaseVtableIndex];
            R14TextureGenerateMipSubLevelsTarget =
                textureVtable[R14TextureGenerateMipSubLevelsVtableIndex];
            R14TextureGetSurfaceLevelTarget =
                textureVtable[R14TextureGetSurfaceLevelVtableIndex];
            R14TextureAddDirtyRectTarget =
                textureVtable[R14TextureAddDirtyRectVtableIndex];
            R14DeviceUpdateSurfaceTarget =
                deviceVtable[R14DeviceUpdateSurfaceVtableIndex];
            R14DeviceUpdateTextureTarget =
                deviceVtable[R14DeviceUpdateTextureVtableIndex];
            R14DeviceStretchRectTarget =
                deviceVtable[R14DeviceStretchRectVtableIndex];
            R14DeviceColorFillTarget =
                deviceVtable[R14DeviceColorFillVtableIndex];
            return true;
        }

        HRESULT __stdcall CreateTextureCompatDestR14(
            IDirect3DDevice9* device, UINT width, UINT height, UINT levels,
            DWORD usage, D3DFORMAT format, D3DPOOL pool,
            IDirect3DTexture9** texture, HANDLE* sharedHandle)
        {
            const HRESULT hr = R14CreateTextureR13Hook.stdcall<HRESULT>(
                device, width, height, levels, usage, format, pool,
                texture, sharedHandle);
            if (FAILED(hr) || pool != D3DPOOL_MANAGED || !texture || !*texture ||
                !OutRunVRD3D9ExUpgrade::IsCompatDevice(device))
                return hr;

            IDirect3DTexture9* shadow = nullptr;
            const HRESULT shadowHr = R14CreateCpuShadow(
                device, *texture, shadow);
            const bool hooksReady = R14EnsureResourceHooks(device, *texture);

            if (SUCCEEDED(shadowHr) && shadow && hooksReady &&
                R14Track(device, *texture, shadow))
            {
                return hr; // registry owns shadow on success
            }

            // Hardware evidence from f15f1acd showed some translated MANAGED
            // textures cannot allocate a SYSTEMMEM peer even though the created
            // DEFAULT texture itself is usable. Preserve the game's successful
            // CreateTexture result only for that narrow case, and keep the
            // resource in the lifecycle registry as DirectOnly so LockRect
            // falls through to the already-instrumented R13 path.
            if (FAILED(shadowHr) && hooksReady &&
                R14TrackDirectOnly(device, *texture))
            {
                ++R14ShadowCreateFailed;
                if (!R14FirstFallbackLogged.exchange(true))
                {
                    D3DSURFACE_DESC desc{};
                    (*texture)->GetLevelDesc(0, &desc);
                    spdlog::warn(
                        "VR R14 EX: CPU shadow unavailable hr=0x{:08x} size={}x{} fmt={} levels={}; keeping translated MANAGED texture on tracked DirectOnly compatibility path",
                        static_cast<unsigned>(shadowHr), desc.Width, desc.Height,
                        static_cast<unsigned>(desc.Format),
                        (*texture)->GetLevelCount());
                }
                return hr;
            }

            if (shadow) shadow->Release();
            if (*texture)
            {
                (*texture)->Release();
                *texture = nullptr;
            }
            ++R14ShadowCreateFailed;
            ++OutRunVRD3D9ExUpgrade::ManagedCreateFailures;
            if (!R14FirstHardFailureLogged.exchange(true))
            {
                spdlog::error(
                    "VR R14 EX: MANAGED 2D resource hook/registry coverage unavailable; hard fail-close retained because DirectOnly lifetime cannot be proven");
            }
            return D3DERR_NOTAVAILABLE;
        }

        DWORD R14SanitizeLockFlags(DWORD flags) noexcept
        {
            return flags & ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE |
                D3DLOCK_NO_DIRTY_UPDATE);
        }

        HRESULT __stdcall TextureLockRectR14(IDirect3DTexture9* texture,
            UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
        {
            const R14EntryPtr entry = R14Find(texture);
            if (!entry || level >= R14MaxTrackedLevels)
                return R14TextureLockR13Hook.stdcall<HRESULT>(
                    texture, level, locked, rect, flags);

            IDirect3DTexture9* cpuToRelease = nullptr;
            HRESULT shadowHr = D3DERR_INVALIDCALL;
            {
                std::unique_lock<std::mutex> lock(entry->mutex);
                if (entry->mode == R14ShadowMode::DirectOnly)
                {
                    lock.unlock();
                    return R14TextureLockR13Hook.stdcall<HRESULT>(
                        texture, level, locked, rect, flags);
                }
                if (entry->retirePending)
                {
                    if (entry->lockedMask != 0)
                        return D3DERR_INVALIDCALL;
                    cpuToRelease = R14DetachShadowLocked(*entry);
                }
                else
                {
                    const std::uint32_t bit = 1u << level;
                    if ((entry->validMask & bit) == 0)
                    {
                        entry->retirePending = true;
                        if (entry->lockedMask != 0)
                            return D3DERR_INVALIDCALL;
                        cpuToRelease = R14DetachShadowLocked(*entry);
                        lock.unlock();
                        if (cpuToRelease) cpuToRelease->Release();
                        R14LogRetiredShadow("invalid mip shadow");
                        return R14TextureLockR13Hook.stdcall<HRESULT>(
                            texture, level, locked, rect, flags);
                    }
                    shadowHr = entry->cpu->LockRect(
                        level, locked, rect, R14SanitizeLockFlags(flags));
                    if (SUCCEEDED(shadowHr))
                    {
                        entry->lockedMask |= bit;
                        if ((flags & D3DLOCK_READONLY) != 0)
                            entry->readOnlyMask |= bit;
                        else
                        {
                            entry->readOnlyMask &= ~bit;
                            entry->dirtyMask |= bit;
                        }
                        if (rect)
                        {
                            entry->lockRect[level] = *rect;
                            entry->lockRectValid[level] = true;
                        }
                        else
                        {
                            entry->lockRect[level] = {};
                            entry->lockRectValid[level] = false;
                        }
                        ++R14ShadowLocks;
                        return shadowHr;
                    }

                    // Never mix a direct GPU lock with outstanding shadow locks.
                    // Once they drain, retire the shadow and use one path only.
                    entry->retirePending = true;
                    if (entry->lockedMask != 0)
                        return shadowHr;
                    cpuToRelease = R14DetachShadowLocked(*entry);
                }
            }

            if (cpuToRelease) cpuToRelease->Release();
            R14LogRetiredShadow("CPU shadow LockRect failure");
            return R14TextureLockR13Hook.stdcall<HRESULT>(
                texture, level, locked, rect, flags);
        }

        bool R14BlockCompressed(D3DFORMAT format) noexcept
        {
            return format == D3DFMT_DXT1 || format == D3DFMT_DXT2 ||
                format == D3DFMT_DXT3 || format == D3DFMT_DXT4 ||
                format == D3DFMT_DXT5;
        }

        HRESULT R14CopyWholeLevelByLock(R14ShadowEntry& entry,
            UINT level) noexcept
        {
            D3DSURFACE_DESC desc{};
            if (FAILED(entry.cpu->GetLevelDesc(level, &desc)))
                return D3DERR_INVALIDCALL;

            D3DLOCKED_RECT source{};
            D3DLOCKED_RECT destination{};
            HRESULT hr = R14TextureLockR13Hook.stdcall<HRESULT>(
                entry.cpu, level, &source, nullptr, D3DLOCK_READONLY);
            if (FAILED(hr)) return hr;

            const bool singleLevelTexture = entry.gpu->GetLevelCount() <= 1;
            const DWORD destinationFlags =
                singleLevelTexture ? D3DLOCK_DISCARD : 0;
            hr = R14TextureLockR13Hook.stdcall<HRESULT>(
                entry.gpu, level, &destination, nullptr, destinationFlags);
            if (FAILED(hr) && destinationFlags != 0)
            {
                hr = R14TextureLockR13Hook.stdcall<HRESULT>(
                    entry.gpu, level, &destination, nullptr, 0);
            }

            const bool destinationLocked = SUCCEEDED(hr);
            if (destinationLocked)
            {
                const UINT rows = R14BlockCompressed(desc.Format)
                    ? std::max<UINT>(1, (desc.Height + 3) / 4)
                    : desc.Height;
                const std::size_t sourcePitch = static_cast<std::size_t>(
                    source.Pitch < 0 ? -static_cast<std::int64_t>(source.Pitch)
                                     : source.Pitch);
                const std::size_t destinationPitch = static_cast<std::size_t>(
                    destination.Pitch < 0
                        ? -static_cast<std::int64_t>(destination.Pitch)
                        : destination.Pitch);
                const std::size_t rowBytes =
                    std::min(sourcePitch, destinationPitch);
                if (!source.pBits || !destination.pBits || rowBytes == 0)
                {
                    hr = E_FAIL;
                }
                else for (UINT row = 0; row < rows; ++row)
                {
                    const auto* src = static_cast<const std::uint8_t*>(
                        source.pBits) + static_cast<std::ptrdiff_t>(row) *
                        source.Pitch;
                    auto* dst = static_cast<std::uint8_t*>(destination.pBits) +
                        static_cast<std::ptrdiff_t>(row) * destination.Pitch;
                    std::memcpy(dst, src, rowBytes);
                }
            }

            HRESULT destinationUnlock = D3D_OK;
            if (destinationLocked)
            {
                destinationUnlock =
                    R14TextureUnlockR13Hook.stdcall<HRESULT>(entry.gpu, level);
            }
            const HRESULT sourceUnlock =
                R14TextureUnlockR13Hook.stdcall<HRESULT>(entry.cpu, level);
            if (FAILED(hr)) return hr;
            if (FAILED(destinationUnlock)) return destinationUnlock;
            return sourceUnlock;
        }

        HRESULT R14UploadLevel(R14ShadowEntry& entry, UINT level) noexcept
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

            // UpdateTexture's level-zero dirty propagation cannot prove that an
            // independently changed lower mip is copied. The fallback therefore
            // copies this exact mip in full through the translated texture's
            // direct lock path, or fails without overwriting unrelated content.
            if (FAILED(hr))
                hr = R14CopyWholeLevelByLock(entry, level);
            return hr;
        }

        HRESULT __stdcall TextureUnlockRectR14(IDirect3DTexture9* texture,
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
                    // Preserve the externally-written GPU contents. The caller's
                    // overlapping CPU write cannot be committed coherently.
                    result = D3DERR_INVALIDCALL;
                }
                else if (!readOnly)
                {
                    result = R14UploadLevel(*entry, level);
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
                                "VR R14 EX: exact mip CPU-shadow upload failed hr=0x{:08x}; shadow will retire instead of risking a stale later overwrite",
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
            if (retired) R14LogRetiredShadow("coherency path retired at Unlock");
            return result;
        }

        void R14RollbackHooks() noexcept
        {
            R14TextureUnlockR13Hook = {};
            R14TextureLockR13Hook = {};
            R14CreateTextureR13Hook = {};
            R14InstallCompatR13Hook = {};
        }

        class VRD3D9ExUpgradeR14Hook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRD3D9ExUpgradeR14";
            }
            bool validate() override
            {
                return Settings::VRPreferD3D9Ex;
            }
            bool apply() override
            {
                if (!R13InstallCompatHook)
                {
                    spdlog::error(
                        "VR R14 EX: R13 compatibility installer is unavailable; R14 overlay remains disabled");
                    return false;
                }
                const auto disabled = safetyhook::InlineHook::StartDisabled;
                R14CreateTextureR13Hook = safetyhook::create_inline(
                    reinterpret_cast<void*>(&CreateTextureCompatDestR13),
                    CreateTextureCompatDestR14, disabled);
                R14TextureLockR13Hook = safetyhook::create_inline(
                    reinterpret_cast<void*>(&TextureLockRectR13),
                    TextureLockRectR14, disabled);
                R14TextureUnlockR13Hook = safetyhook::create_inline(
                    reinterpret_cast<void*>(&TextureUnlockRectR13),
                    TextureUnlockRectR14, disabled);
                R14InstallCompatR13Hook = safetyhook::create_inline(
                    reinterpret_cast<void*>(&InstallManagedResourceCompatR13),
                    InstallManagedResourceCompatR14, disabled);

                SafetyHookInline* hooks[]{
                    &R14InstallCompatR13Hook,
                    &R14CreateTextureR13Hook,
                    &R14TextureLockR13Hook,
                    &R14TextureUnlockR13Hook
                };
                for (auto* hook : hooks)
                {
                    if (!*hook || !hook->enable().has_value())
                    {
                        R14RollbackHooks();
                        spdlog::error(
                            "VR R14 EX: CPU-shadow hook transaction failed; R13 remains active");
                        return false;
                    }
                }
                spdlog::info(
                    "VR R14 EX: lifetime-bound, coherency-fail-closed MANAGED 2D shadow compatibility armed");
                return true;
            }
            static VRD3D9ExUpgradeR14Hook instance;
        };

        VRD3D9ExUpgradeR14Hook VRD3D9ExUpgradeR14Hook::instance;
    }
}
