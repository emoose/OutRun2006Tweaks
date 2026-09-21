#pragma once

// R13 arbitration between the Desktop-Duplication fallback and the D3D9Ex
// shared-eye ring. Shared producer textures are copied into host-owned D3D11
// textures before the producer slot is acknowledged for reuse.

#include "sbs_capture_override.hpp"
#include "../../../src/vr/ipc/direct_ack_r13.hpp"

#ifdef xrEndFrame
#undef xrEndFrame
#endif
#ifdef xrDestroySession
#undef xrDestroySession
#endif

#include <d3d9types.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace OutRunVrD3D9ExDirectPassthrough
{
    inline constexpr const char* BuildId = "D3D9Ex-direct-gpu-copy-R23-20260916";
    inline constexpr const char* LegacyBuildId = "D3D9Ex-direct-passthrough-20260915";
    inline constexpr ULONGLONG FallbackSourceMaxAgeMs = 250;

    // 90 Hz gives roughly 11.1 ms for a complete frame. A 25 ms synchronous
    // GPU fence wait could consume more than two frames before fallback. Keep
    // this bounded so a delayed direct copy fails closed to the classic path.
    inline constexpr ULONGLONG CopyFenceTimeoutMs = 8;

    inline std::uint64_t DirectPassFrames = 0;
    inline std::uint64_t FallbackFrames = 0;
    inline std::uint64_t DirectCandidateRejected = 0;
    inline std::uint64_t SafeCopySuccess = 0;
    inline std::uint64_t SafeCopyFailure = 0;
    inline std::uint64_t CopyFenceTimeout = 0;
    inline std::uint64_t StaleFallbackInvalidations = 0;
    inline bool FirstDirectPassLogged = false;
    inline bool FirstDirectRejectLogged = false;
    inline bool FirstGpuSafeAckLogged = false;
    inline bool FirstAckMappingLogged = false;
    inline bool FirstCopyFenceTimeoutLogged = false;
    inline bool FirstDeclaredFormatMismatchLogged = false;

    inline HANDLE PoseMapping = nullptr;
    inline const OutRunVR::SharedPoseState* PoseState = nullptr;
    inline HANDLE FrameMapping = nullptr;
    inline const OutRunVR::SharedRenderFrameRing* FrameRing = nullptr;
    inline HANDLE DirectAckMapping = nullptr;
    inline OutRunVR::R13::DirectGpuAckState* DirectAckState = nullptr;

    inline ID3D11Query* CopyFence = nullptr;
    inline ID3D11Texture2D* SafeEye[2]{};
    inline ID3D11ShaderResourceView* SafeEyeSrv[2]{};
    inline DXGI_FORMAT SafeEyeFormat = DXGI_FORMAT_UNKNOWN;
    inline std::uint32_t SafeEyeWidth = 0;
    inline std::uint32_t SafeEyeHeight = 0;
    inline std::uint32_t SafeFrameId = 0;
    inline std::uint32_t SafeTransportGeneration = 0;

    inline std::uint64_t LastObservedCaptureFresh = 0;
    inline ULONGLONG LastCaptureFreshMs = 0;

    template <typename T>
    inline void ReleaseCom(T*& value)
    {
        if (value)
        {
            value->Release();
            value = nullptr;
        }
    }

    struct DirectHostState
    {
        bool valid = false;
        std::uint32_t flags = 0;
        std::uint32_t hostPid = 0;
        std::uint32_t openedFrame = 0;
    };

    inline DXGI_FORMAT ExpectedDeclaredFormat(std::uint32_t raw) noexcept
    {
        switch (static_cast<D3DFORMAT>(raw))
        {
        case D3DFMT_A8R8G8B8:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case D3DFMT_A8B8G8R8:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case D3DFMT_A2B10G10R10:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case D3DFMT_A16B16G16R16F:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    }

    inline void ResetSafeEyes() noexcept
    {
        ReleaseCom(SafeEyeSrv[0]);
        ReleaseCom(SafeEye[0]);
        ReleaseCom(SafeEyeSrv[1]);
        ReleaseCom(SafeEye[1]);
        SafeEyeFormat = DXGI_FORMAT_UNKNOWN;
        SafeEyeWidth = SafeEyeHeight = 0;
        SafeFrameId = 0;
        SafeTransportGeneration = 0;
    }

    inline void CloseDirectAckMapping() noexcept
    {
        if (DirectAckState)
        {
            UnmapViewOfFile(DirectAckState);
            DirectAckState = nullptr;
        }
        if (DirectAckMapping)
        {
            CloseHandle(DirectAckMapping);
            DirectAckMapping = nullptr;
        }
    }

    inline void CloseReadMappings() noexcept
    {
        if (PoseState)
        {
            UnmapViewOfFile(PoseState);
            PoseState = nullptr;
        }
        if (PoseMapping)
        {
            CloseHandle(PoseMapping);
            PoseMapping = nullptr;
        }
        if (FrameRing)
        {
            UnmapViewOfFile(FrameRing);
            FrameRing = nullptr;
        }
        if (FrameMapping)
        {
            CloseHandle(FrameMapping);
            FrameMapping = nullptr;
        }
    }

    inline bool EnsurePoseState() noexcept
    {
        if (PoseState && PoseState->magic == OutRunVR::SharedMagic &&
            PoseState->protocolVersion == OutRunVR::SharedProtocolVersion &&
            PoseState->structSize == sizeof(OutRunVR::SharedPoseState))
            return true;

        if (PoseState)
        {
            UnmapViewOfFile(PoseState);
            PoseState = nullptr;
        }
        if (PoseMapping)
        {
            CloseHandle(PoseMapping);
            PoseMapping = nullptr;
        }

        PoseMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, OutRunVR::SharedMemoryName);
        if (!PoseMapping)
            return false;
        PoseState = static_cast<const OutRunVR::SharedPoseState*>(MapViewOfFile(
            PoseMapping, FILE_MAP_READ, 0, 0, sizeof(OutRunVR::SharedPoseState)));
        if (!PoseState)
        {
            CloseHandle(PoseMapping);
            PoseMapping = nullptr;
            return false;
        }
        if (PoseState->magic != OutRunVR::SharedMagic ||
            PoseState->protocolVersion != OutRunVR::SharedProtocolVersion ||
            PoseState->structSize != sizeof(OutRunVR::SharedPoseState))
        {
            UnmapViewOfFile(PoseState);
            PoseState = nullptr;
            CloseHandle(PoseMapping);
            PoseMapping = nullptr;
            return false;
        }
        return true;
    }

    inline bool EnsureFrameRing() noexcept
    {
        if (FrameRing && FrameRing->magic == OutRunVR::RenderFrameMagic &&
            FrameRing->protocolVersion == OutRunVR::RenderFrameProtocolVersion &&
            FrameRing->structSize == sizeof(OutRunVR::SharedRenderFrameRing) &&
            FrameRing->slotCount == OutRunVR::RenderFrameRingSize)
            return true;

        if (FrameRing)
        {
            UnmapViewOfFile(FrameRing);
            FrameRing = nullptr;
        }
        if (FrameMapping)
        {
            CloseHandle(FrameMapping);
            FrameMapping = nullptr;
        }
        FrameMapping = OpenFileMappingW(FILE_MAP_READ, FALSE,
            OutRunVR::RenderFrameMemoryName);
        if (!FrameMapping)
            return false;
        FrameRing = static_cast<const OutRunVR::SharedRenderFrameRing*>(MapViewOfFile(
            FrameMapping, FILE_MAP_READ, 0, 0,
            sizeof(OutRunVR::SharedRenderFrameRing)));
        if (!FrameRing)
        {
            CloseHandle(FrameMapping);
            FrameMapping = nullptr;
            return false;
        }
        if (FrameRing->magic != OutRunVR::RenderFrameMagic ||
            FrameRing->protocolVersion != OutRunVR::RenderFrameProtocolVersion ||
            FrameRing->structSize != sizeof(OutRunVR::SharedRenderFrameRing) ||
            FrameRing->slotCount != OutRunVR::RenderFrameRingSize)
        {
            CloseReadMappings();
            return false;
        }
        return true;
    }

    inline bool BeginAckWrite() noexcept
    {
        if (!DirectAckState)
            return false;
        LONG seq = InterlockedIncrement(
            reinterpret_cast<volatile LONG*>(&DirectAckState->sequence));
        if ((seq & 1) == 0)
            InterlockedIncrement(
                reinterpret_cast<volatile LONG*>(&DirectAckState->sequence));
        MemoryBarrier();
        return true;
    }

    inline void EndAckWrite() noexcept
    {
        MemoryBarrier();
        LONG seq = InterlockedIncrement(
            reinterpret_cast<volatile LONG*>(&DirectAckState->sequence));
        if (seq & 1)
            InterlockedIncrement(
                reinterpret_cast<volatile LONG*>(&DirectAckState->sequence));
    }

    inline bool EnsureDirectAckState() noexcept
    {
        if (DirectAckState &&
            DirectAckState->magic == OutRunVR::R13::DirectGpuAckMagic &&
            DirectAckState->version == OutRunVR::R13::DirectGpuAckVersion &&
            DirectAckState->structSize == sizeof(OutRunVR::R13::DirectGpuAckState) &&
            DirectAckState->hostPid == GetCurrentProcessId())
            return true;

        CloseDirectAckMapping();
        DirectAckMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(OutRunVR::R13::DirectGpuAckState)),
            OutRunVR::R13::DirectGpuAckName);
        if (!DirectAckMapping)
            return false;

        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        DirectAckState = static_cast<OutRunVR::R13::DirectGpuAckState*>(MapViewOfFile(
            DirectAckMapping, FILE_MAP_ALL_ACCESS, 0, 0,
            sizeof(OutRunVR::R13::DirectGpuAckState)));
        if (!DirectAckState)
        {
            CloseDirectAckMapping();
            return false;
        }

        const bool validExisting = existed &&
            DirectAckState->magic == OutRunVR::R13::DirectGpuAckMagic &&
            DirectAckState->version == OutRunVR::R13::DirectGpuAckVersion &&
            DirectAckState->structSize == sizeof(OutRunVR::R13::DirectGpuAckState);
        if (existed && !validExisting)
        {
            std::cerr
                << "[D3D9Ex R23] incompatible DirectGpuAck mapping; direct path disabled\n";
            CloseDirectAckMapping();
            return false;
        }

        if (!existed)
        {
            std::memset(DirectAckState, 0, sizeof(*DirectAckState));
            DirectAckState->version = OutRunVR::R13::DirectGpuAckVersion;
            DirectAckState->structSize = sizeof(*DirectAckState);
            DirectAckState->hostPid = GetCurrentProcessId();
            MemoryBarrier();
            DirectAckState->magic = OutRunVR::R13::DirectGpuAckMagic;
        }
        else
        {
            BeginAckWrite();
            DirectAckState->magic = OutRunVR::R13::DirectGpuAckMagic;
            DirectAckState->version = OutRunVR::R13::DirectGpuAckVersion;
            DirectAckState->structSize = sizeof(*DirectAckState);
            DirectAckState->hostPid = GetCurrentProcessId();
            DirectAckState->transportGeneration = 0;
            std::memset(DirectAckState->completedFrameId, 0,
                sizeof(DirectAckState->completedFrameId));
            EndAckWrite();
        }

        if (!FirstAckMappingLogged)
        {
            FirstAckMappingLogged = true;
            std::cerr
                << "[D3D9Ex R23] dedicated per-slot DirectGpuAck mapping ready\n";
        }
        return true;
    }

    inline DirectHostState ReadDirectHostState() noexcept
    {
        DirectHostState out{};
        if (!EnsurePoseState())
            return out;

        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::uint32_t before = PoseState->sequence;
            if (before & 1u)
                continue;
            MemoryBarrier();
            out.flags = PoseState->flags;
            out.hostPid = PoseState->hostPid;
            out.openedFrame = PoseState->hostDirectConsumedFrameId;
            MemoryBarrier();
            const std::uint32_t after = PoseState->sequence;
            if (before != after || (after & 1u))
                continue;

            constexpr std::uint32_t required =
                OutRunVR::HostAlive |
                OutRunVR::HostDirectGpuTransport |
                OutRunVR::HostDirectGpuReady;
            out.valid = out.hostPid == GetCurrentProcessId() &&
                (out.flags & required) == required;
            return out;
        }
        return {};
    }

    inline bool ReadFrameById(std::uint32_t frameId,
        OutRunVR::SharedRenderFrameState& out) noexcept
    {
        if (!frameId || !EnsureFrameRing())
            return false;

        for (int ringAttempt = 0; ringAttempt < 6; ++ringAttempt)
        {
            const std::uint32_t ringBefore = FrameRing->publishSequence;
            if (ringBefore & 1u)
                continue;

            bool found = false;
            OutRunVR::SharedRenderFrameState selected{};
            for (std::uint32_t i = 0; i < OutRunVR::RenderFrameRingSize && !found; ++i)
            {
                const auto& slot = FrameRing->slots[i];
                for (int attempt = 0; attempt < 4; ++attempt)
                {
                    const std::uint32_t before = slot.sequence;
                    if (before & 1u)
                        continue;
                    MemoryBarrier();
                    OutRunVR::SharedRenderFrameState candidate{};
                    std::memcpy(&candidate, &slot, sizeof(candidate));
                    MemoryBarrier();
                    const std::uint32_t after = slot.sequence;
                    if (before == after && !(after & 1u) &&
                        candidate.magic == OutRunVR::RenderFrameMagic &&
                        candidate.protocolVersion == OutRunVR::RenderFrameProtocolVersion &&
                        candidate.structSize == sizeof(candidate) &&
                        OutRunVR::RenderFrameRunIdentityMatches(*FrameRing, candidate) &&
                        candidate.frameId == frameId &&
                        candidate.state == OutRunVR::StereoSbsActive &&
                        (candidate.flags & OutRunVR::RenderFrameDirectGpuTransport) != 0 &&
                        (candidate.flags & OutRunVR::RenderFramePresentInFlight) == 0)
                    {
                        selected = candidate;
                        found = true;
                        break;
                    }
                }
            }

            MemoryBarrier();
            const std::uint32_t ringAfter = FrameRing->publishSequence;
            if (found && ringBefore == ringAfter && !(ringAfter & 1u))
            {
                out = selected;
                return true;
            }
        }
        return false;
    }

    inline bool IncomingProjectionValid(const XrFrameEndInfo* endInfo) noexcept
    {
        return endInfo && endInfo->layerCount > 0 && endInfo->layers &&
            endInfo->layers[0] &&
            endInfo->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    }

    inline bool EnsureCopyFence() noexcept
    {
        if (CopyFence)
            return true;
        if (!OutRunVrFinalTest::Device)
            return false;
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_EVENT;
        return SUCCEEDED(OutRunVrFinalTest::Device->CreateQuery(&desc, &CopyFence)) &&
            CopyFence;
    }

    inline bool WaitForCopyFence() noexcept
    {
        if (!CopyFence || !OutRunVrFinalTest::Context)
            return false;
        OutRunVrFinalTest::Context->End(CopyFence);
        OutRunVrFinalTest::Context->Flush();
        const ULONGLONG start = GetTickCount64();
        for (;;)
        {
            const HRESULT hr = OutRunVrFinalTest::Context->GetData(
                CopyFence, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_OK)
                return true;
            if (FAILED(hr) || GetTickCount64() - start >= CopyFenceTimeoutMs)
            {
                ++CopyFenceTimeout;
                if (!FirstCopyFenceTimeoutLogged)
                {
                    FirstCopyFenceTimeoutLogged = true;
                    std::cerr
                        << "[D3D9Ex R23] direct copy fence exceeded "
                        << CopyFenceTimeoutMs
                        << "ms VR budget; dropping direct candidate to fallback\n";
                }
                return false;
            }
            SwitchToThread();
        }
    }

    inline bool EnsureSafeEyes(const D3D11_TEXTURE2D_DESC& desc) noexcept
    {
        if (SafeEye[0] && SafeEye[1] && SafeEyeSrv[0] && SafeEyeSrv[1] &&
            SafeEyeWidth == desc.Width && SafeEyeHeight == desc.Height &&
            SafeEyeFormat == desc.Format)
            return true;

        ResetSafeEyes();
        D3D11_TEXTURE2D_DESC safe = desc;
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
                    &safe, nullptr, &SafeEye[eye])) || !SafeEye[eye] ||
                FAILED(OutRunVrFinalTest::Device->CreateShaderResourceView(
                    SafeEye[eye], nullptr, &SafeEyeSrv[eye])) || !SafeEyeSrv[eye])
            {
                ResetSafeEyes();
                return false;
            }
        }
        SafeEyeWidth = safe.Width;
        SafeEyeHeight = safe.Height;
        SafeEyeFormat = safe.Format;
        return true;
    }

    inline bool PublishCompletedFrame(
        const OutRunVR::SharedRenderFrameState& frame) noexcept
    {
        if (!EnsureDirectAckState())
            return false;
        const std::uint32_t slot =
            frame.reserved[OutRunVR::RenderFrameDirectSlotIndex];
        const std::uint32_t generation =
            frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex];
        if (slot >= OutRunVR::R13::DirectGpuAckRingSize || !generation ||
            !frame.frameId)
            return false;

        BeginAckWrite();
        if (DirectAckState->transportGeneration != generation)
        {
            DirectAckState->transportGeneration = generation;
            std::memset(DirectAckState->completedFrameId, 0,
                sizeof(DirectAckState->completedFrameId));
        }
        DirectAckState->hostPid = GetCurrentProcessId();
        DirectAckState->completedFrameId[slot] = frame.frameId;
        EndAckWrite();
        return true;
    }

    inline bool CopySharedFrameToSafeEyes(
        const OutRunVR::SharedRenderFrameState& frame) noexcept
    {
        if (!OutRunVrFinalTest::Device || !OutRunVrFinalTest::Context ||
            !EnsureCopyFence())
            return false;

        const std::uint32_t leftRaw =
            frame.reserved[OutRunVR::RenderFrameDirectLeftHandleIndex];
        const std::uint32_t rightRaw =
            frame.reserved[OutRunVR::RenderFrameDirectRightHandleIndex];
        const std::uint32_t width =
            frame.reserved[OutRunVR::RenderFrameDirectWidthIndex];
        const std::uint32_t height =
            frame.reserved[OutRunVR::RenderFrameDirectHeightIndex];
        const DXGI_FORMAT declared = ExpectedDeclaredFormat(
            frame.reserved[OutRunVR::RenderFrameDirectFormatIndex]);
        if (!leftRaw || !rightRaw || !width || !height ||
            declared == DXGI_FORMAT_UNKNOWN)
            return false;

        ID3D11Texture2D* shared[2]{};
        bool ok = true;
        const std::uint32_t handles[2]{ leftRaw, rightRaw };
        for (int eye = 0; eye < 2 && ok; ++eye)
        {
            ID3D11Resource* resource = nullptr;
            const HANDLE handle = reinterpret_cast<HANDLE>(
                static_cast<std::uintptr_t>(handles[eye]));
            if (FAILED(OutRunVrFinalTest::Device->OpenSharedResource(
                    handle, __uuidof(ID3D11Resource),
                    reinterpret_cast<void**>(&resource))) || !resource)
            {
                ok = false;
                break;
            }
            const HRESULT qi = resource->QueryInterface(
                __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&shared[eye]));
            resource->Release();
            if (FAILED(qi) || !shared[eye])
                ok = false;
        }

        D3D11_TEXTURE2D_DESC desc[2]{};
        if (ok)
        {
            shared[0]->GetDesc(&desc[0]);
            shared[1]->GetDesc(&desc[1]);
            ok = desc[0].Width == width && desc[0].Height == height &&
                desc[1].Width == width && desc[1].Height == height &&
                desc[0].Format == desc[1].Format &&
                desc[0].Format == declared &&
                desc[0].SampleDesc.Count == 1 &&
                desc[1].SampleDesc.Count == 1 &&
                EnsureSafeEyes(desc[0]);
            if (!ok && desc[0].Format != declared &&
                !FirstDeclaredFormatMismatchLogged)
            {
                FirstDeclaredFormatMismatchLogged = true;
                std::cerr
                    << "[D3D9Ex R23] Frame.v2 declared direct format does not match opened D3D11 texture; candidate rejected\n";
            }
        }

        if (ok)
        {
            OutRunVrFinalTest::Context->CopyResource(SafeEye[0], shared[0]);
            OutRunVrFinalTest::Context->CopyResource(SafeEye[1], shared[1]);
            ok = WaitForCopyFence();
        }

        ReleaseCom(shared[0]);
        ReleaseCom(shared[1]);
        if (!ok || !PublishCompletedFrame(frame))
        {
            ++SafeCopyFailure;
            return false;
        }

        SafeFrameId = frame.frameId;
        SafeTransportGeneration =
            frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex];
        ++SafeCopySuccess;
        if (!FirstGpuSafeAckLogged)
        {
            FirstGpuSafeAckLogged = true;
            std::cerr
                << "[D3D9Ex R23] host-owned GPU eye copies + completion ACK active frame="
                << frame.frameId
                << "; dedicated per-slot ACK allows shared ring reuse\n";
        }
        return true;
    }

    inline bool EnsureSafeFrame(std::uint32_t frameId) noexcept
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
        return CopySharedFrameToSafeEyes(frame);
    }

    inline bool RenderSafeProjection(XrSession session,
        const XrFrameEndInfo* endInfo,
        XrCompositionLayerProjection& projection,
        std::array<XrCompositionLayerProjectionView, 2>& views) noexcept
    {
        using namespace OutRunVrSbsCaptureOverride;
        if (!IncomingProjectionValid(endInfo) || !SafeEyeSrv[0] || !SafeEyeSrv[1])
            return false;
        const auto* incoming = reinterpret_cast<const XrCompositionLayerProjection*>(
            endInfo->layers[0]);
        if (incoming->viewCount < 2 || !incoming->views)
            return false;

        const std::uint32_t width = std::max(
            1, incoming->views[0].subImage.imageRect.extent.width);
        const std::uint32_t height = std::max(
            1, incoming->views[0].subImage.imageRect.extent.height);
        if (!EnsureSwapchain(Projection, session, width, height, 2) ||
            !CreateShaders())
            return false;

        std::uint32_t image = 0;
        if (!Acquire(Projection, image))
            return false;
        if (image >= Projection.rtvs.size())
        {
            Release(Projection);
            return false;
        }

        ID3D11ShaderResourceView* savedSrv = SourceSrv;
        const DXGI_FORMAT savedFormat = SourceFormat;
        bool ok = true;
        for (int eye = 0; eye < 2; ++eye)
        {
            SourceSrv = SafeEyeSrv[eye];
            SourceFormat = SafeEyeFormat;
            ok = RenderTo(Projection.rtvs[image][eye], Projection.width,
                Projection.height, UvRect{ 0.f, 0.f, 1.f, 1.f }) && ok;
        }
        SourceSrv = savedSrv;
        SourceFormat = savedFormat;
        if (OutRunVrFinalTest::Context)
            OutRunVrFinalTest::Context->Flush();
        Release(Projection);
        if (!ok)
            return false;

        projection = *incoming;
        for (int eye = 0; eye < 2; ++eye)
        {
            views[eye] = incoming->views[eye];
            views[eye].subImage.swapchain = Projection.handle;
            views[eye].subImage.imageRect.offset = { 0, 0 };
            views[eye].subImage.imageRect.extent = {
                static_cast<std::int32_t>(Projection.width),
                static_cast<std::int32_t>(Projection.height)
            };
            views[eye].subImage.imageArrayIndex = eye;
        }
        projection.viewCount = 2;
        projection.views = views.data();
        return true;
    }

    inline void ObserveCaptureFreshness() noexcept
    {
        using namespace OutRunVrSbsCaptureOverride;
        if (CaptureFresh != LastObservedCaptureFresh)
        {
            LastObservedCaptureFresh = CaptureFresh;
            LastCaptureFreshMs = GetTickCount64();
        }
    }

    inline void InvalidateStaleFallbackSource() noexcept
    {
        using namespace OutRunVrSbsCaptureOverride;
        ObserveCaptureFreshness();
        if (HaveSource && LastCaptureFreshMs != 0 &&
            GetTickCount64() - LastCaptureFreshMs > FallbackSourceMaxAgeMs)
        {
            HaveSource = false;
            ++StaleFallbackInvalidations;
            std::cerr << "[R23] stale Desktop Duplication source invalidated after "
                      << FallbackSourceMaxAgeMs << "ms without a fresh capture\n";
        }
    }

    inline void ResetR13HostState() noexcept
    {
        ResetSafeEyes();
        ReleaseCom(CopyFence);
        CloseDirectAckMapping();
        CloseReadMappings();
        LastObservedCaptureFresh = 0;
        LastCaptureFreshMs = 0;
    }

    inline XrResult XRAPI_CALL EndFrame(XrSession session,
        const XrFrameEndInfo* endInfo)
    {
        const DirectHostState state = ReadDirectHostState();
        const bool directCandidate = IncomingProjectionValid(endInfo) &&
            state.valid && state.openedFrame != 0;

        if (directCandidate && EnsureSafeFrame(state.openedFrame))
        {
            XrCompositionLayerProjection projection{};
            std::array<XrCompositionLayerProjectionView, 2> views{};
            if (RenderSafeProjection(session, endInfo, projection, views))
            {
                const XrCompositionLayerBaseHeader* layer =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                XrFrameEndInfo patched = *endInfo;
                patched.layerCount = 1;
                patched.layers = &layer;
                ++DirectPassFrames;
                if (!FirstDirectPassLogged)
                {
                    FirstDirectPassLogged = true;
                    std::cerr
                        << "[D3D9Ex] DIRECT GPU-COPY projection passthrough ACTIVE build="
                        << BuildId << " legacy=" << LegacyBuildId
                        << " frame=" << state.openedFrame
                        << "; shared eyes copied to host-owned GPU textures\n";
                }
                return OutRunVrFinalTest::EndFrame(session, &patched);
            }
        }

        if (directCandidate)
        {
            ++DirectCandidateRejected;
            if (!FirstDirectRejectLogged)
            {
                FirstDirectRejectLogged = true;
                std::cerr
                    << "[D3D9Ex R23] direct frame could not be staged/rendered safely; keeping Desktop Duplication fallback\n";
            }
        }

        ++FallbackFrames;
        InvalidateStaleFallbackSource();
        const XrResult result =
            OutRunVrSbsCaptureOverride::EndFrame(session, endInfo);
        ObserveCaptureFreshness();
        return result;
    }

    inline XrResult XRAPI_CALL DestroySession(XrSession session)
    {
        ResetR13HostState();
        return OutRunVrSbsCaptureOverride::DestroySession(session);
    }
}

#define xrEndFrame OutRunVrD3D9ExDirectPassthrough::EndFrame
#define xrDestroySession OutRunVrD3D9ExDirectPassthrough::DestroySession
