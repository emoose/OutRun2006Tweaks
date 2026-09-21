#pragma once

// Bound XR_INFINITE_DURATION waits so a wedged runtime/driver cannot leave the
// host process alive forever while the headset shows only black. Finite waits
// requested by callers are preserved exactly. Infinite waits are sliced into
// short waits and converted to XR_ERROR_RUNTIME_FAILURE after the total budget;
// callers that can degrade return to their fallback path, while the main host
// exits through its existing exception/cleanup path instead of hanging.

#include <Windows.h>
#include <openxr/openxr.h>

#include <atomic>
#include <cstdint>
#include <iostream>

namespace OutRunVrBoundedSwapchainWait
{
    inline constexpr XrDuration SliceNs = 20'000'000; // 20 ms
    inline constexpr ULONGLONG TotalBudgetMs = 250;
    inline std::atomic<bool> FirstBudgetExceededLogged{ false };

    inline XrResult XRAPI_CALL WaitSwapchainImage(
        XrSwapchain swapchain,
        const XrSwapchainImageWaitInfo* info) noexcept
    {
        if (!info || info->timeout != XR_INFINITE_DURATION)
            return ::xrWaitSwapchainImage(swapchain, info);

        const ULONGLONG start = GetTickCount64();
        for (;;)
        {
            XrSwapchainImageWaitInfo bounded = *info;
            bounded.timeout = SliceNs;
            const XrResult result = ::xrWaitSwapchainImage(swapchain, &bounded);
            if (result != XR_TIMEOUT_EXPIRED)
                return result;

            if (GetTickCount64() - start >= TotalBudgetMs)
            {
                if (!FirstBudgetExceededLogged.exchange(
                        true, std::memory_order_acq_rel))
                {
                    std::cerr
                        << "[R24] xrWaitSwapchainImage exceeded 250ms; aborting the wait so the host can recover/exit instead of hanging on a black frame\n";
                }
                return XR_ERROR_RUNTIME_FAILURE;
            }
            SwitchToThread();
        }
    }
}

#define xrWaitSwapchainImage OutRunVrBoundedSwapchainWait::WaitSwapchainImage
