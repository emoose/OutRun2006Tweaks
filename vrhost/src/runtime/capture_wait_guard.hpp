#pragma once

// R24 black-screen recovery: Desktop Duplication is sampled from inside the
// OpenXR frame loop. The legacy compositor intentionally used a 1000 ms wait
// before its first successful capture, which can starve xrEndFrame long enough
// for the HMD compositor to show only black. Keep zero/short waits unchanged but
// clamp any long AcquireNextFrame request to a tiny VR-safe budget.
//
// dxgi1_5.h is included before the macro is installed so COM interface method
// declarations are never rewritten. Subsequent main.cpp/main_r23.cpp call sites
// are the only tokens affected by the compatibility macro below.

#include <Windows.h>
#include <dxgi1_5.h>

#include <algorithm>

namespace OutRunVrCaptureWaitGuard
{
    inline constexpr DWORD MaxAcquireWaitMs = 2;

    inline DWORD ClampDesktopDuplicationWait(DWORD requested) noexcept
    {
        return std::min(requested, MaxAcquireWaitMs);
    }
}

// Self-token suppression keeps the underlying COM method name intact while
// replacing only its timeout argument, matching the established ReleaseFrame
// instrumentation technique used by the capture fallback layer.
#define AcquireNextFrame(timeoutMs, frameInfo, resource) \
    AcquireNextFrame(OutRunVrCaptureWaitGuard::ClampDesktopDuplicationWait(timeoutMs), \
        frameInfo, resource)
