#pragma once

// R20 production OpenXR compatibility/state shim.
//
// VirtualDesktopXR 1.0.x is the primary runtime for this project. Keep the
// application API request on the OpenXR 1.0 baseline and retain only the small
// amount of state required by the production SBS capture layer. The old R9
// alternating-color no-layer test swapchain is intentionally gone: production
// xrEndFrame is a direct pass-through here and sbs_capture_override.hpp owns the
// only production end-frame replacement.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <d3d11.h>

#ifndef XR_USE_PLATFORM_WIN32
#define OUTRUN_R20_DEFINED_XR_PLATFORM 1
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define OUTRUN_R20_DEFINED_XR_D3D11 1
#define XR_USE_GRAPHICS_API_D3D11
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifdef OUTRUN_R20_DEFINED_XR_PLATFORM
#undef XR_USE_PLATFORM_WIN32
#undef OUTRUN_R20_DEFINED_XR_PLATFORM
#endif
#ifdef OUTRUN_R20_DEFINED_XR_D3D11
#undef XR_USE_GRAPHICS_API_D3D11
#undef OUTRUN_R20_DEFINED_XR_D3D11
#endif

#include <cstdint>
#include <cstring>
#include <iostream>

#include "vr_shared.hpp"

#ifdef XR_CURRENT_API_VERSION
#undef XR_CURRENT_API_VERSION
#endif
#define XR_CURRENT_API_VERSION XR_MAKE_VERSION(1, 0, 0)

static_assert(XR_VERSION_MAJOR(XR_CURRENT_API_VERSION) == 1 &&
              XR_VERSION_MINOR(XR_CURRENT_API_VERSION) == 0,
    "OutRun VR host must request the OpenXR 1.0 application API baseline");

namespace OutRunVrFinalTest
{
    // Name retained because the R19 production capture layer consumed this
    // helper namespace. The contents are production-only in R20.
    inline constexpr const char* BuildId = "R20-production-host-20260916";

    inline XrSession Session = XR_NULL_HANDLE;
    // BaseLocalSpace is the runtime's original LOCAL origin. LocalSpace is the
    // application-visible origin and may be replaced by user recenter.
    inline XrSpace BaseLocalSpace = XR_NULL_HANDLE;
    inline XrSpace LocalSpace = XR_NULL_HANDLE;
    inline ID3D11Device* Device = nullptr;
    inline ID3D11DeviceContext* Context = nullptr;

    inline HANDLE FrameMapping = nullptr;
    inline const OutRunVR::SharedRenderFrameRing* FrameRing = nullptr;

    inline void CloseFrameMapping() noexcept
    {
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

    inline void ReleaseGraphicsBinding() noexcept
    {
        if (Context)
        {
            Context->Release();
            Context = nullptr;
        }
        if (Device)
        {
            Device->Release();
            Device = nullptr;
        }
    }

    inline void EnsureFrameMapping() noexcept
    {
        if (FrameRing)
            return;
        FrameMapping = OpenFileMappingW(
            FILE_MAP_READ, FALSE, OutRunVR::RenderFrameMemoryName);
        if (!FrameMapping)
            return;
        FrameRing = static_cast<const OutRunVR::SharedRenderFrameRing*>(
            MapViewOfFile(FrameMapping, FILE_MAP_READ, 0, 0,
                sizeof(OutRunVR::SharedRenderFrameRing)));
        if (!FrameRing)
        {
            CloseHandle(FrameMapping);
            FrameMapping = nullptr;
        }
    }

    inline bool ReadLatestFrame(OutRunVR::SharedRenderFrameState& out,
        std::uint32_t& publishSequence) noexcept
    {
        EnsureFrameMapping();
        if (!FrameRing || FrameRing->magic != OutRunVR::RenderFrameMagic ||
            FrameRing->protocolVersion != OutRunVR::RenderFrameProtocolVersion ||
            FrameRing->structSize != sizeof(OutRunVR::SharedRenderFrameRing) ||
            FrameRing->slotCount != OutRunVR::RenderFrameRingSize)
            return false;

        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::uint32_t before = FrameRing->publishSequence;
            if (before & 1u)
                continue;
            MemoryBarrier();
            const std::uint32_t slot = FrameRing->latestSlot;
            if (slot >= OutRunVR::RenderFrameRingSize)
                return false;
            const auto& src = FrameRing->slots[slot];
            const std::uint32_t frameBefore = src.sequence;
            if (frameBefore & 1u)
                continue;
            MemoryBarrier();
            std::memcpy(&out, &src, sizeof(out));
            MemoryBarrier();
            const std::uint32_t frameAfter = src.sequence;
            const std::uint32_t after = FrameRing->publishSequence;
            if (before == after && frameBefore == frameAfter &&
                !(after & 1u) && !(frameAfter & 1u))
            {
                publishSequence = after;
                return out.magic == OutRunVR::RenderFrameMagic &&
                    out.protocolVersion == OutRunVR::RenderFrameProtocolVersion &&
                    out.structSize == sizeof(out) &&
                    OutRunVR::RenderFrameRunIdentityMatches(*FrameRing, out);
            }
        }
        return false;
    }

