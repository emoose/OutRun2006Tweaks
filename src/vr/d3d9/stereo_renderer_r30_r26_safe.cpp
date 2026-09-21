// R30 screen-space asymmetric-FOV correction overlay.
//
// R29 restores conservative world/effect classification and removes the steady-
// state third mono draw. R30 fixes the remaining HUD convergence problem without
// moving the world image: only main-backbuffer orthographic/ScreenSpace2D draws
// receive an eye-specific clip-space X affine. The affine maps one common
// head-relative tangent-angle interval into each eye's actual asymmetric OpenXR
// FOV, so the same HUD feature lands on the same visual ray in both eyes.
//
// This is intentionally an interim projection-layer HUD solution. It does not
// pretend to be XrCompositionLayerQuad: perspective world draws and fragile
// perspective effects remain entirely owned by R29/R13.

#include "stereo_renderer_r26.cpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Settings
{
    extern Setting<float> VRHudScale;
    extern Setting<float> VRStereoDepth;
    extern Setting<int> SkyGlowFactor;
    extern Setting<bool> SkyGlowTwoStep;
}

namespace OutRunVRRenderer
{
    bool GetLastRawGameWvpWrite(float outConstants[16],
        std::uint64_t& writeSerial, std::uint64_t& topLevelDrawSerial,
        std::uintptr_t& shaderIdentity, std::uint64_t& shaderSerial) noexcept;
}

namespace OutRunVRStereo
{
    namespace
    {
        // R26-safe comparison owner: R26 remains the world/effect authority;
        // only R30 HUD, XYZRHW correction and stereo SkyGlow are layered above.
        std::uint64_t R30SafeTwoEyeDraws = 0;

        void R30ArmSafeFallback(std::uint64_t = 2) noexcept
        {
            FrameStereoIncomplete = true;
            if (R9MonoSeeded && !R9MonoBackupGap)
                R13ForceMonoShadow = true;
        }

        bool R30SafeStereoBase(IDirect3DDevice9* device) noexcept
        {
            if (!IsGameDevice(device) || InternalStereoPass ||
                !TargetIsBackBuffer() || !StereoWanted() || !R9StereoSeeded)
                return false;
            if (!OutRunVR::RuntimeEligibility::MayInjectStereo() ||
                OutRunVR::RuntimeEligibility::RecoveryPending.load(
                    std::memory_order_acquire) ||
                OutRunVR::RuntimeEligibility::PoseWarmupAllowed())
                return false;
            if (FrameStereoIncomplete || R9DeferredDepth ||
                !R9CurrentDepthCanMirror() || AnyAuxRenderTargetActive() ||
                R13ForceMonoShadow ||
                OcclusionQueryTrackingUnavailable.load(
                    std::memory_order_acquire) ||
                ActiveOcclusionQueries.load(std::memory_order_acquire) > 0)
                return false;
            return true;
        }

        SafetyHookInline R30DrawPrimitiveR29Hook{};
        SafetyHookInline R30DrawIndexedPrimitiveR29Hook{};
        SafetyHookInline R30DrawPrimitiveUPR29Hook{};
        SafetyHookInline R30DrawIndexedPrimitiveUPR29Hook{};

        std::atomic<OutRunVR::RuntimeEligibility::InstallState> R30InstallState{
            OutRunVR::RuntimeEligibility::InstallState::Pending };

        std::uint64_t R30ScreenSpaceFovDraws = 0;
        std::uint64_t R30ScreenSpaceFallbacks = 0;
        std::uint64_t R30ScreenSpaceBuildFailures = 0;
        bool R30FirstScreenSpaceLogged = false;
        bool R30FirstFlatPerspectiveLogged = false;
        bool R30FirstPerspectiveHudLogged = false;

        std::uint64_t R30XyzrhwHudDraws = 0;
        std::uint64_t R30XyzrhwWorldLockedHudDraws = 0;
        std::uint64_t R30XyzrhwWorldEffectDraws = 0;
        std::uint64_t R30Hud2DDraws = 0;
        std::uint64_t R30PerspectiveHudDraws = 0;
        std::uint64_t R30WorldBillboardDraws = 0;
        std::uint64_t R44OverlayOwnedWvpHits = 0;
        std::uint64_t R44OverlayOwnedWvpGroupHits = 0;
        std::uint64_t R44SpatialBillboardClassifications = 0;
        std::uint64_t R44FlatOverlayClassifications = 0;
        std::uint64_t R30XyzrhwRhwWorldPromotions = 0;
        std::uint64_t R30XyzrhwRhwOnlyDepthEvidence = 0;
        std::uint64_t R30XyzrhwZOnlyDepthEvidence = 0;
        std::uint64_t R30XyzrhwFallbacks = 0;
        bool R30FirstXyzrhwHudLogged = false;
        bool R30FirstXyzrhwWorldLogged = false;
        bool R30FirstXyzrhwRhwPromotionLogged = false;
        bool R30FirstXyzrhwRhwOnlyEvidenceLogged = false;
        std::uint64_t R30XyzrhwAtomicFallbacks = 0;
        std::uint64_t R30XyzrhwDepthPreserveFallbacks = 0;
        bool R30FirstXyzrhwAtomicFallbackLogged = false;
        std::uint64_t R30XyzrhwBilateralFallbacks = 0;
        bool R30FirstXyzrhwBilateralFallbackLogged = false;


        // R30.6 CPU shadows for XYZRHW vertex/index buffers.
        //
        // The old VB/IB conversion path called Lock(READONLY) from inside Draw,
        // which can block behind the GPU and also fails for WRITEONLY buffers.
        // Observe the game's own Lock/Unlock writes instead and keep the exact
        // CPU byte ranges that are known current. Draw then consumes only this
        // shadow and fails open to R29 when a range was never observed.
        constexpr std::size_t R30CreateVertexBufferVtableIndex = 26;
        constexpr std::size_t R30CreateIndexBufferVtableIndex = 27;
        constexpr std::size_t R30BufferReleaseVtableIndex = 2;
        constexpr std::size_t R30BufferLockVtableIndex = 11;
        constexpr std::size_t R30BufferUnlockVtableIndex = 12;
        constexpr UINT R30MaxShadowBytes = 16u * 1024u * 1024u;

        SafetyHookInline R30CreateVertexBufferHook{};
        SafetyHookInline R30CreateIndexBufferHook{};
        SafetyHookInline R30VertexBufferReleaseHook{};
        SafetyHookInline R30VertexBufferLockHook{};
        SafetyHookInline R30VertexBufferUnlockHook{};
        SafetyHookInline R30IndexBufferReleaseHook{};
        SafetyHookInline R30IndexBufferLockHook{};
        SafetyHookInline R30IndexBufferUnlockHook{};

        struct R30ByteRange
        {
            UINT begin = 0;
            UINT end = 0; // exclusive
        };

        struct R30BufferShadow
        {
            std::mutex mutex;
            UINT size = 0;
            DWORD usage = 0;
            D3DPOOL pool = D3DPOOL_DEFAULT;
            D3DFORMAT indexFormat = D3DFMT_UNKNOWN;
            std::vector<std::uint8_t> bytes;
            std::vector<R30ByteRange> valid;
            void* lockPtr = nullptr;
            UINT lockOffset = 0;
            UINT lockSize = 0;
            DWORD lockFlags = 0;
            bool writeLock = false;
        };

        using R30ShadowPtr = std::shared_ptr<R30BufferShadow>;
        std::mutex R30ShadowRegistryMutex;
        std::mutex R30ShadowHookMutex;
        std::unordered_map<IDirect3DVertexBuffer9*, R30ShadowPtr>
            R30VertexShadows;
        std::unordered_map<IDirect3DIndexBuffer9*, R30ShadowPtr>
            R30IndexShadows;
        std::uint64_t R30ShadowWrites = 0;
        std::uint64_t R30ShadowReadHits = 0;
        std::uint64_t R30ShadowReadMisses = 0;
        std::uint64_t R30ShadowDiscardInvalidations = 0;
        std::atomic<bool> R30BufferShadowCaptureArmed{ false };
        bool R30FirstShadowMissLogged = false;
        bool R30FirstShadowArmLogged = false;

        struct R30ScratchBuffers
        {
            std::vector<std::uint8_t> source;
            std::vector<std::uint8_t> indexSource;
            std::vector<std::uint8_t> left;
            std::vector<std::uint8_t> right;
            std::vector<std::uint8_t> used;
            std::vector<std::uint32_t> physical;
            std::vector<std::uint16_t> indices16;
            std::vector<std::uint32_t> indices32;
            bool busy = false;
        };
        thread_local R30ScratchBuffers R30Scratch;

        struct R30ScratchLease
        {
            R30ScratchBuffers* buffers = nullptr;
            R30ScratchLease() noexcept
            {
                if (!R30Scratch.busy)
                {
                    R30Scratch.busy = true;
                    buffers = &R30Scratch;
                }
            }
            ~R30ScratchLease()
            {
                if (buffers)
                    buffers->busy = false;
            }
            explicit operator bool() const noexcept { return buffers != nullptr; }
        };

        void R30MergeValidRange(std::vector<R30ByteRange>& ranges,
            UINT begin, UINT end)
        {
            if (begin >= end)
                return;
            R30ByteRange merged{ begin, end };
            auto it = ranges.begin();
            while (it != ranges.end() && it->end < merged.begin)
                ++it;
            while (it != ranges.end() && it->begin <= merged.end)
            {
                merged.begin = std::min(merged.begin, it->begin);
                merged.end = std::max(merged.end, it->end);
                it = ranges.erase(it);
            }
            ranges.insert(it, merged);
        }

        bool R30RangeValid(const std::vector<R30ByteRange>& ranges,
            UINT begin, UINT end) noexcept
        {
            if (begin >= end)
                return false;
            for (const auto& range : ranges)
            {
                if (range.begin <= begin && range.end >= end)
                    return true;
                if (range.begin > begin)
                    break;
            }
            return false;
        }

        R30ShadowPtr R30FindVertexShadow(IDirect3DVertexBuffer9* buffer)
        {
            std::lock_guard<std::mutex> lock(R30ShadowRegistryMutex);
            const auto it = R30VertexShadows.find(buffer);
            return it == R30VertexShadows.end() ? nullptr : it->second;
        }

        R30ShadowPtr R30FindIndexShadow(IDirect3DIndexBuffer9* buffer)
        {
            std::lock_guard<std::mutex> lock(R30ShadowRegistryMutex);
            const auto it = R30IndexShadows.find(buffer);
            return it == R30IndexShadows.end() ? nullptr : it->second;
        }

        R30ShadowPtr R30EnsureVertexShadow(IDirect3DVertexBuffer9* buffer)
        {
            if (!buffer)
                return nullptr;
            if (auto existing = R30FindVertexShadow(buffer))
                return existing;
            D3DVERTEXBUFFER_DESC desc{};
            if (FAILED(buffer->GetDesc(&desc)) || !desc.Size ||
                desc.Size > R30MaxShadowBytes)
                return nullptr;
            auto entry = std::make_shared<R30BufferShadow>();
            entry->size = desc.Size;
            entry->usage = desc.Usage;
            entry->pool = desc.Pool;
            std::lock_guard<std::mutex> lock(R30ShadowRegistryMutex);
            auto [it, inserted] = R30VertexShadows.emplace(buffer, entry);
            return inserted ? entry : it->second;
        }

        R30ShadowPtr R30EnsureIndexShadow(IDirect3DIndexBuffer9* buffer)
        {
            if (!buffer)
                return nullptr;
            if (auto existing = R30FindIndexShadow(buffer))
                return existing;
            D3DINDEXBUFFER_DESC desc{};
            if (FAILED(buffer->GetDesc(&desc)) || !desc.Size ||
                desc.Size > R30MaxShadowBytes ||
                (desc.Format != D3DFMT_INDEX16 &&
                 desc.Format != D3DFMT_INDEX32))
                return nullptr;
            auto entry = std::make_shared<R30BufferShadow>();
            entry->size = desc.Size;
            entry->usage = desc.Usage;
            entry->pool = desc.Pool;
            entry->indexFormat = desc.Format;
            std::lock_guard<std::mutex> lock(R30ShadowRegistryMutex);
            auto [it, inserted] = R30IndexShadows.emplace(buffer, entry);
            return inserted ? entry : it->second;
        }

        void R30BeginObservedLock(const R30ShadowPtr& entry,
            UINT offset, UINT size, void* data, DWORD flags)
        {
            if (!entry || !data)
                return;
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (offset > entry->size)
                return;
            const UINT effectiveSize =
                size == 0 ? entry->size - offset : size;
            if (!effectiveSize || effectiveSize > entry->size - offset)
                return;
            entry->lockPtr = data;
            entry->lockOffset = offset;
            entry->lockSize = effectiveSize;
            entry->lockFlags = flags;
            entry->writeLock = (flags & D3DLOCK_READONLY) == 0;
            if (entry->writeLock && (flags & D3DLOCK_DISCARD))
            {
                entry->valid.clear();
                ++R30ShadowDiscardInvalidations;
            }
        }

        void R30FinishObservedLock(const R30ShadowPtr& entry,
            bool unlockSucceeded)
        {
            if (!entry)
                return;
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (unlockSucceeded && entry->writeLock && entry->lockPtr &&
                entry->lockSize && entry->lockOffset <= entry->size &&
                entry->lockSize <= entry->size - entry->lockOffset)
            {
                try
                {
                    if (entry->bytes.size() != entry->size)
                        entry->bytes.resize(entry->size);
                    std::memcpy(entry->bytes.data() + entry->lockOffset,
                        entry->lockPtr, entry->lockSize);
                    R30MergeValidRange(entry->valid, entry->lockOffset,
                        entry->lockOffset + entry->lockSize);
                    ++R30ShadowWrites;
                }
                catch (...)
                {
                    entry->bytes.clear();
                    entry->valid.clear();
                }
            }
            else if (!unlockSucceeded && entry->writeLock)
            {
                entry->valid.clear();
            }
            entry->lockPtr = nullptr;
            entry->lockOffset = 0;
            entry->lockSize = 0;
            entry->lockFlags = 0;
            entry->writeLock = false;
        }

        bool R30CopyVertexShadow(IDirect3DVertexBuffer9* buffer,
            UINT offset, UINT size, std::vector<std::uint8_t>& out)
        {
            const auto entry = R30FindVertexShadow(buffer);
            if (!entry)
            {
                ++R30ShadowReadMisses;
                return false;
            }
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (offset > entry->size || size > entry->size - offset ||
                entry->bytes.size() != entry->size ||
                !R30RangeValid(entry->valid, offset, offset + size))
            {
                ++R30ShadowReadMisses;
                if (!R30FirstShadowMissLogged)
                {
                    R30FirstShadowMissLogged = true;
                    spdlog::info(
                        "VR R30.6 BUFFER SHADOW: draw-time GPU Lock removed; an unobserved VB/IB range will fail open until the game's next write Lock/Unlock supplies CPU bytes");
                }
                return false;
            }
            try
            {
                out.resize(size);
                std::memcpy(out.data(), entry->bytes.data() + offset, size);
            }
            catch (...)
            {
                return false;
            }
            ++R30ShadowReadHits;
            return true;
        }

        bool R30CopyIndexShadow(IDirect3DIndexBuffer9* buffer,
            UINT offset, UINT size, std::vector<std::uint8_t>& out)
        {
            const auto entry = R30FindIndexShadow(buffer);
            if (!entry)
            {
                ++R30ShadowReadMisses;
                return false;
            }
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (offset > entry->size || size > entry->size - offset ||
                entry->bytes.size() != entry->size ||
                !R30RangeValid(entry->valid, offset, offset + size))
            {
                ++R30ShadowReadMisses;
                return false;
            }
            try
            {
                out.resize(size);
                std::memcpy(out.data(), entry->bytes.data() + offset, size);
            }
            catch (...)
            {
                return false;
            }
            ++R30ShadowReadHits;
            return true;
        }

        HRESULT __stdcall R30VertexBufferLockDest(
            IDirect3DVertexBuffer9* buffer, UINT offset, UINT size,
            void** data, DWORD flags)
        {
            const HRESULT hr = R30VertexBufferLockHook.stdcall<HRESULT>(
                buffer, offset, size, data, flags);
            if (R30BufferShadowCaptureArmed.load(
                    std::memory_order_acquire) &&
                SUCCEEDED(hr) && data && *data)
                R30BeginObservedLock(
                    R30EnsureVertexShadow(buffer), offset, size, *data, flags);
            return hr;
        }

        HRESULT __stdcall R30VertexBufferUnlockDest(
            IDirect3DVertexBuffer9* buffer)
        {
            const auto entry = R30FindVertexShadow(buffer);
            // The pointer returned by Lock is guaranteed valid until Unlock,
            // so snapshot bytes before forwarding the real Unlock.
            if (entry)
                R30FinishObservedLock(entry, true);
            const HRESULT hr =
                R30VertexBufferUnlockHook.stdcall<HRESULT>(buffer);
            if (FAILED(hr) && entry)
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                entry->valid.clear();
            }
            return hr;
        }

