#pragma once

// R32 host-side DirectGPU safety staging.
//
// The R23 SafeEye path copied into the single active pair before waiting for the
// GPU fence. If that wait timed out, the queued copy could complete later while
// SafeFrameId still named the previous frame. R32 copies into an inactive pair
// and swaps it into the active SafeEye only after completion + ACK. Shared
// D3D9Ex handles are cached per ring slot/generation so fallback staging no
// longer re-opens the same resources every frame.

#include "d3d9ex_direct_passthrough.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace OutRunVrD3D9ExDirectPassthrough
{
    inline constexpr const char* R32SafeEyeBuildId =
        "D3D9Ex-safe-eye-ab-cache-R32-20260917";

    struct R32SharedSlotCache
    {
        std::uint32_t generation = 0;
        std::uint32_t leftHandle = 0;
        std::uint32_t rightHandle = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        ID3D11Texture2D* eye[2]{};
    };

    inline std::array<R32SharedSlotCache,
        OutRunVR::RenderFrameRingSize> R32SharedSlots{};
    inline ID3D11Texture2D* R32InactiveEye[2]{};
    inline ID3D11ShaderResourceView* R32InactiveSrv[2]{};
    inline DXGI_FORMAT R32InactiveFormat = DXGI_FORMAT_UNKNOWN;
    inline std::uint32_t R32InactiveWidth = 0;
    inline std::uint32_t R32InactiveHeight = 0;

    inline std::uint64_t R32SharedCacheHits = 0;
    inline std::uint64_t R32SharedCacheMisses = 0;
    inline std::uint64_t R32SafeSwaps = 0;
    inline std::uint64_t R32SafeTimeoutPreserves = 0;
    inline bool R32FirstSafeSwapLogged = false;
    inline bool R32FirstTimeoutPreserveLogged = false;

    inline void R32ReleaseSharedSlot(R32SharedSlotCache& slot) noexcept
    {
        ReleaseCom(slot.eye[0]);
        ReleaseCom(slot.eye[1]);
        slot = {};
    }

    inline void R32ReleaseInactiveEyes() noexcept
    {
        ReleaseCom(R32InactiveSrv[0]);
        ReleaseCom(R32InactiveEye[0]);
        ReleaseCom(R32InactiveSrv[1]);
        ReleaseCom(R32InactiveEye[1]);
        R32InactiveFormat = DXGI_FORMAT_UNKNOWN;
        R32InactiveWidth = 0;
        R32InactiveHeight = 0;
    }

    inline void R32ResetDirectCaches() noexcept
    {
        for (auto& slot : R32SharedSlots)
            R32ReleaseSharedSlot(slot);
        R32ReleaseInactiveEyes();
        R32SharedCacheHits = 0;
        R32SharedCacheMisses = 0;
        R32SafeSwaps = 0;
        R32SafeTimeoutPreserves = 0;
    }

    inline bool R32OpenSharedSlot(
        const OutRunVR::SharedRenderFrameState& frame,
        R32SharedSlotCache*& out) noexcept
    {
        out = nullptr;
        if (!OutRunVrFinalTest::Device)
            return false;

        const std::uint32_t slotIndex =
            frame.reserved[OutRunVR::RenderFrameDirectSlotIndex];
        const std::uint32_t generation =
            frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex];
        const std::uint32_t left =
            frame.reserved[OutRunVR::RenderFrameDirectLeftHandleIndex];
        const std::uint32_t right =
            frame.reserved[OutRunVR::RenderFrameDirectRightHandleIndex];
        const std::uint32_t width =
            frame.reserved[OutRunVR::RenderFrameDirectWidthIndex];
        const std::uint32_t height =
            frame.reserved[OutRunVR::RenderFrameDirectHeightIndex];
        const DXGI_FORMAT declared = ExpectedDeclaredFormat(
            frame.reserved[OutRunVR::RenderFrameDirectFormatIndex]);

        if (slotIndex >= R32SharedSlots.size() || !generation || !left ||
            !right || !width || !height || declared == DXGI_FORMAT_UNKNOWN)
            return false;

        auto& cache = R32SharedSlots[slotIndex];
        if (cache.eye[0] && cache.eye[1] &&
            cache.generation == generation &&
            cache.leftHandle == left && cache.rightHandle == right &&
            cache.width == width && cache.height == height &&
            cache.format == declared)
        {
            ++R32SharedCacheHits;
            out = &cache;
            return true;
        }

        ++R32SharedCacheMisses;
        R32ReleaseSharedSlot(cache);

        const std::uint32_t handles[2]{ left, right };
        for (int eye = 0; eye < 2; ++eye)
        {
            ID3D11Resource* resource = nullptr;
            const HANDLE handle = reinterpret_cast<HANDLE>(
                static_cast<std::uintptr_t>(handles[eye]));
            if (FAILED(OutRunVrFinalTest::Device->OpenSharedResource(
                    handle, __uuidof(ID3D11Resource),
                    reinterpret_cast<void**>(&resource))) || !resource)
            {
                R32ReleaseSharedSlot(cache);
                return false;
            }
            const HRESULT qi = resource->QueryInterface(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&cache.eye[eye]));
            resource->Release();
            if (FAILED(qi) || !cache.eye[eye])
            {
                R32ReleaseSharedSlot(cache);
                return false;
            }
        }

        D3D11_TEXTURE2D_DESC desc[2]{};
        cache.eye[0]->GetDesc(&desc[0]);
        cache.eye[1]->GetDesc(&desc[1]);
        const bool valid =
            desc[0].Width == width && desc[0].Height == height &&
            desc[1].Width == width && desc[1].Height == height &&
            desc[0].Format == declared && desc[1].Format == declared &&
            desc[0].SampleDesc.Count == 1 && desc[1].SampleDesc.Count == 1;
        if (!valid)
        {
            R32ReleaseSharedSlot(cache);
            return false;
        }

        cache.generation = generation;
        cache.leftHandle = left;
        cache.rightHandle = right;
        cache.width = width;
        cache.height = height;
        cache.format = declared;
        out = &cache;
        return true;
    }

    inline bool R32EnsureInactiveEyes(
        const D3D11_TEXTURE2D_DESC& source) noexcept
    {
        if (R32InactiveEye[0] && R32InactiveEye[1] &&
            R32InactiveSrv[0] && R32InactiveSrv[1] &&
            R32InactiveWidth == source.Width &&
            R32InactiveHeight == source.Height &&
            R32InactiveFormat == source.Format)
            return true;

        R32ReleaseInactiveEyes();
        D3D11_TEXTURE2D_DESC safe = source;
        safe.MipLevels = 1;
        safe.ArraySize = 1;
        safe.SampleDesc.Count = 1;
        safe.SampleDesc.Quality = 0;
        safe.Usage = D3D11_USAGE_DEFAULT;
        safe.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        safe.CPUAccessFlags = 0;
        safe.MiscFlags = 0;

        for (int eye = 0; eye < 2; ++eye)
        {
            if (FAILED(OutRunVrFinalTest::Device->CreateTexture2D(
                    &safe, nullptr, &R32InactiveEye[eye])) ||
                !R32InactiveEye[eye] ||
                FAILED(OutRunVrFinalTest::Device->CreateShaderResourceView(
                    R32InactiveEye[eye], nullptr, &R32InactiveSrv[eye])) ||
                !R32InactiveSrv[eye])
            {
                R32ReleaseInactiveEyes();
                return false;
            }
        }
        R32InactiveWidth = safe.Width;
        R32InactiveHeight = safe.Height;
        R32InactiveFormat = safe.Format;
        return true;
    }

    inline bool CopySharedFrameToSafeEyesR32(
        const OutRunVR::SharedRenderFrameState& frame) noexcept
    {
        if (!OutRunVrFinalTest::Context || !EnsureCopyFence())
            return false;

        R32SharedSlotCache* source = nullptr;
        if (!R32OpenSharedSlot(frame, source) || !source)
            return false;

        D3D11_TEXTURE2D_DESC desc{};
        source->eye[0]->GetDesc(&desc);
        if (!R32EnsureInactiveEyes(desc))
            return false;

        OutRunVrFinalTest::Context->CopyResource(
            R32InactiveEye[0], source->eye[0]);
        OutRunVrFinalTest::Context->CopyResource(
            R32InactiveEye[1], source->eye[1]);

        if (!WaitForCopyFence())
        {
            // The copy may still finish later, but it targets only the inactive
            // pair. SafeEye + SafeFrameId continue to describe the old image.
            ++R32SafeTimeoutPreserves;
            ++SafeCopyFailure;
            if (!R32FirstTimeoutPreserveLogged)
            {
                R32FirstTimeoutPreserveLogged = true;
                std::cerr
                    << "[D3D9Ex R32] SafeEye fence timeout preserved the previous active pair; queued copy cannot mutate SafeFrameId image identity\n";
            }
            return false;
        }

        if (!PublishCompletedFrame(frame))
        {
            ++SafeCopyFailure;
            return false;
        }

        for (int eye = 0; eye < 2; ++eye)
        {
            std::swap(SafeEye[eye], R32InactiveEye[eye]);
            std::swap(SafeEyeSrv[eye], R32InactiveSrv[eye]);
        }
        std::swap(SafeEyeFormat, R32InactiveFormat);
        std::swap(SafeEyeWidth, R32InactiveWidth);
        std::swap(SafeEyeHeight, R32InactiveHeight);

        SafeFrameId = frame.frameId;
        SafeTransportGeneration =
            frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex];
        ++SafeCopySuccess;
        ++R32SafeSwaps;
        if (!R32FirstSafeSwapLogged)
        {
            R32FirstSafeSwapLogged = true;
            std::cerr
                << "[D3D9Ex R32] SafeEye A/B staging + per-slot shared-resource cache ACTIVE; host-owned GPU eye copies + completion ACK active build="
                << R32SafeEyeBuildId << "\n";
        }
        return true;
    }

    inline bool EnsureSafeFrameR32(std::uint32_t frameId) noexcept
    {
        OutRunVR::SharedRenderFrameState frame{};
        if (!ReadFrameById(frameId, frame))
            return false;
        const std::uint32_t generation =
            frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex];
        if (frameId && generation && SafeFrameId == frameId &&
            SafeTransportGeneration == generation &&
            SafeEyeSrv[0] && SafeEyeSrv[1])
            return true;
        return CopySharedFrameToSafeEyesR32(frame);
    }
}

// Later R22/R23/R24 headers call EnsureSafeFrame unqualified after importing the
// DirectPassthrough namespace. Redirect those final safety paths to R32 A/B
// staging while leaving the validated R23 implementation available as fallback.
#define EnsureSafeFrame EnsureSafeFrameR32