    inline XrResult XRAPI_CALL CreateInstance(
        const XrInstanceCreateInfo* info, XrInstance* instance)
    {
        const XrVersion v = info ? info->applicationInfo.apiVersion : 0;
        std::cerr << "[R20] build=" << BuildId
                  << " requestedApi=" << XR_VERSION_MAJOR(v) << "."
                  << XR_VERSION_MINOR(v) << "." << XR_VERSION_PATCH(v)
                  << " testPattern=removed\n";
        const XrResult result = ::xrCreateInstance(info, instance);
        if (XR_SUCCEEDED(result) && instance && *instance != XR_NULL_HANDLE)
        {
            XrInstanceProperties props{ XR_TYPE_INSTANCE_PROPERTIES };
            if (XR_SUCCEEDED(::xrGetInstanceProperties(*instance, &props)))
            {
                std::cerr << "[R20] runtime=" << props.runtimeName
                          << " runtimeVersion="
                          << XR_VERSION_MAJOR(props.runtimeVersion) << "."
                          << XR_VERSION_MINOR(props.runtimeVersion) << "."
                          << XR_VERSION_PATCH(props.runtimeVersion) << "\n";
            }
        }
        return result;
    }

    inline XrResult XRAPI_CALL CreateSession(XrInstance instance,
        const XrSessionCreateInfo* info, XrSession* session)
    {
        const XrResult result = ::xrCreateSession(instance, info, session);
        if (XR_FAILED(result) || !session)
            return result;

        ReleaseGraphicsBinding();
        Session = *session;
        const XrBaseInStructure* next = info
            ? reinterpret_cast<const XrBaseInStructure*>(info->next) : nullptr;
        while (next)
        {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
            {
                const auto* binding =
                    reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
                if (binding->device)
                {
                    Device = binding->device;
                    Device->AddRef();
                    Device->GetImmediateContext(&Context);
                }
                break;
            }
            next = next->next;
        }
        std::cerr << "[R20] xrCreateSession ok d3d11="
                  << (Device && Context ? "yes" : "no") << "\n";
        return result;
    }

    inline XrResult XRAPI_CALL CreateReferenceSpace(XrSession session,
        const XrReferenceSpaceCreateInfo* info, XrSpace* space)
    {
        const XrResult result = ::xrCreateReferenceSpace(session, info, space);
        if (XR_SUCCEEDED(result) && info && space &&
            info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
        {
            if (BaseLocalSpace == XR_NULL_HANDLE)
                BaseLocalSpace = *space;
            LocalSpace = *space;
        }
        return result;
    }

    inline XrResult XRAPI_CALL DestroySession(XrSession session)
    {
        CloseFrameMapping();
        // main.cpp destroys the active LocalSpace before the session. If a user
        // recenter replaced it, the immutable base LOCAL is a second handle and
        // remains ours to release here.
        if (BaseLocalSpace != XR_NULL_HANDLE &&
            BaseLocalSpace != LocalSpace)
            ::xrDestroySpace(BaseLocalSpace);
        BaseLocalSpace = XR_NULL_HANDLE;
        LocalSpace = XR_NULL_HANDLE;
        Session = XR_NULL_HANDLE;
        ReleaseGraphicsBinding();
        return ::xrDestroySession(session);
    }

    inline XrResult XRAPI_CALL EndFrame(XrSession session,
        const XrFrameEndInfo* endInfo)
    {
        // Production R20 has no fallback diagnostic layer here. The R19/R20 SBS
        // capture override may patch a valid game/theater layer, then calls this
        // function to submit it directly to the OpenXR loader.
        return ::xrEndFrame(session, endInfo);
    }
}

// Redirect only the host translation units compiled with this forced include.
// sbs_capture_override.hpp intentionally undefines xrEndFrame/xrDestroySession
// and then installs its production wrappers above these compatibility helpers.
#define xrCreateInstance OutRunVrFinalTest::CreateInstance
#define xrCreateSession OutRunVrFinalTest::CreateSession
#define xrCreateReferenceSpace OutRunVrFinalTest::CreateReferenceSpace
#define xrDestroySession OutRunVrFinalTest::DestroySession
#define xrEndFrame OutRunVrFinalTest::EndFrame