        ULONG __stdcall R30VertexBufferReleaseDest(
            IDirect3DVertexBuffer9* buffer)
        {
            const ULONG refs =
                R30VertexBufferReleaseHook.stdcall<ULONG>(buffer);
            if (refs == 0)
            {
                std::lock_guard<std::mutex> lock(R30ShadowRegistryMutex);
                R30VertexShadows.erase(buffer);
            }
            return refs;
        }

        HRESULT __stdcall R30IndexBufferLockDest(
            IDirect3DIndexBuffer9* buffer, UINT offset, UINT size,
            void** data, DWORD flags)
        {
            const HRESULT hr = R30IndexBufferLockHook.stdcall<HRESULT>(
                buffer, offset, size, data, flags);
            if (R30BufferShadowCaptureArmed.load(
                    std::memory_order_acquire) &&
                SUCCEEDED(hr) && data && *data)
                R30BeginObservedLock(
                    R30EnsureIndexShadow(buffer), offset, size, *data, flags);
            return hr;
        }

        HRESULT __stdcall R30IndexBufferUnlockDest(
            IDirect3DIndexBuffer9* buffer)
        {
            const auto entry = R30FindIndexShadow(buffer);
            if (entry)
                R30FinishObservedLock(entry, true);
            const HRESULT hr =
                R30IndexBufferUnlockHook.stdcall<HRESULT>(buffer);
            if (FAILED(hr) && entry)
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                entry->valid.clear();
            }
            return hr;
        }

        ULONG __stdcall R30IndexBufferReleaseDest(
            IDirect3DIndexBuffer9* buffer)
        {
            const ULONG refs =
                R30IndexBufferReleaseHook.stdcall<ULONG>(buffer);
            if (refs == 0)
            {
                std::lock_guard<std::mutex> lock(R30ShadowRegistryMutex);
                R30IndexShadows.erase(buffer);
            }
            return refs;
        }

        bool R30EnsureVertexBufferHooks(IDirect3DVertexBuffer9* buffer)
        {
            if (!buffer)
                return false;
            std::lock_guard<std::mutex> lock(R30ShadowHookMutex);
            auto** vtable = *reinterpret_cast<void***>(buffer);
            if (!R30VertexBufferLockHook)
                R30VertexBufferLockHook = safetyhook::create_inline(
                    vtable[R30BufferLockVtableIndex],
                    R30VertexBufferLockDest);
            if (!R30VertexBufferUnlockHook)
                R30VertexBufferUnlockHook = safetyhook::create_inline(
                    vtable[R30BufferUnlockVtableIndex],
                    R30VertexBufferUnlockDest);
            if (!R30VertexBufferReleaseHook)
                R30VertexBufferReleaseHook = safetyhook::create_inline(
                    vtable[R30BufferReleaseVtableIndex],
                    R30VertexBufferReleaseDest);
            return R30VertexBufferLockHook &&
                R30VertexBufferUnlockHook && R30VertexBufferReleaseHook;
        }

        bool R30EnsureIndexBufferHooks(IDirect3DIndexBuffer9* buffer)
        {
            if (!buffer)
                return false;
            std::lock_guard<std::mutex> lock(R30ShadowHookMutex);
            auto** vtable = *reinterpret_cast<void***>(buffer);
            if (!R30IndexBufferLockHook)
                R30IndexBufferLockHook = safetyhook::create_inline(
                    vtable[R30BufferLockVtableIndex],
                    R30IndexBufferLockDest);
            if (!R30IndexBufferUnlockHook)
                R30IndexBufferUnlockHook = safetyhook::create_inline(
                    vtable[R30BufferUnlockVtableIndex],
                    R30IndexBufferUnlockDest);
            if (!R30IndexBufferReleaseHook)
                R30IndexBufferReleaseHook = safetyhook::create_inline(
                    vtable[R30BufferReleaseVtableIndex],
                    R30IndexBufferReleaseDest);
            return R30IndexBufferLockHook &&
                R30IndexBufferUnlockHook && R30IndexBufferReleaseHook;
        }

        HRESULT __stdcall R30CreateVertexBufferDest(
            IDirect3DDevice9* device, UINT length, DWORD usage, DWORD fvf,
            D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared)
        {
            const HRESULT hr = R30CreateVertexBufferHook.stdcall<HRESULT>(
                device, length, usage, fvf, pool, out, shared);
            if (SUCCEEDED(hr) && out && *out)
            {
                R30EnsureVertexBufferHooks(*out);
                if (R30BufferShadowCaptureArmed.load(
                        std::memory_order_acquire))
                    R30EnsureVertexShadow(*out);
            }
            return hr;
        }

        HRESULT __stdcall R30CreateIndexBufferDest(
            IDirect3DDevice9* device, UINT length, DWORD usage,
            D3DFORMAT format, D3DPOOL pool,
            IDirect3DIndexBuffer9** out, HANDLE* shared)
        {
            const HRESULT hr = R30CreateIndexBufferHook.stdcall<HRESULT>(
                device, length, usage, format, pool, out, shared);
            if (SUCCEEDED(hr) && out && *out)
            {
                R30EnsureIndexBufferHooks(*out);
                if (R30BufferShadowCaptureArmed.load(
                        std::memory_order_acquire))
                    R30EnsureIndexShadow(*out);
            }
            return hr;
        }

        void R30InstallBufferCreationHooks(IDirect3DDevice9* device)
        {
            if (!device)
                return;
            auto** vtable = *reinterpret_cast<void***>(device);
            if (!R30CreateVertexBufferHook)
                R30CreateVertexBufferHook = safetyhook::create_inline(
                    vtable[R30CreateVertexBufferVtableIndex],
                    R30CreateVertexBufferDest);
            if (!R30CreateIndexBufferHook)
                R30CreateIndexBufferHook = safetyhook::create_inline(
                    vtable[R30CreateIndexBufferVtableIndex],
                    R30CreateIndexBufferDest);
            if (!R30CreateVertexBufferHook || !R30CreateIndexBufferHook)
                spdlog::warn(
                    "VR R30.6 BUFFER SHADOW: creation hook incomplete; existing/dynamic buffers still register lazily on draw/Lock");
        }

        void R30RollbackBufferShadowHooks() noexcept
        {
            // Unhook entry points before dropping registry ownership. Any draw
            // holding a COM reference also holds a shared_ptr acquired from the
            // registry, so clearing the maps cannot invalidate an in-flight
            // shadow object.
            R30CreateIndexBufferHook = {};
            R30CreateVertexBufferHook = {};
            R30IndexBufferUnlockHook = {};
            R30IndexBufferLockHook = {};
            R30IndexBufferReleaseHook = {};
            R30VertexBufferUnlockHook = {};
            R30VertexBufferLockHook = {};
            R30VertexBufferReleaseHook = {};
            {
                std::lock_guard<std::mutex> lock(
                    R30ShadowRegistryMutex);
                R30IndexShadows.clear();
                R30VertexShadows.clear();
            }
        }


        // R30.6 stereo sky glow.
        //
        // The original game samples D3DBACKBUFFER_TYPE_MONO and owns only one
        // glow chain. In true stereo that is necessarily the left eye. Keep the
        // original mono post-process disabled and run a completely separate
        // reduced/blurred chain for each completed eye immediately before the
        // renderer composes/publishes the stereo frame.
        SafetyHookInline R30PresentR29Hook{};
        SafetyHookInline R30ResetR29Hook{};

        struct R30SkyGlowResources
        {
            IDirect3DTexture9* reduced[2]{};
            IDirect3DTexture9* temp[2]{};
            IDirect3DPixelShader9* bright = nullptr;
            IDirect3DPixelShader9* blur = nullptr;
            IDirect3DPixelShader9* composite = nullptr;
            UINT eyeWidth = 0;
            UINT eyeHeight = 0;
            UINT glowWidth = 0;
            UINT glowHeight = 0;
            int factor = 0;
        };
        R30SkyGlowResources R30SkyGlow{};
        std::uint64_t R30SkyGlowFrames = 0;
        std::uint64_t R30SkyGlowFailures = 0;
        std::uint64_t R30SkyGlowSceneCaptureEpoch = 0;
        bool R30FirstSkyGlowLogged = false;
        bool R30FirstSkyGlowFailureLogged = false;

        void R30ReleaseSkyGlowResources() noexcept
        {
            for (int eye = 0; eye < 2; ++eye)
            {
                ReleaseCom(R30SkyGlow.reduced[eye]);
                ReleaseCom(R30SkyGlow.temp[eye]);
            }
            ReleaseCom(R30SkyGlow.bright);
            ReleaseCom(R30SkyGlow.blur);
            ReleaseCom(R30SkyGlow.composite);
            R30SkyGlow.eyeWidth = 0;
            R30SkyGlow.eyeHeight = 0;
            R30SkyGlow.glowWidth = 0;
            R30SkyGlow.glowHeight = 0;
            R30SkyGlow.factor = 0;
        }

        bool R30CompilePixelShader(IDirect3DDevice9* device,
            const char* source, const char* entry,
            IDirect3DPixelShader9** shader)
        {
            if (!device || !source || !entry || !shader)
                return false;
            ID3DBlob* bytecode = nullptr;
            ID3DBlob* errors = nullptr;
            const HRESULT compile = D3DCompile(
                source, std::strlen(source),
                "OutRunVR-StereoSkyGlow", nullptr, nullptr,
                entry, "ps_2_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &bytecode, &errors);
            if (FAILED(compile) || !bytecode)
            {
                if (errors && errors->GetBufferPointer())
                    spdlog::warn(
                        "VR SKY GLOW: pixel shader compile failed: {}",
                        static_cast<const char*>(
                            errors->GetBufferPointer()));
                ReleaseCom(errors);
                ReleaseCom(bytecode);
                return false;
            }
            const HRESULT create = device->CreatePixelShader(
                static_cast<const DWORD*>(bytecode->GetBufferPointer()),
                shader);
            ReleaseCom(errors);
            ReleaseCom(bytecode);
            return SUCCEEDED(create) && *shader;
        }

        bool R30EnsureSkyGlowResources(IDirect3DDevice9* device)
        {
            if (!device || !BackBufferDesc.Width || !BackBufferDesc.Height)
                return false;
            const int factor =
                std::clamp(Settings::SkyGlowFactor.get(), 1, 16);
            const UINT glowWidth = std::max<UINT>(
                160u, BackBufferDesc.Width /
                    static_cast<UINT>(factor));
            const UINT glowHeight = std::max<UINT>(
                120u, BackBufferDesc.Height /
                    static_cast<UINT>(factor));

            if (R30SkyGlow.reduced[0] && R30SkyGlow.reduced[1] &&
                R30SkyGlow.temp[0] && R30SkyGlow.temp[1] &&
                R30SkyGlow.bright && R30SkyGlow.blur &&
                R30SkyGlow.composite &&
                R30SkyGlow.eyeWidth == BackBufferDesc.Width &&
                R30SkyGlow.eyeHeight == BackBufferDesc.Height &&
                R30SkyGlow.glowWidth == glowWidth &&
                R30SkyGlow.glowHeight == glowHeight &&
                R30SkyGlow.factor == factor)
                return true;

            R30ReleaseSkyGlowResources();

            for (int eye = 0; eye < 2; ++eye)
            {
                if (FAILED(device->CreateTexture(
                        glowWidth, glowHeight, 1,
                        D3DUSAGE_RENDERTARGET,
                        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                        &R30SkyGlow.reduced[eye], nullptr)) ||
                    FAILED(device->CreateTexture(
                        glowWidth, glowHeight, 1,
                        D3DUSAGE_RENDERTARGET,
                        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                        &R30SkyGlow.temp[eye], nullptr)))
                {
                    R30ReleaseSkyGlowResources();
                    return false;
                }
            }

            static constexpr char BrightPs[] = R"(
                sampler Scene : register(s0);
                float4 Bright(float2 uv : TEXCOORD0) : COLOR0
                {
                    float4 c = tex2D(Scene, uv);
                    float peak = max(c.r, max(c.g, c.b));
                    float bright = saturate((peak - 0.58) * 2.75);
                    // World HDR/exposure draws normally leave useful alpha below
                    // one, while most fixed HUD art is opaque. This is not used
                    // for eye identity; it only suppresses UI contamination.
                    float worldMask = max(0.15,
                        saturate((1.0 - c.a) * 2.0));
                    float m = bright * worldMask;
                    return float4(c.rgb * m, m);
                })";
            static constexpr char BlurPs[] = R"(
                sampler Source : register(s0);
                float4 Texel : register(c0);
                float4 Blur(float2 uv : TEXCOORD0) : COLOR0
                {
                    float2 d = Texel.xy;
                    float4 c = tex2D(Source, uv) * 0.40;
                    c += tex2D(Source, uv + d * 1.5) * 0.24;
                    c += tex2D(Source, uv - d * 1.5) * 0.24;
                    c += tex2D(Source, uv + d * 3.5) * 0.06;
                    c += tex2D(Source, uv - d * 3.5) * 0.06;
                    return c;
                })";
            static constexpr char CompositePs[] = R"(
                sampler Glow : register(s0);
                float4 Params : register(c0);
                float4 Composite(float2 uv : TEXCOORD0) : COLOR0
                {
                    float4 g = tex2D(Glow, uv);
                    return float4(g.rgb * Params.x, 0.0);
                })";

            if (!R30CompilePixelShader(
                    device, BrightPs, "Bright",
                    &R30SkyGlow.bright) ||
                !R30CompilePixelShader(
                    device, BlurPs, "Blur",
                    &R30SkyGlow.blur) ||
                !R30CompilePixelShader(
                    device, CompositePs, "Composite",
                    &R30SkyGlow.composite))
            {
                R30ReleaseSkyGlowResources();
                return false;
            }

            R30SkyGlow.eyeWidth = BackBufferDesc.Width;
            R30SkyGlow.eyeHeight = BackBufferDesc.Height;
            R30SkyGlow.glowWidth = glowWidth;
            R30SkyGlow.glowHeight = glowHeight;
            R30SkyGlow.factor = factor;
            return true;
        }

        struct R30GlowVertex
        {
            float x, y, z, rhw;
            float u, v;
        };

        bool R30DrawSkyGlowPass(IDirect3DDevice9* device,
            IDirect3DSurface9* target, UINT width, UINT height,
            IDirect3DTexture9* source,
            IDirect3DPixelShader9* shader,
            const float constant[4],
            bool additive)
        {
            if (!device || !target || !source || !shader ||
                !width || !height)
                return false;

            if (FAILED(device->SetRenderTarget(0, target)) ||
                FAILED(device->SetDepthStencilSurface(nullptr)))
                return false;

            D3DVIEWPORT9 viewport{};
            viewport.Width = width;
            viewport.Height = height;
            viewport.MinZ = 0.0f;
            viewport.MaxZ = 1.0f;
            if (FAILED(device->SetViewport(&viewport)))
                return false;

            const R30GlowVertex v[4]{
                { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
                { static_cast<float>(width) - 0.5f, -0.5f,
                    0.0f, 1.0f, 1.0f, 0.0f },
                { -0.5f, static_cast<float>(height) - 0.5f,
                    0.0f, 1.0f, 0.0f, 1.0f },
                { static_cast<float>(width) - 0.5f,
                    static_cast<float>(height) - 0.5f,
                    0.0f, 1.0f, 1.0f, 1.0f }
            };

            device->SetVertexShader(nullptr);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
            device->SetPixelShader(shader);
            device->SetTexture(0, source);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE,
                additive ? TRUE : FALSE);
            if (additive)
            {
                device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
                device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
                device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
                device->SetRenderState(D3DRS_COLORWRITEENABLE,
                    D3DCOLORWRITEENABLE_RED |
                    D3DCOLORWRITEENABLE_GREEN |
                    D3DCOLORWRITEENABLE_BLUE);
            }
            else
            {
                device->SetRenderState(D3DRS_COLORWRITEENABLE,
                    D3DCOLORWRITEENABLE_RED |
                    D3DCOLORWRITEENABLE_GREEN |
                    D3DCOLORWRITEENABLE_BLUE |
                    D3DCOLORWRITEENABLE_ALPHA);
            }
            device->SetPixelShaderConstantF(0, constant, 1);
            const HRESULT hr = device->DrawPrimitiveUP(
                D3DPT_TRIANGLESTRIP, 2, v, sizeof(R30GlowVertex));
            device->SetTexture(0, nullptr);
            return SUCCEEDED(hr);
        }

        bool R30CaptureSkyGlowSceneBeforeHud(
            IDirect3DDevice9* device)
        {
            if (!device || Settings::SkyGlowFactor <= 0 ||
                !FrameHadWorldStereo || !FrameHadDuplicatedDraw ||
                FrameRightDrawFailed || FrameStereoIncomplete ||
                !BackBuffer || !RightEyeSurface)
                return false;

            if (R30SkyGlowSceneCaptureEpoch == PresentEpoch)
                return true;
            if (!R30EnsureSkyGlowResources(device))
                return false;

            IDirect3DSurface9* eyeSurface[2]{
                BackBuffer, RightEyeSurface
            };
            bool ok = true;
            for (int eye = 0; eye < 2 && ok; ++eye)
            {
                IDirect3DSurface9* reduced = nullptr;
                if (FAILED(R30SkyGlow.reduced[eye]->GetSurfaceLevel(
                        0, &reduced)) || !reduced)
                {
                    ok = false;
                    break;
                }
                ok = SUCCEEDED(device->StretchRect(
                    eyeSurface[eye], nullptr, reduced, nullptr,
                    D3DTEXF_LINEAR));
                reduced->Release();
            }
            if (ok)
                R30SkyGlowSceneCaptureEpoch = PresentEpoch;
            return ok;
        }

        bool R30ApplyStereoSkyGlow(IDirect3DDevice9* device)
        {
            if (!device || Settings::SkyGlowFactor <= 0 ||
                !StereoWanted() || !FrameHadWorldStereo ||
                !FrameHadDuplicatedDraw || FrameRightDrawFailed ||
                FrameStereoIncomplete || !BackBuffer ||
                !RightEyeSurface)
                return true;

            if (!R30EnsureSkyGlowResources(device))
                return false;

            IDirect3DStateBlock9* stateBlock = nullptr;
            IDirect3DSurface9* savedRt = nullptr;
            IDirect3DSurface9* savedDepth = nullptr;
            D3DVIEWPORT9 savedViewport{};
            if (FAILED(device->CreateStateBlock(
                    D3DSBT_ALL, &stateBlock)) ||
                !stateBlock ||
                FAILED(device->GetRenderTarget(0, &savedRt)) ||
                !savedRt ||
                FAILED(device->GetViewport(&savedViewport)))
            {
                if (savedRt) savedRt->Release();
                if (stateBlock) stateBlock->Release();
                return false;
            }
            const HRESULT depthHr =
                device->GetDepthStencilSurface(&savedDepth);
            const bool depthOk =
                SUCCEEDED(depthHr) || depthHr == D3DERR_NOTFOUND;
            if (!depthOk)
            {
                savedRt->Release();
                stateBlock->Release();
                return false;
            }

            bool ok = true;
            IDirect3DSurface9* eyeSurface[2]{
                BackBuffer, RightEyeSurface
            };
            for (int eye = 0; eye < 2 && ok; ++eye)
            {
                IDirect3DSurface9* reduced = nullptr;
                IDirect3DSurface9* temp = nullptr;
                if (FAILED(R30SkyGlow.reduced[eye]->GetSurfaceLevel(
                        0, &reduced)) || !reduced ||
                    FAILED(R30SkyGlow.temp[eye]->GetSurfaceLevel(
                        0, &temp)) || !temp)
                {
                    if (temp) temp->Release();
                    if (reduced) reduced->Release();
                    ok = false;
                    break;
                }

                if (R30SkyGlowSceneCaptureEpoch != PresentEpoch)
                {
                    ok = SUCCEEDED(device->StretchRect(
                        eyeSurface[eye], nullptr, reduced, nullptr,
                        D3DTEXF_LINEAR));
                }
                const float zero[4]{ 0, 0, 0, 0 };
                if (ok)
                {
                    // Bright pass overwrites the entire reduced target, so no
                    // Clear is needed. Clearing here before rebinding the RT
                    // would clear the game's current eye backbuffer.
                    ok = R30DrawSkyGlowPass(
                        device, temp,
                        R30SkyGlow.glowWidth,
                        R30SkyGlow.glowHeight,
                        R30SkyGlow.reduced[eye],
                        R30SkyGlow.bright, zero, false);
                }

                const float horizontal[4]{
                    1.0f /
                        static_cast<float>(R30SkyGlow.glowWidth),
                    0.0f, 0.0f, 0.0f
                };
                if (ok)
                    ok = R30DrawSkyGlowPass(
                        device, reduced,
                        R30SkyGlow.glowWidth,
                        R30SkyGlow.glowHeight,
                        R30SkyGlow.temp[eye],
                        R30SkyGlow.blur, horizontal, false);

                // TwoStep exists to reduce aliasing after a downsample.
                // At factor=1 there is no downsample, so a second full-resolution
                // blur only doubles bandwidth. Also make sure the vertical pass,
                // when requested for factor>1, is actually the composite source.
                const bool effectiveTwoStep =
                    Settings::SkyGlowTwoStep.get() &&
                    R30SkyGlow.factor > 1;
                IDirect3DTexture9* compositeSource =
                    R30SkyGlow.temp[eye];
                if (effectiveTwoStep)
                {
                    const float vertical[4]{
                        0.0f,
                        1.0f /
                            static_cast<float>(R30SkyGlow.glowHeight),
                        0.0f, 0.0f
                    };
                    if (ok)
                        ok = R30DrawSkyGlowPass(
                            device, temp,
                            R30SkyGlow.glowWidth,
                            R30SkyGlow.glowHeight,
                            R30SkyGlow.reduced[eye],
                            R30SkyGlow.blur, vertical, false);
                    if (ok)
                        compositeSource = R30SkyGlow.reduced[eye];
                }

                const float composite[4]{ 0.38f, 0, 0, 0 };
                if (ok)
                    ok = R30DrawSkyGlowPass(
                        device, eyeSurface[eye],
                        BackBufferDesc.Width,
                        BackBufferDesc.Height,
                        compositeSource,
                        R30SkyGlow.composite, composite, true);

                temp->Release();
                reduced->Release();
            }

            // Apply the captured pipeline state first, then explicitly restore
            // RT/depth/viewport last. This guarantees the game bindings win even
            // if a driver/state-block implementation restores more state than
            // the code path historically relied on.
            bool restoreOk = SUCCEEDED(stateBlock->Apply());
            restoreOk =
                SUCCEEDED(device->SetRenderTarget(0, savedRt)) &&
                restoreOk;
            const HRESULT restoreDepth =
                device->SetDepthStencilSurface(savedDepth);
            restoreOk =
                (SUCCEEDED(restoreDepth) ||
                 (!savedDepth && restoreDepth == D3D_OK)) &&
                restoreOk;
            restoreOk =
                SUCCEEDED(device->SetViewport(&savedViewport)) &&
                restoreOk;
            savedRt->Release();
            if (savedDepth) savedDepth->Release();
            stateBlock->Release();

            if (ok && restoreOk)
            {
                ++R30SkyGlowFrames;
                if (!R30FirstSkyGlowLogged)
                {
                    R30FirstSkyGlowLogged = true;
                    spdlog::info(
                        "VR SKY GLOW: independent L/R extract + stereo blur + additive composite ACTIVE factor={} requestedTwoStep={} effectiveTwoStep={} buffer={}x{}",
                        R30SkyGlow.factor,
                        Settings::SkyGlowTwoStep.get() ? 1 : 0,
                        (Settings::SkyGlowTwoStep.get() && R30SkyGlow.factor > 1) ? 1 : 0,
                        R30SkyGlow.glowWidth,
                        R30SkyGlow.glowHeight);
                }
                return true;
            }

            ++R30SkyGlowFailures;
            if (!R30FirstSkyGlowFailureLogged)
            {
                R30FirstSkyGlowFailureLogged = true;
                spdlog::warn(
                    "VR SKY GLOW: stereo eye post-process failed; frame continues without enabling the stock mono glow chain");
            }
            return false;
        }

        ULONGLONG R30LastTelemetryMs = 0;

        void R30MaybeLogTelemetry()
        {
            if (!Settings::VRTelemetry)
                return;
            const ULONGLONG now = GetTickCount64();
            if (now - R30LastTelemetryMs < 5000)
                return;
            R30LastTelemetryMs = now;
            spdlog::info(
                "VR R41: bufferShadow[armed={},writes={},hits={},misses={},discardInvalid={},drawReadLocks=0] xyzrhw[world={},hud={},hudWorldLock={},rhwPromote={},rhwOnlyDepth={},zOnlyDepth={},atomicFallback={},depthPreserve={},bilateralFallback={}] screen[all={},hud2d={},perspectiveHud={},worldBillboard={}] r44[ownedWvp={},groupReuse={},spatial={},flat={}] skyGlow[frames={},failures={},factor={},buffer={}x{}]",
                R30BufferShadowCaptureArmed.load(std::memory_order_acquire) ? 1 : 0,
                R30ShadowWrites, R30ShadowReadHits, R30ShadowReadMisses,
                R30ShadowDiscardInvalidations,
                R30XyzrhwWorldEffectDraws, R30XyzrhwHudDraws,
                R30XyzrhwWorldLockedHudDraws,
                R30XyzrhwRhwWorldPromotions,
                R30XyzrhwRhwOnlyDepthEvidence,
                R30XyzrhwZOnlyDepthEvidence,
                R30XyzrhwAtomicFallbacks,
                R30XyzrhwDepthPreserveFallbacks,
                R30XyzrhwBilateralFallbacks,
                R30ScreenSpaceFovDraws, R30Hud2DDraws,
                R30PerspectiveHudDraws, R30WorldBillboardDraws,
                R44OverlayOwnedWvpHits, R44OverlayOwnedWvpGroupHits,
                R44SpatialBillboardClassifications,
                R44FlatOverlayClassifications,
                R30SkyGlowFrames, R30SkyGlowFailures,
                R30SkyGlow.factor, R30SkyGlow.glowWidth,
                R30SkyGlow.glowHeight);
        }

        HRESULT __stdcall PresentDestR30(
            IDirect3DDevice9* device, const RECT* sourceRect,
            const RECT* destRect, HWND destWindowOverride,
            const RGNDATA* dirtyRegion)
        {
            R30MaybeLogTelemetry();
            if (Settings::SkyGlowFactor > 0 &&
                StereoWanted() && FrameHadWorldStereo &&
                FrameHadDuplicatedDraw &&
                !FrameRightDrawFailed && !FrameStereoIncomplete)
            {
                InternalPassScope guard;
                R30ApplyStereoSkyGlow(device);
            }
            return R30PresentR29Hook.stdcall<HRESULT>(
                device, sourceRect, destRect,
                destWindowOverride, dirtyRegion);
        }

        HRESULT __stdcall ResetDestR30(
            IDirect3DDevice9* device,
            D3DPRESENT_PARAMETERS* params)
        {
            R30ReleaseSkyGlowResources();
            R30SkyGlowSceneCaptureEpoch = 0;
            R30BufferShadowCaptureArmed.store(
                false, std::memory_order_release);
            return R30ResetR29Hook.stdcall<HRESULT>(device, params);
        }

        // User-adjustable projection-space HUD scale. The per-eye FOV affine
        // remains automatic; this value is only a common-centre size trim after
        // the headset-specific mapping.
        float R30HudScaleValue() noexcept
        {
            return std::clamp(Settings::VRHudScale.get(), 0.30f, 1.20f);
        }

        float R30HudAspectCompensation(
            const OutRunVRRenderer::LatchedStereoFrame& stereo) noexcept
        {
            if (!BackBufferDesc.Width || !BackBufferDesc.Height)
                return 1.0f;

            const float sourceAspect =
                static_cast<float>(BackBufferDesc.Width) /
                static_cast<float>(BackBufferDesc.Height);

            // R35.4: SharedPoseState already carries the runtime's actual
            // recommended per-eye render-target size. That pixel aspect is the
            // quantity that finally stretches a screen-space HUD sprite in the
            // OpenXR projection swapchain. Using angular FOV aspect was close,
            // but on Quest/VDXR it leaves circular HUD elements slightly tall.
            float targetAspectSum = 0.0f;
            int targetEyes = 0;
            if (SharedState &&
                SharedState->magic == OutRunVR::SharedMagic &&
                SharedState->protocolVersion ==
                    OutRunVR::SharedProtocolVersion)
            {
                for (int eye = 0; eye < 2; ++eye)
                {
                    const std::uint32_t w =
                        SharedState->recommendedWidth[eye];
                    const std::uint32_t h =
                        SharedState->recommendedHeight[eye];
                    if (!w || !h)
                        continue;
                    const float aspect =
                        static_cast<float>(w) /
                        static_cast<float>(h);
                    if (!std::isfinite(aspect) ||
                        aspect < 0.25f || aspect > 4.0f)
                        continue;
                    targetAspectSum += aspect;
                    ++targetEyes;
                }
            }

            // Fall back to the old angular estimate only if the host has not
            // published swapchain dimensions yet (startup/recovery).
            if (!targetEyes)
            {
                for (int eye = 0; eye < 2; ++eye)
                {
                    const float left =
                        std::tan(stereo.eyeFov[eye].angleLeft);
                    const float right =
                        std::tan(stereo.eyeFov[eye].angleRight);
                    const float up =
                        std::tan(stereo.eyeFov[eye].angleUp);
                    const float down =
                        std::tan(stereo.eyeFov[eye].angleDown);
                    const float horizontal = right - left;
                    const float vertical = up - down;
                    if (!std::isfinite(horizontal) ||
                        !std::isfinite(vertical) ||
                        horizontal <= 0.05f || vertical <= 0.05f)
                        continue;
                    const float aspect = horizontal / vertical;
                    if (!std::isfinite(aspect) ||
                        aspect < 0.25f || aspect > 4.0f)
                        continue;
                    targetAspectSum += aspect;
                    ++targetEyes;
                }
            }

            if (!targetEyes)
                return 1.0f;

            const float targetAspect =
                targetAspectSum / static_cast<float>(targetEyes);
            const float compensation = sourceAspect / targetAspect;
            if (!std::isfinite(compensation))
                return 1.0f;
            return std::clamp(compensation, 0.75f, 3.50f);
        }

        void R30HudContainScale(
            const OutRunVRRenderer::LatchedStereoFrame& stereo,
            float& scaleX, float& scaleY) noexcept
        {
            (void)stereo;
            const float userScale = R30HudScaleValue();

            // R43: the finite HUD plane is reconstructed through the game's
            // base projection and then through each OpenXR eye projection.
            // Applying sourceAspect/eyeAspect again here was a second,
            // anisotropic aspect correction. On this run sourceOverTarget=2.569,
            // so HudScale=0.55 became X=0.55/Y=0.214 and visibly squashed the
            // complete HUD. Keep HUD scale uniform; projection handles aspect.
            scaleX = userScale;
            scaleY = userScale;
        }

        enum class R30ScreenSpaceKind : std::uint8_t
        {
            None,
            Hud2D,
            PerspectiveHud,
            WorldBillboard
        };

        constexpr std::uint64_t R44OverlayWvpDrawWindow = 12u;

        bool R44GetOwnedRawOverlayWvp(
            float outConstants[16], std::uint64_t* drawAge = nullptr) noexcept
        {
            if (!outConstants ||
                CurrentVertexShaderIdentity.load(std::memory_order_acquire) == 0)
                return false;

            std::uint64_t writeSerial = 0;
            std::uint64_t writeDrawSerial = 0;
            std::uintptr_t writeShader = 0;
            std::uint64_t writeShaderSerial = 0;
            std::uintptr_t currentShader = 0;
            std::uint64_t currentShaderSerial = 0;
            if (!OutRunVRRenderer::GetLastRawGameWvpWrite(
                    outConstants, writeSerial, writeDrawSerial,
                    writeShader, writeShaderSerial) ||
                !GetCurrentShaderEpoch(currentShader, currentShaderSerial) ||
                writeShader != currentShader ||
                writeShaderSerial != currentShaderSerial)
                return false;

            // R45: R30 is the outer draw hook and R23 increments its top-level
            // serial only after this classifier calls down into R26/R23. R44
            // compared against R23GameDrawSerial directly, so the immediate
            // draw after a c64 upload had age=0 and every owner test failed.
            // At this boundary the draw being classified is serial + 1.
            const std::uint64_t currentOuterDrawSerial =
                R23GameDrawSerial + 1u;
            if (currentOuterDrawSerial <= writeDrawSerial)
                return false;
            const std::uint64_t age =
                currentOuterDrawSerial - writeDrawSerial;
            if (age == 0 || age > R44OverlayWvpDrawWindow)
                return false;

            ++R44OverlayOwnedWvpHits;
            if (age > 1)
                ++R44OverlayOwnedWvpGroupHits;
            if (drawAge)
                *drawAge = age;
            return true;
        }

        enum class R44OverlayMatrixKind : std::uint8_t
        {
            Unknown,
            FlatHud,
            SpatialBillboard
        };

        R44OverlayMatrixKind R44ClassifyOwnedOverlayMatrix(
            float rawWvp[16]) noexcept
        {
            if (!rawWvp)
                return R44OverlayMatrixKind::Unknown;

            float projectionRaw[16]{};
            if (!OutRunVRRenderer::GetRendererBaseProjection(projectionRaw))
                return R44OverlayMatrixKind::Unknown;

            D3DMATRIX uploadedT{};
            D3DMATRIX baseProjection{};
            D3DMATRIX inverseProjection{};
            std::memcpy(&uploadedT, rawWvp, sizeof(uploadedT));
            std::memcpy(&baseProjection, projectionRaw, sizeof(baseProjection));
            const D3DMATRIX stockWvp = TransposeMatrix(uploadedT);
            if (!MatrixFinite(stockWvp) || !MatrixFinite(baseProjection) ||
                !InvertMatrix(baseProjection, inverseProjection))
                return R44OverlayMatrixKind::Unknown;

            const D3DMATRIX worldView =
                MultiplyMatrix(stockWvp, inverseProjection);
            if (!MatrixFinite(worldView))
                return R44OverlayMatrixKind::Unknown;

            const float affineError =
                std::fabs(worldView._14) + std::fabs(worldView._24) +
                std::fabs(worldView._34) + std::fabs(worldView._44 - 1.0f);
            const float viewDepth = std::fabs(worldView._43);
            const float clipW =
                worldView._43 * baseProjection._34 + baseProjection._44;
            if (!std::isfinite(affineError) || !std::isfinite(viewDepth) ||
                !std::isfinite(clipW) || clipW <= 0.05f)
                return R44OverlayMatrixKind::Unknown;

            // R44: OutRun often uploads one c64 then emits several glyph/sprite
            // draws with the same shader. A spatial vehicle marker remains an
            // affine WorldView at real scene depth. Results/time-record text is
            // a near/screen overlay and must converge on the HUD plane instead
            // of falling through to R13 zero-disparity replay.
            if (affineError <= 0.02f &&
                viewDepth >= 1.25f && viewDepth < 1000000.0f)
                return R44OverlayMatrixKind::SpatialBillboard;
            if (viewDepth < 1.25f || affineError > 0.02f)
                return R44OverlayMatrixKind::FlatHud;
            return R44OverlayMatrixKind::Unknown;
        }

        R30ScreenSpaceKind R30ClassifyScreenSpacePass(
            IDirect3DDevice9* device) noexcept
        {
            if (!device || !TargetIsBackBuffer())
                return R30ScreenSpaceKind::None;

            float projection[16]{};
            if (!OutRunVRRenderer::GetRendererBaseProjection(projection))
                return R30ScreenSpaceKind::None;

            const auto projectionClass =
                OutRunVR::PassPolicy::ClassifyProjectionSignature(
                    projection[11], projection[15]);
            if (projectionClass ==
                OutRunVR::PassPolicy::ProjectionClass::Orthographic2D)
                return R30ScreenSpaceKind::Hud2D;
            if (projectionClass !=
                OutRunVR::PassPolicy::ProjectionClass::Perspective3D)
                return R30ScreenSpaceKind::None;

            // Verified/rebindable perspective WVP is already owned by the true
            // world path and must never be intercepted as UI.
            if (CurrentDrawMatchesVerifiedWorld(device))
                return R30ScreenSpaceKind::None;
            std::uintptr_t reboundShader = 0;
            std::uint64_t reboundSerial = 0;
            if (R28CanRebindVerifiedWorld(
                    device, reboundShader, reboundSerial))
                return R30ScreenSpaceKind::None;

            DWORD zEnable = D3DZB_TRUE;
            DWORD zWrite = TRUE;
            DWORD alphaBlend = FALSE;
            DWORD alphaTest = FALSE;
            DWORD cullMode = D3DCULL_CCW;
            if (!ReadTrackedRenderState(device, D3DRS_ZENABLE, zEnable) ||
                !ReadTrackedRenderState(device, D3DRS_ZWRITEENABLE, zWrite) ||
                !ReadTrackedRenderState(
                    device, D3DRS_ALPHABLENDENABLE, alphaBlend) ||
                !ReadTrackedRenderState(
                    device, D3DRS_ALPHATESTENABLE, alphaTest) ||
                !ReadTrackedRenderState(device, D3DRS_CULLMODE, cullMode))
                return R30ScreenSpaceKind::None;

            const bool alphaLike =
                alphaBlend != FALSE || alphaTest != FALSE;
            if (!alphaLike)
                return R30ScreenSpaceKind::None;

            float rawOwnedWvp[16]{};
            const bool haveOwnedWvp =
                R44GetOwnedRawOverlayWvp(rawOwnedWvp);
            if (haveOwnedWvp)
            {
                const auto matrixKind =
                    R44ClassifyOwnedOverlayMatrix(rawOwnedWvp);
                if (matrixKind == R44OverlayMatrixKind::SpatialBillboard)
                {
                    ++R44SpatialBillboardClassifications;
                    return R30ScreenSpaceKind::WorldBillboard;
                }
                if (matrixKind == R44OverlayMatrixKind::FlatHud)
                {
                    ++R44FlatOverlayClassifications;
                    return R30ScreenSpaceKind::PerspectiveHud;
                }
            }

            // Keep R13's fragile-effect shape as an explicit fallback signal.
            // If the matrix owner is unavailable, depth-disabled/two-sided alpha
            // is still a flat overlay. Routing it through the finite HUD plane
            // prevents two visible zero-disparity copies for result text while
            // preserving depth-tested unknown effects fail-closed.
            const bool r13FlatEffectShape =
                cullMode == D3DCULL_NONE &&
                zEnable == D3DZB_FALSE &&
                ((alphaBlend != FALSE && zWrite == FALSE) ||
                 alphaTest != FALSE);
            if (r13FlatEffectShape)
            {
                ++R44FlatOverlayClassifications;
                return R30ScreenSpaceKind::PerspectiveHud;
            }

            if (zEnable != D3DZB_FALSE)
                return R30ScreenSpaceKind::None;
            return R30ScreenSpaceKind::PerspectiveHud;
        }


        bool R30BuildEyeAffine(
            const OutRunVRRenderer::LatchedStereoFrame& stereo,
            float eyeScale[2], float eyeOffset[2]) noexcept
        {
            float tanLeft[2]{};
            float tanRight[2]{};
            float width[2]{};
            for (int eye = 0; eye < 2; ++eye)
            {
                tanLeft[eye] = std::tan(stereo.eyeFov[eye].angleLeft);
                tanRight[eye] = std::tan(stereo.eyeFov[eye].angleRight);
                width[eye] = tanRight[eye] - tanLeft[eye];
                if (!std::isfinite(tanLeft[eye]) ||
                    !std::isfinite(tanRight[eye]) ||
                    !std::isfinite(width[eye]) || width[eye] <= 0.05f)
                    return false;
            }

            const float commonLeft = 0.5f * (tanLeft[0] + tanLeft[1]);
            const float commonRight = 0.5f * (tanRight[0] + tanRight[1]);
            const float commonWidth = commonRight - commonLeft;
            const float commonSum = commonRight + commonLeft;
            if (!std::isfinite(commonWidth) || commonWidth <= 0.05f)
                return false;

            for (int eye = 0; eye < 2; ++eye)
            {
                const float eyeSum = tanRight[eye] + tanLeft[eye];
                const float scale = commonWidth / width[eye];
                const float offset = (commonSum - eyeSum) / width[eye];
                if (!std::isfinite(scale) || !std::isfinite(offset) ||
                    scale < 0.50f || scale > 1.50f ||
                    std::fabs(offset) > 0.50f)
                    return false;
                eyeScale[eye] = scale;
                eyeOffset[eye] = offset;
            }
            return true;
        }

        struct R30XyzrhwState
        {
            OutRunVRRenderer::LatchedStereoFrame stereo{};
            D3DVIEWPORT9 viewport{};
            float eyeScale[2]{};
            float eyeOffset[2]{};
            float worldScaleX[2]{};
            float worldOffsetX[2]{};
            float worldScaleY[2]{};
            float worldOffsetY[2]{};
            float parallaxPerRhwX[2]{};
            float parallaxPerRhwY[2]{};
            D3DMATRIX baseProjection{};
            D3DMATRIX inverseBaseProjection{};
            D3DMATRIX eyeProjection[2]{};
            D3DMATRIX eyeInverse[2]{};
            D3DMATRIX hudViewProjection[2]{};
            float hudClipX[2][3]{};
            float hudClipY[2][3]{};
            float hudClipW[2][3]{};
            bool hudWorldLockValid = false;
            bool fullWorldReprojection = false;
            bool depthTestEnabled = false;
            bool rhwDepthEvidence = false;
            bool worldEffect = false;
        };

        bool R30PrepareXyzrhwState(
            IDirect3DDevice9* device, R30XyzrhwState& state) noexcept
        {
            if (!R30SafeStereoBase(device) ||
                CurrentVertexShaderIdentity.load(std::memory_order_acquire) != 0)
                return false;

            IDirect3DVertexShader9* shader = nullptr;
            if (FAILED(device->GetVertexShader(&shader)))
                return false;
            if (shader)
            {
                shader->Release();
                return false;
            }

            DWORD fvf = 0;
            if (FAILED(device->GetFVF(&fvf)) ||
                (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW)
                return false;

            if (!EnsureStereoResources(device) ||
                FAILED(device->GetViewport(&state.viewport)) ||
                state.viewport.Width == 0 || state.viewport.Height == 0 ||
                !std::isfinite(state.viewport.MinZ) ||
                !std::isfinite(state.viewport.MaxZ) ||
                state.viewport.MaxZ <= state.viewport.MinZ)
                return false;

            DWORD zEnable = D3DZB_FALSE;
            if (!ReadTrackedRenderState(device, D3DRS_ZENABLE, zEnable))
                return false;
            state.depthTestEnabled = zEnable != D3DZB_FALSE;

            if (!OutRunVRRenderer::GetLatchedStereoFrame(state.stereo) ||
                state.stereo.poseSequence == 0 ||
                (FrameStereoPoseSequence != 0 &&
                 FrameStereoPoseSequence != state.stereo.poseSequence) ||
                !R30BuildEyeAffine(state.stereo,
                    state.eyeScale, state.eyeOffset))
                return false;

            return true;
        }

        bool R30XyzrhwHasProjectedDepthSignature(
            const void* source, UINT vertexCount, UINT stride,
            const R30XyzrhwState& state, const D3DMATRIX& projection,
            const std::vector<std::uint8_t>* usedMask) noexcept
        {
            if (!source || vertexCount == 0 || stride < sizeof(float) * 4)
                return false;

            const float depthSpan =
                state.viewport.MaxZ - state.viewport.MinZ;
            if (!std::isfinite(depthSpan) || depthSpan <= 1.0e-6f ||
                std::fabs(projection._34) <= 1.0e-5f ||
                std::fabs(projection._13) > 1.0e-4f ||
                std::fabs(projection._23) > 1.0e-4f)
                return false;

            UINT sampled = 0;
            UINT valid = 0;
            UINT projected = 0;
            UINT nonUnitRhw = 0;
            UINT plausibleRhwDepth = 0;
            UINT plausibleScreenZDepth = 0;
            for (UINT i = 0; i < vertexCount && sampled < 512u; ++i)
            {
                if (usedMask &&
                    (i >= usedMask->size() || (*usedMask)[i] == 0))
                    continue;
                ++sampled;
                const float* p = reinterpret_cast<const float*>(
                    static_cast<const std::uint8_t*>(source) +
                    static_cast<std::size_t>(i) * stride);
                const float z = p[2];
                const float rhw = p[3];
                if (!std::isfinite(z) || !std::isfinite(rhw) ||
                    rhw <= 1.0e-6f || rhw >= 1000.0f)
                    continue;

                const float clipW = 1.0f / rhw;
                const float viewZ =
                    (clipW - projection._44) / projection._34;
                const float clipZ =
                    viewZ * projection._33 + projection._43;
                const float expectedNdcZ = clipZ / clipW;
                const float actualNdcZ =
                    (z - state.viewport.MinZ) / depthSpan;
                if (!std::isfinite(expectedNdcZ) ||
                    !std::isfinite(actualNdcZ))
                    continue;

                ++valid;

                // RHW is often forced to 1.0 by OutRun's pretransformed
                // particle/marker path. Screen-Z still contains the perspective
                // depth. Solve the projection equation directly and accept
                // interior Z only when it reconstructs to a real world distance.
                const float zDenominator =
                    actualNdcZ * projection._34 - projection._33;
                if (actualNdcZ > 1.0e-5f &&
                    actualNdcZ < 0.99999f &&
                    std::fabs(zDenominator) > 1.0e-6f)
                {
                    const float zOnlyViewZ =
                        (projection._43 -
                         actualNdcZ * projection._44) /
                        zDenominator;
                    if (std::isfinite(zOnlyViewZ) &&
                        std::fabs(zOnlyViewZ) >= 0.75f &&
                        std::fabs(zOnlyViewZ) < 1000000.0f)
                        ++plausibleScreenZDepth;
                }

                const bool carriesPerspectiveDepth =
                    std::fabs(rhw - 1.0f) > 0.02f;
                if (carriesPerspectiveDepth)
                {
                    ++nonUnitRhw;
                    // Some OutRun CPU-projected smoke/skid/decal vertices bias
                    // or clamp screen-Z after projection. RHW still preserves
                    // clip-W, so accept it as depth evidence when the implied
                    // view depth is finite and physically plausible.
                    if (std::isfinite(viewZ) &&
                        std::fabs(viewZ) > 1.0e-4f &&
                        std::fabs(viewZ) < 1.0e6f)
                        ++plausibleRhwDepth;
                }

                // Strongest path: screen-Z and RHW both match the perspective
                // projection. HUD glyphs with RHW=1 never satisfy this.
                if (carriesPerspectiveDepth &&
                    std::fabs(expectedNdcZ - actualNdcZ) <= 0.10f)
                    ++projected;
            }

            const bool enoughEvidence =
                sampled >= 3u && valid >= 3u &&
                valid * 2u >= sampled;
            if (!enoughEvidence)
                return false;

            const bool exactProjected =
                projected * 4u >= valid * 3u;
            const bool rhwDepthOnly =
                nonUnitRhw * 4u >= valid * 3u &&
                plausibleRhwDepth * 4u >= nonUnitRhw * 3u;
            const bool screenZDepthOnly =
                plausibleScreenZDepth * 4u >= valid * 3u;
            if (screenZDepthOnly && !exactProjected && !rhwDepthOnly)
            {
                ++R30XyzrhwZOnlyDepthEvidence;
                if (!R30FirstXyzrhwRhwOnlyEvidenceLogged)
                {
                    R30FirstXyzrhwRhwOnlyEvidenceLogged = true;
                    spdlog::info(
                        "VR R30.11 XYZRHW DEPTH: perspective screen-Z accepted as world depth even when RHW=1; near-plane HUD Z remains excluded");
                }
            }
            if (rhwDepthOnly && !exactProjected)
            {
                ++R30XyzrhwRhwOnlyDepthEvidence;
                if (!R30FirstXyzrhwRhwOnlyEvidenceLogged)
                {
                    R30FirstXyzrhwRhwOnlyEvidenceLogged = true;
                    spdlog::info(
                        "VR R30.10 XYZRHW DEPTH: stable non-unit RHW accepted as world depth when screen-Z is post-projection biased; RHW=1 HUD remains excluded");
                }
            }
            return exactProjected || rhwDepthOnly || screenZDepthOnly;
        }

        bool R30XyzrhwLooksLikeHudPlane(
            const void* source, UINT vertexCount, UINT stride,
            const R30XyzrhwState& state,
            const std::vector<std::uint8_t>* usedMask) noexcept
        {
            if (!source || vertexCount == 0 || stride < sizeof(float) * 4)
                return false;
            const float depthSpan = state.viewport.MaxZ - state.viewport.MinZ;
            if (!std::isfinite(depthSpan) || depthSpan <= 1.0e-6f)
                return false;
            UINT sampled = 0, valid = 0, unitRhw = 0;
            float minZ = 1.0f, maxZ = 0.0f;
            for (UINT i = 0; i < vertexCount && sampled < 512u; ++i)
            {
                if (usedMask &&
                    (i >= usedMask->size() || (*usedMask)[i] == 0))
                    continue;
                ++sampled;
                const float* p = reinterpret_cast<const float*>(
                    static_cast<const std::uint8_t*>(source) +
                    static_cast<std::size_t>(i) * stride);
                const float z = p[2], rhw = p[3];
                if (!std::isfinite(z) || !std::isfinite(rhw))
                    continue;
                const float ndcZ = (z - state.viewport.MinZ) / depthSpan;
                if (!std::isfinite(ndcZ) || ndcZ < -0.01f || ndcZ > 1.01f)
                    continue;
                ++valid;
                if (std::fabs(rhw - 1.0f) <= 0.02f) ++unitRhw;
                minZ = std::min(minZ, ndcZ);
                maxZ = std::max(maxZ, ndcZ);
            }
            return sampled >= 3u && valid >= 3u &&
                valid * 2u >= sampled &&
                unitRhw * 4u >= valid * 3u &&
                std::isfinite(minZ) && std::isfinite(maxZ) &&
                maxZ - minZ <= 0.0025f;
        }

        bool R30BuildHudPlaneCoefficients(R30XyzrhwState& state) noexcept
        {
            constexpr float HudPlaneViewZ = -2.50f;
            float scaleX = 1.0f, scaleY = 1.0f;
            R30HudContainScale(state.stereo, scaleX, scaleY);
            const D3DMATRIX& bp = state.baseProjection;
            const float clipW = HudPlaneViewZ * bp._34 + bp._44;
            const float cx = HudPlaneViewZ * bp._31 + bp._41;
            const float cy = HudPlaneViewZ * bp._32 + bp._42;
            const float det = bp._11 * bp._22 - bp._21 * bp._12;
            if (!std::isfinite(clipW) || !std::isfinite(cx) ||
                !std::isfinite(cy) || !std::isfinite(det) ||
                std::fabs(clipW) <= 1.0e-6f ||
                std::fabs(det) <= 1.0e-6f)
                return false;

            const float vxX = bp._22 * clipW * scaleX / det;
            const float vxY = -bp._21 * clipW * scaleY / det;
            const float vxC = (-cx * bp._22 + bp._21 * cy) / det;
            const float vyX = -bp._12 * clipW * scaleX / det;
            const float vyY = bp._11 * clipW * scaleY / det;
            const float vyC = (-bp._11 * cy + bp._12 * cx) / det;

            for (int eye = 0; eye < 2; ++eye)
            {
                const auto& m = state.hudViewProjection[eye];
                auto build = [&](int col, float out[3]) noexcept {
                    out[0] = vxX * m.m[0][col] + vyX * m.m[1][col];
                    out[1] = vxY * m.m[0][col] + vyY * m.m[1][col];
                    out[2] = vxC * m.m[0][col] + vyC * m.m[1][col] +
                        HudPlaneViewZ * m.m[2][col] + m.m[3][col];
                };
                build(0, state.hudClipX[eye]);
                build(1, state.hudClipY[eye]);
                build(3, state.hudClipW[eye]);
                for (int i = 0; i < 3; ++i)
                    if (!std::isfinite(state.hudClipX[eye][i]) ||
                        !std::isfinite(state.hudClipY[eye][i]) ||
                        !std::isfinite(state.hudClipW[eye][i]))
                        return false;
            }
            return true;
        }

        bool R30ConfigureXyzrhwWorldEffect(
            IDirect3DDevice9* device, const void* source,
            UINT vertexCount, UINT stride, R30XyzrhwState& state,
            const std::vector<std::uint8_t>* usedMask = nullptr) noexcept
        {
            float projectionRaw[16]{};
            D3DMATRIX baseProjection{};
            const bool haveBaseProjection =
                OutRunVRRenderer::GetRendererBaseProjection(projectionRaw);
            if (haveBaseProjection)
                std::memcpy(&baseProjection, projectionRaw, sizeof(baseProjection));

            state.rhwDepthEvidence =
                haveBaseProjection &&
                R30XyzrhwHasProjectedDepthSignature(
                    source, vertexCount, stride, state,
                    baseProjection, usedMask);

            // R42: depth signature is authoritative world evidence. ZENABLE by
            // itself is not: several HUD passes leave Z enabled. When depth
            // evidence is absent, admit only a strongly pre-transformed screen
            // plane (RHW~=1 with near-constant Z) to the HUD world-lock.
            state.worldEffect = state.rhwDepthEvidence;
            if (state.depthTestEnabled && !state.worldEffect &&
                !R30XyzrhwLooksLikeHudPlane(
                    source, vertexCount, stride, state, usedMask))
                return false;
            if (!state.worldEffect)
            {
                // R41: most of OutRun's HUD is fixed-function XYZRHW, not the
                // shader/c64 path. Build a synthetic finite HUD plane in the
                // recentered gameplay space, then transform that plane by the
                // current head inverse and relative-eye IPD/FOV. Preserve the
                // original screen Z/RHW later so only placement is changed.
                if (!haveBaseProjection || !MatrixFinite(baseProjection))
                    return false;
                state.baseProjection = baseProjection;

                float headRaw[16]{};
                std::uint32_t headPoseSequence = 0;
                D3DMATRIX headInverse{};
                if (!OutRunVRRenderer::GetLatchedHeadInverse(
                        headRaw, headPoseSequence) ||
                    headPoseSequence != state.stereo.poseSequence)
                    return false;
                std::memcpy(&headInverse, headRaw, sizeof(headInverse));
                if (!MatrixFinite(headInverse))
                    return false;

                const float centerEye[3]{
                    0.5f * (state.stereo.eyeOffset[0][0] +
                            state.stereo.eyeOffset[1][0]),
                    0.5f * (state.stereo.eyeOffset[0][1] +
                            state.stereo.eyeOffset[1][1]),
                    0.5f * (state.stereo.eyeOffset[0][2] +
                            state.stereo.eyeOffset[1][2])
                };
                const float identityOrientation[4]{
                    0.0f, 0.0f, 0.0f, 1.0f
                };
                for (int eye = 0; eye < 2; ++eye)
                {
                    const float relativeEye[3]{
                        state.stereo.eyeOffset[eye][0] - centerEye[0],
                        state.stereo.eyeOffset[eye][1] - centerEye[1],
                        state.stereo.eyeOffset[eye][2] - centerEye[2]
                    };
                    const D3DMATRIX eyePose =
                        MatrixFromQuaternionTranslation(
                            identityOrientation, relativeEye,
                            Settings::VRWorldScale *
                                Settings::VRStereoDepth);
                    const D3DMATRIX eyeInverse = InverseRigid(eyePose);
                    const D3DMATRIX eyeProjection =
                        ProjectionFromFov(
                            baseProjection,
                            state.stereo.eyeFov[eye]);
                    state.hudViewProjection[eye] =
                        MultiplyMatrix(
                            MultiplyMatrix(headInverse, eyeInverse),
                            eyeProjection);
                    if (!MatrixFinite(state.hudViewProjection[eye]))
                        return false;
                }
                if (!R30BuildHudPlaneCoefficients(state))
                    return false;
                state.hudWorldLockValid = true;
                return true;
            }

            if (TrackedDepthStencil &&
                (!RightDepthSynchronized || !RightStencilSynchronized))
                TryBootstrapRightDepthFromRecentClear(device);
            if (TrackedDepthStencil &&
                !RightDepthSynchronized && DepthTestActive(device))
                return false;
            if (TrackedDepthStencil &&
                !RightStencilSynchronized && StencilTestActive(device))
                return false;

            if (!haveBaseProjection)
                return false;
            state.baseProjection = baseProjection;
            if (!MatrixFinite(baseProjection) ||
                std::fabs(baseProjection._11) < 0.01f ||
                std::fabs(baseProjection._22) < 0.01f ||
                std::fabs(baseProjection._34) < 0.25f ||
                !InvertMatrix(baseProjection, state.inverseBaseProjection) ||
                !MatrixFinite(state.inverseBaseProjection))
                return false;
            state.fullWorldReprojection = true;

            const float center[3]{
                0.5f * (state.stereo.eyeOffset[0][0] +
                        state.stereo.eyeOffset[1][0]),
                0.5f * (state.stereo.eyeOffset[0][1] +
                        state.stereo.eyeOffset[1][1]),
                0.5f * (state.stereo.eyeOffset[0][2] +
                        state.stereo.eyeOffset[1][2])
            };
            const float zero[3]{};

            for (int eye = 0; eye < 2; ++eye)
            {
                // Use the exact same per-eye rigid transform convention as the
                // normal fixed-function/world path: view * eyeInverse *
                // eyeProjection. The reconstructed CPU-projected point is
                // already in game view space, so only eyeInverse is applied
                // here; common head/camera motion must not be applied twice.
                // CPU-projected XYZRHW effects were generated from OutRun's
                // already head-synchronised camera. Reapplying the absolute HMD
                // eye orientation here rotates them twice and shifts smoke/skid
                // away from the road. Apply only the eye pose relative to the
                // common head centre; the asymmetric eye projection still owns
                // the final FOV.
                const float relativeEye[3]{
                    state.stereo.eyeOffset[eye][0] - center[0],
                    state.stereo.eyeOffset[eye][1] - center[1],
                    state.stereo.eyeOffset[eye][2] - center[2]
                };
                const float identityOrientation[4]{
                    0.0f, 0.0f, 0.0f, 1.0f
                };
                const D3DMATRIX eyePose =
                    MatrixFromQuaternionTranslation(
                        identityOrientation, relativeEye,
                        Settings::VRWorldScale * Settings::VRStereoDepth);
                state.eyeInverse[eye] = InverseRigid(eyePose);
                state.eyeProjection[eye] =
                    ProjectionFromFov(baseProjection,
                        state.stereo.eyeFov[eye]);

                // Preserve a conservative affine fallback for vertices whose
                // clip reconstruction is unsafe. Its parallax uses the old
                // centered IPD estimate, but both eyes are forced to choose the
                // same fallback mode by R30TransformXyzrhwStereo().
                const float rel[3]{
                    state.stereo.eyeOffset[eye][0] - center[0],
                    state.stereo.eyeOffset[eye][1] - center[1],
                    state.stereo.eyeOffset[eye][2] - center[2]
                };
                // The source camera already owns common head rotation, so the
                // conservative affine fallback also uses head-relative IPD only.
                const float localX = rel[0];
                const float localY = rel[1];

                const D3DMATRIX& eyeProjection = state.eyeProjection[eye];
                state.worldScaleX[eye] =
                    eyeProjection._11 / baseProjection._11;
                state.worldOffsetX[eye] =
                    (eyeProjection._31 -
                     baseProjection._31 * state.worldScaleX[eye]) /
                    baseProjection._34;
                state.worldScaleY[eye] =
                    eyeProjection._22 / baseProjection._22;
                state.worldOffsetY[eye] =
                    (eyeProjection._32 -
                     baseProjection._32 * state.worldScaleY[eye]) /
                    baseProjection._34;
                state.parallaxPerRhwX[eye] =
                    -localX * Settings::VRWorldScale * Settings::VRStereoDepth *
                    eyeProjection._11;
                state.parallaxPerRhwY[eye] =
                    -localY * Settings::VRWorldScale * Settings::VRStereoDepth *
                    eyeProjection._22;

                if (!std::isfinite(state.worldScaleX[eye]) ||
                    !std::isfinite(state.worldOffsetX[eye]) ||
                    !std::isfinite(state.worldScaleY[eye]) ||
                    !std::isfinite(state.worldOffsetY[eye]) ||
                    !std::isfinite(state.parallaxPerRhwX[eye]) ||
                    !std::isfinite(state.parallaxPerRhwY[eye]) ||
                    !MatrixFinite(state.eyeProjection[eye]) ||
                    !MatrixFinite(state.eyeInverse[eye]) ||
                    state.worldScaleX[eye] < 0.20f ||
                    state.worldScaleX[eye] > 5.0f ||
                    state.worldScaleY[eye] < 0.20f ||
                    state.worldScaleY[eye] > 5.0f)
                    return false;
            }

            if (state.rhwDepthEvidence && !state.depthTestEnabled)
            {
                ++R30XyzrhwRhwWorldPromotions;
                if (!R30FirstXyzrhwRhwPromotionLogged)
                {
                    R30FirstXyzrhwRhwPromotionLogged = true;
                    spdlog::info(
                        "VR R30.6 XYZRHW WORLD: referenced RHW/depth signature promoted a depth-disabled particle/decal; normal-world eyeInverse/projection transform is shared");
                }
            }
            return true;
        }

        UINT R30PrimitiveElementCount(
            D3DPRIMITIVETYPE type, UINT primitiveCount) noexcept
        {
            switch (type)
            {
            case D3DPT_POINTLIST: return primitiveCount;
            case D3DPT_LINELIST: return primitiveCount * 2u;
            case D3DPT_LINESTRIP: return primitiveCount + 1u;
            case D3DPT_TRIANGLELIST: return primitiveCount * 3u;
            case D3DPT_TRIANGLESTRIP:
            case D3DPT_TRIANGLEFAN: return primitiveCount + 2u;
            default: return 0;
            }
        }

        bool R30TransformXyzrhwVertices(
            const void* source, UINT vertexCount, UINT stride,
            const R30XyzrhwState& state, int eye,
            std::vector<std::uint8_t>& out,
            const std::vector<std::uint8_t>* usedMask,
            bool allowFullReprojection, bool& usedFullReprojection) noexcept
        {
            usedFullReprojection = false;
            if (!source || vertexCount == 0 || stride < sizeof(float) * 4 ||
                eye < 0 || eye > 1)
                return false;
            const std::size_t bytes =
                static_cast<std::size_t>(vertexCount) * stride;
            if (bytes == 0 || bytes > 4u * 1024u * 1024u)
                return false;
            if (usedMask && usedMask->size() < vertexCount)
                return false;

            try
            {
                out.resize(bytes);
            }
            catch (...)
            {
                return false;
            }
            std::memcpy(out.data(), source, bytes);

            const float x0 = static_cast<float>(state.viewport.X);
            const float y0 = static_cast<float>(state.viewport.Y);
            const float width = static_cast<float>(state.viewport.Width);
            const float height = static_cast<float>(state.viewport.Height);
            const float minZ = state.viewport.MinZ;
            const float maxZ = state.viewport.MaxZ;
            const float depthSpan = maxZ - minZ;
            if (width <= 0.0f || height <= 0.0f ||
                !std::isfinite(depthSpan) || depthSpan <= 1.0e-6f)
                return false;

            auto selected = [&](UINT i) noexcept
            {
                return !usedMask || (*usedMask)[i] != 0;
            };
            auto sourceVertex = [&](UINT i) noexcept -> const float*
            {
                return reinterpret_cast<const float*>(
                    static_cast<const std::uint8_t*>(source) +
                    static_cast<std::size_t>(i) * stride);
            };
            auto outputVertex = [&](UINT i) noexcept -> float*
            {
                return reinterpret_cast<float*>(
                    out.data() + static_cast<std::size_t>(i) * stride);
            };

            if (state.worldEffect && state.fullWorldReprojection &&
                allowFullReprojection)
            {
                bool fullDrawOk = true;
                UINT selectedVertices = 0;
                for (UINT i = 0; i < vertexCount; ++i)
                {
                    if (!selected(i))
                        continue;
                    ++selectedVertices;

                    const float* src = sourceVertex(i);
                    float* dst = outputVertex(i);
                    const float x = src[0];
                    const float y = src[1];
                    const float screenZ = src[2];
                    const float rhw = src[3];
                    if (!std::isfinite(x) || !std::isfinite(y) ||
                        !std::isfinite(screenZ) || !std::isfinite(rhw) ||
                        rhw <= 1.0e-6f || rhw >= 1000.0f)
                    {
                        fullDrawOk = false;
                        break;
                    }

                    const float ndcX = ((x - x0) / width) * 2.0f - 1.0f;
                    const float ndcY =
                        1.0f - ((y - y0) / height) * 2.0f;
                    const float clipW = 1.0f / rhw;
                    const float clipX = ndcX * clipW;
                    const float clipY = ndcY * clipW;

                    float restoredView[4]{};
                    bool restored = false;

                    // Pre-transformed D3D9 particles carry RHW = 1/clipW.
                    // For the normal OutRun perspective matrix, clipW gives
                    // view-Z directly and X/Y can then be solved from clip X/Y.
                    // This avoids depending on the particle's biased/clamped
                    // screen Z, which is exactly what caused smoke/decal draws
                    // to fall back to the approximate affine path.
                    const D3DMATRIX& bp = state.baseProjection;
                    if (std::fabs(bp._14) <= 1.0e-5f &&
                        std::fabs(bp._24) <= 1.0e-5f &&
                        std::fabs(bp._34) > 1.0e-5f)
                    {
                        const float viewZ =
                            (clipW - bp._44) / bp._34;
                        const float rhsX =
                            clipX - viewZ * bp._31 - bp._41;
                        const float rhsY =
                            clipY - viewZ * bp._32 - bp._42;
                        const float det =
                            bp._11 * bp._22 - bp._21 * bp._12;
                        if (std::isfinite(viewZ) &&
                            std::isfinite(rhsX) &&
                            std::isfinite(rhsY) &&
                            std::isfinite(det) &&
                            std::fabs(det) > 1.0e-6f)
                        {
                            const float viewX =
                                (rhsX * bp._22 -
                                 bp._21 * rhsY) / det;
                            const float viewY =
                                (bp._11 * rhsY -
                                 rhsX * bp._12) / det;
                            if (std::isfinite(viewX) &&
                                std::isfinite(viewY))
                            {
                                restoredView[0] = viewX;
                                restoredView[1] = viewY;
                                restoredView[2] = viewZ;
                                restoredView[3] = 1.0f;
                                restored = true;
                            }
                        }
                    }

                    // Compatibility fallback for an unusual projection shape:
                    // retain the previous inverse-projection reconstruction.
                    if (!restored)
                    {
                        const float ndcZ =
                            (screenZ - minZ) / depthSpan;
                        if (!std::isfinite(ndcZ) ||
                            ndcZ < -0.25f || ndcZ > 1.25f)
                        {
                            fullDrawOk = false;
                            break;
                        }
                        const float clip[4]{
                            clipX, clipY, ndcZ * clipW, clipW
                        };
                        float restoredViewH[4]{};
                        for (int col = 0; col < 4; ++col)
                            for (int row = 0; row < 4; ++row)
                                restoredViewH[col] += clip[row] *
                                    state.inverseBaseProjection.m[row][col];

                        if (!std::isfinite(restoredViewH[0]) ||
                            !std::isfinite(restoredViewH[1]) ||
                            !std::isfinite(restoredViewH[2]) ||
                            !std::isfinite(restoredViewH[3]) ||
                            std::fabs(restoredViewH[3]) <= 1.0e-6f)
                        {
                            fullDrawOk = false;
                            break;
                        }

                        const float invRestoredW =
                            1.0f / restoredViewH[3];
                        restoredView[0] =
                            restoredViewH[0] * invRestoredW;
                        restoredView[1] =
                            restoredViewH[1] * invRestoredW;
                        restoredView[2] =
                            restoredViewH[2] * invRestoredW;
                        restoredView[3] = 1.0f;
                    }

                    // Match the normal world path exactly: the reconstructed
                    // game-view point receives the same rigid eyeInverse that
                    // BuildEyeConstants/fixed-function world draws use.
                    float eyeView[4]{};
                    for (int col = 0; col < 4; ++col)
                        for (int row = 0; row < 4; ++row)
                            eyeView[col] += restoredView[row] *
                                state.eyeInverse[eye].m[row][col];

                    float clipEye[4]{};
                    for (int col = 0; col < 4; ++col)
                        for (int row = 0; row < 4; ++row)
                            clipEye[col] += eyeView[row] *
                                state.eyeProjection[eye].m[row][col];

                    // D3D9 perspective clip-W must stay in front of the eye.
                    // abs(W) would accept a point behind the camera and mirror
                    // it back into the visible image.
                    if (!std::isfinite(clipEye[0]) ||
                        !std::isfinite(clipEye[1]) ||
                        !std::isfinite(clipEye[2]) ||
                        !std::isfinite(clipEye[3]) ||
                        clipEye[3] <= 1.0e-6f)
                    {
                        fullDrawOk = false;
                        break;
                    }

                    const float invEyeW = 1.0f / clipEye[3];
                    const float fullX = clipEye[0] * invEyeW;
                    const float fullY = clipEye[1] * invEyeW;
                    const float fullZ = clipEye[2] * invEyeW;
                    if (!std::isfinite(fullX) ||
                        !std::isfinite(fullY) ||
                        !std::isfinite(fullZ))
                    {
                        fullDrawOk = false;
                        break;
                    }

                    const float transformedX =
                        x0 + (fullX + 1.0f) * 0.5f * width;
                    const float transformedY =
                        y0 + (1.0f - fullY) * 0.5f * height;

                    // Decals/particles can intentionally bias screen Z beyond
                    // the strict viewport interval. Preserve the game's Z in
                    // that case, while still using the accurately reprojected
                    // eye X/Y and RHW. Falling the whole quad back to an affine
                    // shift was the visible left/right smoke/skid offset.
                    float transformedZ = screenZ;
                    if (fullZ >= 0.0f && fullZ <= 1.0f)
                    {
                        transformedZ = minZ + fullZ * depthSpan;
                    }
                    else
                    {
                        ++R30XyzrhwDepthPreserveFallbacks;
                    }
                    if (!std::isfinite(transformedX) ||
                        !std::isfinite(transformedY) ||
                        !std::isfinite(transformedZ))
                    {
                        fullDrawOk = false;
                        break;
                    }

                    dst[0] = transformedX;
                    dst[1] = transformedY;
                    dst[2] = transformedZ;
                    dst[3] = invEyeW;
                }

                if (fullDrawOk && selectedVertices != 0)
                {
                    usedFullReprojection = true;
                    return true;
                }

                ++R30XyzrhwAtomicFallbacks;
                if (!R30FirstXyzrhwAtomicFallbackLogged)
                {
                    R30FirstXyzrhwAtomicFallbackLogged = true;
                    spdlog::info(
                        "VR R30.6 XYZRHW WORLD: full reprojection rejected for a referenced vertex; this eye candidate is discarded before bilateral fallback selection");
                }
                std::memcpy(out.data(), source, bytes);
            }

            for (UINT i = 0; i < vertexCount; ++i)
            {
                if (!selected(i))
                    continue;

                float* p = outputVertex(i);
                const float x = p[0];
                const float y = p[1];
                const float z = p[2];
                const float rhw = p[3];
                if (!std::isfinite(x) || !std::isfinite(y) ||
                    !std::isfinite(z) || !std::isfinite(rhw))
                    return false;

                const float ndcX = ((x - x0) / width) * 2.0f - 1.0f;
                const float ndcY =
                    1.0f - ((y - y0) / height) * 2.0f;

                float correctedX = 0.0f;
                float correctedY = 0.0f;
                if (state.worldEffect)
                {
                    correctedX =
                        state.worldScaleX[eye] * ndcX +
                        state.worldOffsetX[eye];
                    correctedY =
                        state.worldScaleY[eye] * ndcY +
                        state.worldOffsetY[eye];
                    if (rhw > 0.0f && rhw < 1000.0f)
                    {
                        correctedX +=
                            state.parallaxPerRhwX[eye] * rhw;
                        correctedY +=
                            state.parallaxPerRhwY[eye] * rhw;
                    }
                    // Fallback keeps the game's original Z/RHW pair intact.
                }
                else
                {
                    // R41: the real OutRun HUD is overwhelmingly XYZRHW. Put
                    // those pre-transformed vertices on the same finite,
                    // recentered plane used by the shader HUD instead of
                    // copying screen coordinates into both eyes (head-lock).
                    if (!state.hudWorldLockValid)
                        return false;

                    const float clipX =
                        state.hudClipX[eye][0] * ndcX +
                        state.hudClipX[eye][1] * ndcY +
                        state.hudClipX[eye][2];
                    const float clipY =
                        state.hudClipY[eye][0] * ndcX +
                        state.hudClipY[eye][1] * ndcY +
                        state.hudClipY[eye][2];
                    const float clipW =
                        state.hudClipW[eye][0] * ndcX +
                        state.hudClipW[eye][1] * ndcY +
                        state.hudClipW[eye][2];
                    if (!std::isfinite(clipX) ||
                        !std::isfinite(clipY) ||
                        !std::isfinite(clipW) ||
                        clipW <= 1.0e-6f)
                        return false;
                    correctedX = clipX / clipW;
                    correctedY = clipY / clipW;
                    if (!std::isfinite(correctedX) ||
                        !std::isfinite(correctedY))
                        return false;
                }

                const float transformedX =
                    x0 + (correctedX + 1.0f) * 0.5f * width;
                const float transformedY =
                    y0 + (1.0f - correctedY) * 0.5f * height;
                if (!std::isfinite(transformedX) ||
                    !std::isfinite(transformedY))
                    return false;
                p[0] = transformedX;
                p[1] = transformedY;
            }
            return true;
        }

        bool R30TransformXyzrhwStereo(
            const void* source, UINT vertexCount, UINT stride,
            const R30XyzrhwState& state,
            std::vector<std::uint8_t>& left,
            std::vector<std::uint8_t>& right,
            const std::vector<std::uint8_t>* usedMask = nullptr) noexcept
        {
            bool leftFull = false;
            bool rightFull = false;
            if (!R30TransformXyzrhwVertices(source, vertexCount, stride,
                    state, 0, left, usedMask, true, leftFull) ||
                !R30TransformXyzrhwVertices(source, vertexCount, stride,
                    state, 1, right, usedMask, true, rightFull))
                return false;

            if (leftFull != rightFull)
            {
                ++R30XyzrhwBilateralFallbacks;
                if (!R30FirstXyzrhwBilateralFallbackLogged)
                {
                    R30FirstXyzrhwBilateralFallbackLogged = true;
                    spdlog::info(
                        "VR R30.6 XYZRHW WORLD: one eye rejected full reprojection; both eyes are forced onto the same affine fallback policy");
                }
                bool ignored = false;
                if (!R30TransformXyzrhwVertices(source, vertexCount, stride,
                        state, 0, left, usedMask, false, ignored) ||
                    !R30TransformXyzrhwVertices(source, vertexCount, stride,
                        state, 1, right, usedMask, false, ignored))
                    return false;
            }
            return true;
        }

        template <typename LeftDraw, typename RightDraw>
        HRESULT R30ExecuteXyzrhwStereo(
            IDirect3DDevice9* device, const R30XyzrhwState& state,
            LeftDraw&& leftDraw, RightDraw&& rightDraw,
            const char* site)
        {
            if (!state.worldEffect)
                R30CaptureSkyGlowSceneBeforeHud(device);

            ++R9DrawCalls;
            R9MonoBackupGap = true;
            if (LeftDrawMayWriteDepth(device) ||
                LeftDrawMayWriteStencil(device))
                ++R9MainDepthContentSerial;

            const HRESULT leftHr = leftDraw();
            if (FAILED(leftHr))
            {
                R9Poison(OutRunVR::StereoFailureLeftDrawFailed,
                    site, leftHr);
                R30ArmSafeFallback();
                return leftHr;
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
                    rightHr = device->SetViewport(&state.viewport);
                if (SUCCEEDED(rightHr))
                {
                    rightFailure =
                        OutRunVR::StereoFailureRightDrawFailed;
                    rightHr = rightDraw();
                }
                restoreOk = RestoreRightPassState(device,
                    savedRt, savedDepth, state.viewport, nullptr, false);
            }

            FrameHadDuplicatedDraw = true;
            ++DuplicatedDraws;
            ++R30SafeTwoEyeDraws;
            if (state.worldEffect)
            {
                ++R30XyzrhwWorldEffectDraws;
                if (FrameStereoPoseSequence == 0)
                {
                    FrameStereoPoseSequence = state.stereo.poseSequence;
                    FrameStereoMetadata = state.stereo;
                }
                else if (FrameStereoPoseSequence != state.stereo.poseSequence)
                {
                    FrameRightDrawFailed = true;
                    PoisonFrame(OutRunVR::StereoFailurePoseSequenceMismatch);
                }
                FrameHadWorldStereo = true;
                ++WorldStereoDraws;
                if (!R30FirstXyzrhwWorldLogged)
                {
                    R30FirstXyzrhwWorldLogged = true;
                    spdlog::info(
                        "VR R30.4 XYZRHW WORLD: pre-transformed particle/billboard/decal draws reconstruct clip X/Y/Z/RHW, unproject through the game projection, then reproject into each OpenXR eye; affine RHW fallback retained for unsafe vertices");
                }
            }
            else
            {
                ++NonWorldDuplicatedDraws;
                ++R30XyzrhwHudDraws;
                if (state.hudWorldLockValid)
                    ++R30XyzrhwWorldLockedHudDraws;
                if (!R30FirstXyzrhwHudLogged)
                {
                    R30FirstXyzrhwHudLogged = true;
                    spdlog::info(
                        "VR R41 XYZRHW HUD: fixed-function HUD uses HudScale on a finite recentered world-locked plane; R42 Z-enabled screen-plane HUD is safely admitted and uses cached planar coefficients instead of per-vertex 4x4 transforms");
                }
            }

            if (FAILED(rightHr))
            {
                FrameRightDrawFailed = true;
                InvalidateRightDepthStencilIfLeftMayWrite(device);
                R9Poison(rightFailure, site, rightHr);
                R30ArmSafeFallback();
            }
            if (!restoreOk)
            {
                InvalidateRightDepthStencilIfLeftMayWrite(device);
                NoteRestoreFailure("R30.2 XYZRHW right-eye draw");
                R30ArmSafeFallback();
            }
            return leftHr;
        }

        HRESULT R30TryXyzrhwPrimitiveUP(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            UINT primitiveCount, const void* data, UINT stride)
        {
            R30XyzrhwState state{};
            if (!R30PrepareXyzrhwState(device, state))
                return E_NOTIMPL;
            const UINT vertexCount =
                R30PrimitiveElementCount(type, primitiveCount);
            if (!vertexCount || vertexCount > 262144u)
                return E_NOTIMPL;
            if (!R30ConfigureXyzrhwWorldEffect(
                    device, data, vertexCount, stride, state))
            {
                ++R30XyzrhwFallbacks;
                return E_NOTIMPL;
            }

            R30ScratchLease lease;
            if (!lease)
                return E_NOTIMPL;
            auto& scratch = *lease.buffers;
            if (!R30TransformXyzrhwStereo(
                    data, vertexCount, stride, state,
                    scratch.left, scratch.right))
            {
                ++R30XyzrhwFallbacks;
                return E_NOTIMPL;
            }

            auto leftDraw = [&]() {
                return DrawPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, primitiveCount,
                    scratch.left.data(), stride);
            };
            auto rightDraw = [&]() {
                return DrawPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, primitiveCount,
                    scratch.right.data(), stride);
            };
            return R30ExecuteXyzrhwStereo(
                device, state, leftDraw, rightDraw,
                "R30.6/DrawPrimitiveUP-XYZRHW");
        }

        HRESULT R30TryXyzrhwIndexedPrimitiveUP(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            UINT minVertexIndex, UINT numVertices, UINT primitiveCount,
            const void* indexData, D3DFORMAT indexFormat,
            const void* vertexData, UINT stride)
        {
            R30XyzrhwState state{};
            if (!R30PrepareXyzrhwState(device, state))
                return E_NOTIMPL;

            const UINT indexCount =
                R30PrimitiveElementCount(type, primitiveCount);
            if (!indexData || !vertexData || indexCount == 0 ||
                (indexFormat != D3DFMT_INDEX16 &&
                 indexFormat != D3DFMT_INDEX32))
                return E_NOTIMPL;

            UINT maxIndex = 0;
            if (indexFormat == D3DFMT_INDEX16)
            {
                const auto* indices =
                    static_cast<const std::uint16_t*>(indexData);
                for (UINT i = 0; i < indexCount; ++i)
                    maxIndex = std::max<UINT>(maxIndex, indices[i]);
            }
            else
            {
                const auto* indices =
                    static_cast<const std::uint32_t*>(indexData);
                for (UINT i = 0; i < indexCount; ++i)
                    maxIndex = std::max(maxIndex, indices[i]);
            }

            const UINT vertexCount =
                std::max<UINT>(minVertexIndex + numVertices,
                    maxIndex + 1u);
            if (vertexCount == 0 || vertexCount > 262144u)
                return E_NOTIMPL;

            R30ScratchLease lease;
            if (!lease)
                return E_NOTIMPL;
            auto& scratch = *lease.buffers;
            try
            {
                scratch.used.assign(vertexCount, 0);
            }
            catch (...)
            {
                return E_NOTIMPL;
            }
            for (UINT i = 0; i < indexCount; ++i)
            {
                const std::uint32_t raw =
                    indexFormat == D3DFMT_INDEX16
                    ? static_cast<const std::uint16_t*>(indexData)[i]
                    : static_cast<const std::uint32_t*>(indexData)[i];
                if (raw >= vertexCount)
                    return E_NOTIMPL;
                scratch.used[raw] = 1;
            }

            if (!R30ConfigureXyzrhwWorldEffect(
                    device, vertexData, vertexCount, stride, state,
                    &scratch.used))
            {
                ++R30XyzrhwFallbacks;
                return E_NOTIMPL;
            }

            if (!R30TransformXyzrhwStereo(
                    vertexData, vertexCount, stride, state,
                    scratch.left, scratch.right, &scratch.used))
            {
                ++R30XyzrhwFallbacks;
                return E_NOTIMPL;
            }

            auto leftDraw = [&]() {
                return DrawIndexedPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, minVertexIndex, numVertices,
                    primitiveCount, indexData, indexFormat,
                    scratch.left.data(), stride);
            };
            auto rightDraw = [&]() {
                return DrawIndexedPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, minVertexIndex, numVertices,
                    primitiveCount, indexData, indexFormat,
                    scratch.right.data(), stride);
            };
            return R30ExecuteXyzrhwStereo(
                device, state, leftDraw, rightDraw,
                "R30.6/DrawIndexedPrimitiveUP-XYZRHW");
        }


        HRESULT R30TryXyzrhwPrimitiveVB(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            UINT startVertex, UINT primitiveCount)
        {
            R30XyzrhwState state{};
            if (!R30PrepareXyzrhwState(device, state))
                return E_NOTIMPL;

            const UINT vertexCount =
                R30PrimitiveElementCount(type, primitiveCount);
            if (!vertexCount || vertexCount > 262144u)
                return E_NOTIMPL;

            IDirect3DVertexBuffer9* vb = nullptr;
            UINT streamOffset = 0;
            UINT stride = 0;
            if (FAILED(device->GetStreamSource(
                    0, &vb, &streamOffset, &stride)) ||
                !vb || stride < sizeof(float) * 4)
            {
                if (vb) vb->Release();
                return E_NOTIMPL;
            }

            R30EnsureVertexBufferHooks(vb);
            const bool wasArmed = R30BufferShadowCaptureArmed.exchange(
                true, std::memory_order_acq_rel);
            R30EnsureVertexShadow(vb);
            if (!wasArmed && !R30FirstShadowArmLogged)
            {
                R30FirstShadowArmLogged = true;
                spdlog::info(
                    "VR R30 BUFFER SHADOW: lazy capture armed only after a real XYZRHW VB draw; ordinary world buffers no longer pay per-Lock shadow memcpy cost");
            }

            D3DVERTEXBUFFER_DESC desc{};
            const std::uint64_t firstByte =
                static_cast<std::uint64_t>(streamOffset) +
                static_cast<std::uint64_t>(startVertex) * stride;
            const std::uint64_t byteCount =
                static_cast<std::uint64_t>(vertexCount) * stride;
            if (FAILED(vb->GetDesc(&desc)) || firstByte > desc.Size ||
                byteCount > desc.Size - firstByte ||
                firstByte > UINT_MAX || byteCount > UINT_MAX)
            {
                vb->Release();
                return E_NOTIMPL;
            }

            R30ScratchLease lease;
            if (!lease)
            {
                vb->Release();
                return E_NOTIMPL;
            }
            auto& scratch = *lease.buffers;
            if (!R30CopyVertexShadow(vb,
                    static_cast<UINT>(firstByte),
                    static_cast<UINT>(byteCount), scratch.source))
            {
                vb->Release();
                ++R30XyzrhwFallbacks;
                return E_NOTIMPL;
            }

            if (!R30ConfigureXyzrhwWorldEffect(
                    device, scratch.source.data(), vertexCount,
                    stride, state) ||
                !R30TransformXyzrhwStereo(
                    scratch.source.data(), vertexCount, stride, state,
                    scratch.left, scratch.right))
            {
                vb->Release();
                ++R30XyzrhwFallbacks;
                return E_NOTIMPL;
            }

            auto leftDraw = [&]() {
                return DrawPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, primitiveCount,
                    scratch.left.data(), stride);
            };
            auto rightDraw = [&]() {
                return DrawPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, primitiveCount,
                    scratch.right.data(), stride);
            };
            const HRESULT hr = R30ExecuteXyzrhwStereo(
                device, state, leftDraw, rightDraw,
                "R30.6/DrawPrimitiveVB-ShadowXYZRHW");

            {
                InternalPassScope guard;
                if (FAILED(device->SetStreamSource(
                        0, vb, streamOffset, stride)))
                {
                    NoteRestoreFailure("R30.6 VB stream restore");
                    R30ArmSafeFallback();
                }
            }
            vb->Release();
            return hr;
        }

        HRESULT R30TryXyzrhwIndexedPrimitiveVB(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            INT baseVertexIndex, UINT minVertexIndex, UINT numVertices,
            UINT startIndex, UINT primitiveCount)
        {
            R30XyzrhwState state{};
            if (!R30PrepareXyzrhwState(device, state))
                return E_NOTIMPL;

            const UINT indexCount =
                R30PrimitiveElementCount(type, primitiveCount);
            if (!indexCount || !numVertices || indexCount > 524288u)
                return E_NOTIMPL;

            IDirect3DVertexBuffer9* vb = nullptr;
            IDirect3DIndexBuffer9* ib = nullptr;
            UINT streamOffset = 0;
            UINT stride = 0;
            if (FAILED(device->GetStreamSource(
                    0, &vb, &streamOffset, &stride)) ||
                !vb || stride < sizeof(float) * 4 ||
                FAILED(device->GetIndices(&ib)) || !ib)
            {
                if (ib) ib->Release();
                if (vb) vb->Release();
                return E_NOTIMPL;
            }

            R30EnsureVertexBufferHooks(vb);
            R30EnsureIndexBufferHooks(ib);
            const bool wasArmed = R30BufferShadowCaptureArmed.exchange(
                true, std::memory_order_acq_rel);
            R30EnsureVertexShadow(vb);
            R30EnsureIndexShadow(ib);
            if (!wasArmed && !R30FirstShadowArmLogged)
            {
                R30FirstShadowArmLogged = true;
                spdlog::info(
                    "VR R30 BUFFER SHADOW: lazy capture armed only after a real XYZRHW VB/IB draw; ordinary world buffers no longer pay per-Lock shadow memcpy cost");
            }

            D3DINDEXBUFFER_DESC ibDesc{};
            if (FAILED(ib->GetDesc(&ibDesc)) ||
                (ibDesc.Format != D3DFMT_INDEX16 &&
                 ibDesc.Format != D3DFMT_INDEX32))
            {
                ib->Release();
                vb->Release();
                return E_NOTIMPL;
            }
            const UINT indexSize =
                ibDesc.Format == D3DFMT_INDEX16 ? 2u : 4u;
            const std::uint64_t firstIndexByte =
                static_cast<std::uint64_t>(startIndex) * indexSize;
            const std::uint64_t indexBytes =
                static_cast<std::uint64_t>(indexCount) * indexSize;
            if (firstIndexByte > ibDesc.Size ||
                indexBytes > ibDesc.Size - firstIndexByte ||
                firstIndexByte > UINT_MAX || indexBytes > UINT_MAX)
            {
                ib->Release();
                vb->Release();
                return E_NOTIMPL;
            }

            R30ScratchLease lease;
            if (!lease)
            {
                ib->Release();
                vb->Release();
                return E_NOTIMPL;
            }
            auto& scratch = *lease.buffers;
            if (!R30CopyIndexShadow(ib,
                    static_cast<UINT>(firstIndexByte),
                    static_cast<UINT>(indexBytes), scratch.indexSource))
            {
                ib->Release();
                vb->Release();
                ++R30XyzrhwFallbacks;
                return E_NOTIMPL;
            }

            try
            {
                scratch.physical.resize(indexCount);
            }
            catch (...)
            {
                ib->Release();
                vb->Release();
                return E_NOTIMPL;
            }

            std::int64_t minPhysical = INT64_MAX;
            std::int64_t maxPhysical = INT64_MIN;
            for (UINT i = 0; i < indexCount; ++i)
            {
                const std::uint32_t raw =
                    ibDesc.Format == D3DFMT_INDEX16
                    ? static_cast<const std::uint16_t*>(
                        static_cast<const void*>(scratch.indexSource.data()))[i]
                    : static_cast<const std::uint32_t*>(
                        static_cast<const void*>(scratch.indexSource.data()))[i];
                const std::int64_t p =
                    static_cast<std::int64_t>(baseVertexIndex) + raw;
                if (p < 0 || p > UINT_MAX)
                {
                    ib->Release();
                    vb->Release();
                    return E_NOTIMPL;
                }
                scratch.physical[i] = static_cast<std::uint32_t>(p);
                minPhysical = std::min(minPhysical, p);
                maxPhysical = std::max(maxPhysical, p);
            }

            if (minPhysical == INT64_MAX || maxPhysical < minPhysical)
            {
                ib->Release();
                vb->Release();
                return E_NOTIMPL;
            }

            const std::uint64_t vertexCount64 =
                static_cast<std::uint64_t>(maxPhysical - minPhysical) + 1u;
            if (!vertexCount64 || vertexCount64 > 262144u)
            {
                ib->Release();
                vb->Release();
                return E_NOTIMPL;
            }
            const UINT vertexCount = static_cast<UINT>(vertexCount64);

            D3DVERTEXBUFFER_DESC vbDesc{};
            const std::uint64_t firstVertexByte =
                static_cast<std::uint64_t>(streamOffset) +
                static_cast<std::uint64_t>(minPhysical) * stride;
            const std::uint64_t vertexBytes =
                static_cast<std::uint64_t>(vertexCount) * stride;
            if (FAILED(vb->GetDesc(&vbDesc)) ||
                firstVertexByte > vbDesc.Size ||
                vertexBytes > vbDesc.Size - firstVertexByte ||
                firstVertexByte > UINT_MAX || vertexBytes > UINT_MAX)
            {
                ib->Release();
                vb->Release();
                return E_NOTIMPL;
            }

            if (!R30CopyVertexShadow(vb,
                    static_cast<UINT>(firstVertexByte),
                    static_cast<UINT>(vertexBytes), scratch.source))
            {
                ib->Release();
                vb->Release();
                ++R30XyzrhwFallbacks;
                return E_NOTIMPL;
            }

            try
            {
                scratch.used.assign(vertexCount, 0);
            }
            catch (...)
            {
                ib->Release();
                vb->Release();
                return E_NOTIMPL;
            }
            for (UINT i = 0; i < indexCount; ++i)
            {
                const std::uint64_t local =
                    scratch.physical[i] -
                    static_cast<std::uint64_t>(minPhysical);
                if (local >= vertexCount)
                {
                    ib->Release();
                    vb->Release();
                    return E_NOTIMPL;
                }
                scratch.used[static_cast<std::size_t>(local)] = 1;
            }

            if (!R30ConfigureXyzrhwWorldEffect(
                    device, scratch.source.data(), vertexCount,
                    stride, state, &scratch.used) ||
                !R30TransformXyzrhwStereo(
                    scratch.source.data(), vertexCount, stride, state,
                    scratch.left, scratch.right, &scratch.used))
            {
                ib->Release();
                vb->Release();
                ++R30XyzrhwFallbacks;
                return E_NOTIMPL;
            }

            const void* rebased = nullptr;
            if (ibDesc.Format == D3DFMT_INDEX16)
            {
                try { scratch.indices16.resize(indexCount); }
                catch (...) {
                    ib->Release();
                    vb->Release();
                    return E_NOTIMPL;
                }
                for (UINT i = 0; i < indexCount; ++i)
                {
                    const std::uint64_t v =
                        scratch.physical[i] -
                        static_cast<std::uint64_t>(minPhysical);
                    if (v > 0xFFFFu)
                    {
                        ib->Release();
                        vb->Release();
                        return E_NOTIMPL;
                    }
                    scratch.indices16[i] = static_cast<std::uint16_t>(v);
                }
                rebased = scratch.indices16.data();
            }
            else
            {
                try { scratch.indices32.resize(indexCount); }
                catch (...) {
                    ib->Release();
                    vb->Release();
                    return E_NOTIMPL;
                }
                for (UINT i = 0; i < indexCount; ++i)
                    scratch.indices32[i] =
                        scratch.physical[i] -
                        static_cast<std::uint32_t>(minPhysical);
                rebased = scratch.indices32.data();
            }

            auto leftDraw = [&]() {
                return DrawIndexedPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, 0u, vertexCount, primitiveCount,
                    rebased, ibDesc.Format, scratch.left.data(), stride);
            };
            auto rightDraw = [&]() {
                return DrawIndexedPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, 0u, vertexCount, primitiveCount,
                    rebased, ibDesc.Format, scratch.right.data(), stride);
            };
            const HRESULT hr = R30ExecuteXyzrhwStereo(
                device, state, leftDraw, rightDraw,
                "R30.6/DrawIndexedPrimitiveVB-ShadowXYZRHW");

            {
                InternalPassScope guard;
                bool restoreOk = SUCCEEDED(device->SetStreamSource(
                    0, vb, streamOffset, stride));
                restoreOk = SUCCEEDED(device->SetIndices(ib)) && restoreOk;
                if (!restoreOk)
                {
                    NoteRestoreFailure("R30.6 VB/IB restore");
                    R30ArmSafeFallback();
                }
            }
            ib->Release();
            vb->Release();
            return hr;
        }

        bool R30BuildScreenSpaceEyeConstants(
            IDirect3DDevice9* device,
            const OutRunVRRenderer::LatchedStereoFrame& stereo,
            R30ScreenSpaceKind screenKind,
            float original[16], float eyeConstants[2][16],
            float eyeScale[2], float eyeOffset[2]) noexcept
        {
            if (!device ||
                CurrentVertexShaderIdentity.load(std::memory_order_acquire) == 0)
                return false;

            if (screenKind == R30ScreenSpaceKind::PerspectiveHud ||
                screenKind == R30ScreenSpaceKind::WorldBillboard)
            {
                // R44: glyph/billboard batches commonly reuse one game c64 for
                // several consecutive draws. Use the original game upload, not
                // a live register that may already contain our head correction.
                if (!R44GetOwnedRawOverlayWvp(original))
                {
                    // Some flat result overlays never refresh c64 in the short
                    // ownership window. They are safe only as PerspectiveHud;
                    // preserve the old live fallback for those, but never for a
                    // spatial billboard.
                    if (screenKind == R30ScreenSpaceKind::WorldBillboard ||
                        FAILED(device->GetVertexShaderConstantF(
                            OutRunWvpRegister, original,
                            OutRunWvpRegisterCount)))
                        return false;
                }
            }
            else if (FAILED(device->GetVertexShaderConstantF(
                    OutRunWvpRegister, original, OutRunWvpRegisterCount)))
            {
                return false;
            }
            if (!R30BuildEyeAffine(stereo, eyeScale, eyeOffset))
                return false;

            D3DMATRIX uploadedT{};
            std::memcpy(&uploadedT, original, sizeof(uploadedT));
            const D3DMATRIX stockWvp = TransposeMatrix(uploadedT);
            if (!MatrixFinite(stockWvp))
                return false;

            float baseRaw[16]{};
            D3DMATRIX baseProjection{};
            D3DMATRIX inverseBaseProjection{};
            if (!OutRunVRRenderer::GetRendererBaseProjection(baseRaw))
                return false;
            std::memcpy(&baseProjection, baseRaw, sizeof(baseProjection));
            if (!MatrixFinite(baseProjection) ||
                !InvertMatrix(baseProjection, inverseBaseProjection))
                return false;

            const float centerEye[3]{
                0.5f * (stereo.eyeOffset[0][0] + stereo.eyeOffset[1][0]),
                0.5f * (stereo.eyeOffset[0][1] + stereo.eyeOffset[1][1]),
                0.5f * (stereo.eyeOffset[0][2] + stereo.eyeOffset[1][2])
            };
            const float identityOrientation[4]{
                0.0f, 0.0f, 0.0f, 1.0f
            };

            if (screenKind == R30ScreenSpaceKind::WorldBillboard)
            {
                // R41: this path is reached only after the verified/rebindable
                // renderer-world paths reject the current c64. Therefore the
                // stock WVP cannot be assumed to contain our latched head
                // correction. Rebuild the spatial billboard from its stock
                // WorldView, then apply the same common head inverse and full
                // head-local eye pose used by raw world geometry.
                const D3DMATRIX worldView =
                    MultiplyMatrix(stockWvp, inverseBaseProjection);
                if (!MatrixFinite(worldView))
                    return false;

                float headRaw[16]{};
                std::uint32_t headPoseSequence = 0;
                D3DMATRIX headInverse{};
                if (!OutRunVRRenderer::GetLatchedHeadInverse(
                        headRaw, headPoseSequence) ||
                    headPoseSequence != stereo.poseSequence)
                    return false;
                std::memcpy(&headInverse, headRaw, sizeof(headInverse));
                if (!MatrixFinite(headInverse))
                    return false;

                for (int eye = 0; eye < 2; ++eye)
                {
                    const D3DMATRIX eyePose =
                        MatrixFromQuaternionTranslation(
                            stereo.eyeOrientation[eye],
                            stereo.eyeOffset[eye],
                            Settings::VRWorldScale *
                                Settings::VRStereoDepth);
                    const D3DMATRIX eyeInverse = InverseRigid(eyePose);
                    const D3DMATRIX eyeProjection =
                        ProjectionFromFov(
                            baseProjection, stereo.eyeFov[eye]);
                    const D3DMATRIX corrected =
                        MultiplyMatrix(
                            MultiplyMatrix(
                                MultiplyMatrix(worldView, headInverse),
                                eyeInverse),
                            eyeProjection);
                    if (!MatrixFinite(corrected))
                        return false;
                    const D3DMATRIX correctedT = TransposeMatrix(corrected);
                    std::memcpy(eyeConstants[eye], &correctedT,
                        sizeof(correctedT));
                }
                return true;
            }

            if (screenKind != R30ScreenSpaceKind::Hud2D &&
                screenKind != R30ScreenSpaceKind::PerspectiveHud)
                return false;

            // R40: turn all screen HUD into one finite, recentered view plane.
            // This removes head-lock. HudScale/contain is applied before the
            // plane conversion, so perspective HUD such as the large 6th/6 uses
            // exactly the same size rule as ordinary HUD.
            float headRaw[16]{};
            std::uint32_t headPoseSequence = 0;
            D3DMATRIX headInverse{};
            if (!OutRunVRRenderer::GetLatchedHeadInverse(
                    headRaw, headPoseSequence) ||
                headPoseSequence != stereo.poseSequence)
                return false;
            std::memcpy(&headInverse, headRaw, sizeof(headInverse));
            if (!MatrixFinite(headInverse))
                return false;

            float hudScaleX = 1.0f;
            float hudScaleY = 1.0f;
            R30HudContainScale(stereo, hudScaleX, hudScaleY);
            D3DMATRIX hudScale = IdentityMatrix();
            hudScale._11 = hudScaleX;
            hudScale._22 = hudScaleY;
            const D3DMATRIX scaledClip =
                MultiplyMatrix(stockWvp, hudScale);

            constexpr float HudPlaneViewZ = -2.50f;
            const float planeClipW =
                HudPlaneViewZ * baseProjection._34 +
                baseProjection._44;
            const float planeClipZ =
                HudPlaneViewZ * baseProjection._33 +
                baseProjection._43;
            if (!std::isfinite(planeClipW) ||
                !std::isfinite(planeClipZ) ||
                std::fabs(planeClipW) <= 1.0e-4f)
                return false;
            const float planeNdcZ = planeClipZ / planeClipW;
            if (!std::isfinite(planeNdcZ))
                return false;

            D3DMATRIX depthReset = IdentityMatrix();
            depthReset._33 = 0.0f;
            depthReset._43 = planeNdcZ;
            const D3DMATRIX commonViewPlane =
                MultiplyMatrix(
                    MultiplyMatrix(scaledClip, depthReset),
                    inverseBaseProjection);
            if (!MatrixFinite(commonViewPlane))
                return false;

            for (int eye = 0; eye < 2; ++eye)
            {
                const float relativeEye[3]{
                    stereo.eyeOffset[eye][0] - centerEye[0],
                    stereo.eyeOffset[eye][1] - centerEye[1],
                    stereo.eyeOffset[eye][2] - centerEye[2]
                };
                const D3DMATRIX eyePose =
                    MatrixFromQuaternionTranslation(
                        identityOrientation, relativeEye,
                        Settings::VRWorldScale *
                            Settings::VRStereoDepth);
                const D3DMATRIX eyeInverse = InverseRigid(eyePose);
                const D3DMATRIX eyeProjection =
                    ProjectionFromFov(
                        baseProjection, stereo.eyeFov[eye]);
                const D3DMATRIX corrected =
                    MultiplyMatrix(
                        MultiplyMatrix(
                            MultiplyMatrix(commonViewPlane, headInverse),
                            eyeInverse),
                        eyeProjection);
                if (!MatrixFinite(corrected))
                    return false;
                const D3DMATRIX correctedT = TransposeMatrix(corrected);
                std::memcpy(eyeConstants[eye], &correctedT,
                    sizeof(correctedT));
            }
            return true;
        }

        template <typename ActualDraw>
        HRESULT R30TryScreenSpaceFovDraw(
            IDirect3DDevice9* device, ActualDraw&& actualDraw,
            const char* site)
        {
            if (!R30SafeStereoBase(device))
                return E_NOTIMPL;

            const R30ScreenSpaceKind screenKind =
                R30ClassifyScreenSpacePass(device);
            if (screenKind == R30ScreenSpaceKind::None)
                return E_NOTIMPL;

            if (!EnsureStereoResources(device))
                return E_NOTIMPL;

            if (TrackedDepthStencil &&
                (!RightDepthSynchronized || !RightStencilSynchronized))
                TryBootstrapRightDepthFromRecentClear(device);
            if (TrackedDepthStencil && !RightDepthSynchronized &&
                DepthTestActive(device))
                return E_NOTIMPL;
            if (TrackedDepthStencil && !RightStencilSynchronized &&
                StencilTestActive(device))
                return E_NOTIMPL;

            OutRunVRRenderer::LatchedStereoFrame stereo{};
            if (!OutRunVRRenderer::GetLatchedStereoFrame(stereo) ||
                stereo.poseSequence == 0)
                return E_NOTIMPL;

            // If perspective world geometry already established this Present's
            // pose sequence, screen-space correction must use that exact packet.
            if (FrameStereoPoseSequence != 0 &&
                FrameStereoPoseSequence != stereo.poseSequence)
                return E_NOTIMPL;

            float original[16]{};
            float eyeConstants[2][16]{};
            float eyeScale[2]{};
            float eyeOffset[2]{};
            if (!R30BuildScreenSpaceEyeConstants(device, stereo,
                    screenKind, original, eyeConstants,
                    eyeScale, eyeOffset))
            {
                ++R30ScreenSpaceBuildFailures;
                return E_NOTIMPL;
            }

            D3DVIEWPORT9 savedViewport{};
            if (FAILED(device->GetViewport(&savedViewport)))
                return E_NOTIMPL;

            // Capture the completed world eyes before the first recognized HUD
            // draw. Present then extracts glow from this snapshot, so bright HUD
            // text/icons are never themselves bloom sources.
            R30CaptureSkyGlowSceneBeforeHud(device);

            // From this point the draw is owned by R30. The steady-state frame
            // intentionally has no complete independent mono history.
            ++R9DrawCalls;
            R9MonoBackupGap = true;
            if (LeftDrawMayWriteDepth(device) ||
                LeftDrawMayWriteStencil(device))
                ++R9MainDepthContentSerial;

            bool leftWvpOk = false;
            {
                InternalPassScope guard;
                leftWvpOk = SetWvpOneRegisterAtATime(
                    device, eyeConstants[0]);
            }
            if (!leftWvpOk)
            {
                bool restored = false;
                {
                    InternalPassScope guard;
                    restored = SetWvpOneRegisterAtATime(device, original);
                }
                if (!restored)
                {
                    R9Poison(OutRunVR::StereoFailureRestoreFailed,
                        "R30/HUD-left-WVP-rollback");
                    R30ArmSafeFallback();
                    return E_FAIL;
                }
                --R9DrawCalls;
                return E_NOTIMPL;
            }

            const HRESULT leftHr = actualDraw();
            if (FAILED(leftHr))
            {
                bool restored = false;
                {
                    InternalPassScope guard;
                    restored = SetWvpOneRegisterAtATime(device, original);
                }
                R9Poison(OutRunVR::StereoFailureLeftDrawFailed,
                    site, leftHr);
                if (!restored)
                    NoteRestoreFailure("R30 HUD left draw c64");
                R30ArmSafeFallback();
                return leftHr;
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
                {
                    rightHr = SetDepthStencilSurfaceHook.stdcall<HRESULT>(
                        device, TrackedDepthStencil ? RightEyeDepth : nullptr);
                }
                if (SUCCEEDED(rightHr))
                    rightHr = device->SetViewport(&savedViewport);
                if (SUCCEEDED(rightHr) &&
                    !SetWvpOneRegisterAtATime(device, eyeConstants[1]))
                {
                    rightFailure =
                        OutRunVR::StereoFailureRightWvpUploadFailed;
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
            ++R30SafeTwoEyeDraws;
            ++R30ScreenSpaceFovDraws;
            if (screenKind == R30ScreenSpaceKind::Hud2D)
                ++R30Hud2DDraws;
            else if (screenKind == R30ScreenSpaceKind::PerspectiveHud)
                ++R30PerspectiveHudDraws;
            else if (screenKind == R30ScreenSpaceKind::WorldBillboard)
                ++R30WorldBillboardDraws;

            if (!R30FirstScreenSpaceLogged)
            {
                R30FirstScreenSpaceLogged = true;
                spdlog::info(
                    "VR R30 HUD: asymmetric-FOV convergence correction ACTIVE; common-centre offset[L/R]={:.4f}/{:.4f} hudScale={:.2f} sourceOverTarget={:.3f}",
                    eyeOffset[0], eyeOffset[1], R30HudScaleValue(),
                    R30HudAspectCompensation(stereo));
            }
            if (screenKind == R30ScreenSpaceKind::WorldBillboard &&
                !R30FirstFlatPerspectiveLogged)
            {
                R30FirstFlatPerspectiveLogged = true;
                spdlog::info(
                    "VR R30 FLAT EFFECT: depth-disabled alpha perspective overlay is world-locked on a finite virtual plane; R45 spatial vehicle/rank billboards use corrected outer-hook c64 ownership and rebuild stock WorldView with head+eye transform");
            }
            if (screenKind == R30ScreenSpaceKind::PerspectiveHud &&
                !R30FirstPerspectiveHudLogged)
            {
                R30FirstPerspectiveHudLogged = true;
                spdlog::info(
                    "VR R45 PERSPECTIVE HUD: outer-hook draw serial fixed; 6th/6 and flat white overlays use the raw game c64 before stereo head correction, then uniform HudScale on the world-locked finite HUD plane");
            }

            if (FAILED(rightHr))
            {
                FrameRightDrawFailed = true;
                InvalidateRightDepthStencilIfLeftMayWrite(device);
                R9Poison(rightFailure, site, rightHr);
                R30ArmSafeFallback();
            }
            if (!restoreOk)
            {
                InvalidateRightDepthStencilIfLeftMayWrite(device);
                NoteRestoreFailure("R30 HUD right-eye draw");
                R30ArmSafeFallback();
            }
            return leftHr;
        }

        template <typename ActualDraw, typename R29Draw>
        HRESULT R30GuardScreenSpace(IDirect3DDevice9* device,
            ActualDraw&& actualDraw, R29Draw&& r29Draw,
            const char* site)
        {
            const HRESULT hr = R30TryScreenSpaceFovDraw(
                device, std::forward<ActualDraw>(actualDraw), site);
            if (hr != E_NOTIMPL)
                return hr;
            ++R30ScreenSpaceFallbacks;
            return r29Draw();
        }

        HRESULT __stdcall DrawPrimitiveDestR30(IDirect3DDevice9* device,
            D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
        {
            const HRESULT xyzrhw = R30TryXyzrhwPrimitiveVB(
                device, type, startVertex, primitiveCount);
            if (xyzrhw != E_NOTIMPL)
                return xyzrhw;

            auto actual = [&]() {
                return DrawPrimitiveHook.stdcall<HRESULT>(
                    device, type, startVertex, primitiveCount);
            };
            auto r29 = [&]() {
                return R30DrawPrimitiveR29Hook.stdcall<HRESULT>(
                    device, type, startVertex, primitiveCount);
            };
            return R30GuardScreenSpace(
                device, actual, r29, "R30/DrawPrimitive");
        }

        HRESULT __stdcall DrawIndexedPrimitiveDestR30(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            INT baseVertexIndex, UINT minVertexIndex, UINT numVertices,
            UINT startIndex, UINT primitiveCount)
        {
            const HRESULT xyzrhw = R30TryXyzrhwIndexedPrimitiveVB(
                device, type, baseVertexIndex, minVertexIndex, numVertices,
                startIndex, primitiveCount);
            if (xyzrhw != E_NOTIMPL)
                return xyzrhw;

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
            return R30GuardScreenSpace(
                device, actual, r29, "R30/DrawIndexedPrimitive");
        }

        HRESULT __stdcall DrawPrimitiveUPDestR30(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            UINT primitiveCount, const void* data, UINT stride)
        {
            const HRESULT xyzrhw = R30TryXyzrhwPrimitiveUP(
                device, type, primitiveCount, data, stride);
            if (xyzrhw != E_NOTIMPL)
                return xyzrhw;

            auto actual = [&]() {
                return DrawPrimitiveUPHook.stdcall<HRESULT>(
                    device, type, primitiveCount, data, stride);
            };
            auto r29 = [&]() {
                return R30DrawPrimitiveUPR29Hook.stdcall<HRESULT>(
                    device, type, primitiveCount, data, stride);
            };
            return R30GuardScreenSpace(
                device, actual, r29, "R30/DrawPrimitiveUP");
        }

        HRESULT __stdcall DrawIndexedPrimitiveUPDestR30(
            IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
            UINT minVertexIndex, UINT numVertices, UINT primitiveCount,
            const void* indexData, D3DFORMAT indexFormat,
            const void* vertexData, UINT stride)
        {
            const HRESULT xyzrhw = R30TryXyzrhwIndexedPrimitiveUP(
                device, type, minVertexIndex, numVertices, primitiveCount,
                indexData, indexFormat, vertexData, stride);
            if (xyzrhw != E_NOTIMPL)
                return xyzrhw;

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
            return R30GuardScreenSpace(
                device, actual, r29, "R30/DrawIndexedPrimitiveUP");
        }

        void R30RollbackHooks() noexcept
        {
            R30ResetR29Hook = {};
            R30PresentR29Hook = {};
            R30DrawIndexedPrimitiveUPR29Hook = {};
            R30DrawPrimitiveUPR29Hook = {};
            R30DrawIndexedPrimitiveR29Hook = {};
            R30DrawPrimitiveR29Hook = {};
        }

        bool R30EnableHooks() noexcept
        {
            SafetyHookInline* hooks[]{
                &R30PresentR29Hook,
                &R30ResetR29Hook,
                &R30DrawPrimitiveR29Hook,
                &R30DrawIndexedPrimitiveR29Hook,
                &R30DrawPrimitiveUPR29Hook,
                &R30DrawIndexedPrimitiveUPR29Hook
            };
            for (auto* hook : hooks)
            {
                if (!*hook || !hook->enable().has_value())
                    return false;
            }
            return true;
        }

        DWORD WINAPI R30InstallThread(void*)
        {
            using State = OutRunVR::RuntimeEligibility::InstallState;
            R30InstallState.store(State::Pending, std::memory_order_release);

            for (int attempt = 0; attempt < 4800; ++attempt)
            {
                if (Game::D3DDevice_ptr && *Game::D3DDevice_ptr)
                    R30InstallBufferCreationHooks(*Game::D3DDevice_ptr);

                const auto r29 = R26InstallState.load(
                    std::memory_order_acquire);
                if (r29 == State::Failed)
                {
                    R30RollbackBufferShadowHooks();
                    R30InstallState.store(State::Failed,
                        std::memory_order_release);
                    HookManager::ReportAsyncResult("OpenXRVRStereoR30HUD", false);
                    spdlog::error(
                        "VR R30 HUD: R29 prerequisite failed; R29 remains active without screen-space FOV correction");
                    return 0;
                }

                if (r29 == State::Ready)
                {
                    const auto disabled = safetyhook::InlineHook::StartDisabled;
                    R30PresentR29Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&PresentDestR27),
                        PresentDestR30, disabled);
                    R30ResetR29Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&ResetDestR22),
                        ResetDestR30, disabled);
                    R30DrawPrimitiveR29Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveDestR26),
                        DrawPrimitiveDestR30, disabled);
                    R30DrawIndexedPrimitiveR29Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveDestR26),
                        DrawIndexedPrimitiveDestR30, disabled);
                    R30DrawPrimitiveUPR29Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawPrimitiveUPDestR26),
                        DrawPrimitiveUPDestR30, disabled);
                    R30DrawIndexedPrimitiveUPR29Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&DrawIndexedPrimitiveUPDestR26),
                        DrawIndexedPrimitiveUPDestR30, disabled);

                    if (!R30EnableHooks())
                    {
                        R30RollbackHooks();
                        R30RollbackBufferShadowHooks();
                        R30InstallState.store(State::Failed,
                            std::memory_order_release);
                        HookManager::ReportAsyncResult(
                            "OpenXRVRStereoR30HUD", false);
                        spdlog::error(
                            "VR R30 HUD: disabled-first hook transaction failed; R29 remains active");
                        return 0;
                    }

                    R30InstallState.store(State::Ready,
                        std::memory_order_release);
                    HookManager::ReportAsyncResult(
                        "OpenXRVRStereoR30HUD", true);
                    spdlog::info(
                        "VR R30 HUD: ScreenSpace2D correction READY with configurable common-center HUD scale current={:.2f}; R41 shader+XYZRHW finite HUD world-lock + spatial billboard rebuild active",
                        R30HudScaleValue());
                    return 0;
                }
                Sleep(25);
            }

            R30RollbackBufferShadowHooks();
            R30InstallState.store(State::Failed, std::memory_order_release);
            HookManager::ReportAsyncResult("OpenXRVRStereoR30HUD", false);
            spdlog::error(
                "VR R30 HUD: timed out waiting for R29; R29 remains active");
            return 0;
        }

        class VRStereoR30HudHook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRStereoR30HUD";
            }
            bool validate() override { return true; }
            bool apply() override
            {
                using State = OutRunVR::RuntimeEligibility::InstallState;
                R30InstallState.store(State::Pending,
                    std::memory_order_release);
                HANDLE thread = CreateThread(nullptr, 0,
                    R30InstallThread, nullptr, 0, nullptr);
                if (!thread)
                {
                    R30InstallState.store(State::Failed,
                        std::memory_order_release);
                    return false;
                }
                CloseHandle(thread);
                return true;
            }
            static VRStereoR30HudHook instance;
        };

        VRStereoR30HudHook VRStereoR30HudHook::instance;
    }
}


namespace OutRunVRStereo
{
    namespace
    {
        class VRR26HudSafeCompareMarkerHook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXVRR26HudSafeCompare";
            }
            bool validate() override { return true; }
            bool apply() override
            {
                spdlog::warn(
                    "VR R26+HUD SAFE TEST: R26/R23 world path + R30 HUD/XYZRHW/SkyGlow overlay ACTIVE");
                return true;
            }
            static VRR26HudSafeCompareMarkerHook instance;
        };
        VRR26HudSafeCompareMarkerHook
            VRR26HudSafeCompareMarkerHook::instance;
    }
}
