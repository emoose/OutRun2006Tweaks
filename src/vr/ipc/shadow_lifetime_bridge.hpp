#pragma once

#include <atomic>

namespace OutRunVR::IpcV3
{
    using ShadowBridgeStopCallback = void(*)() noexcept;

    // Header-only registration keeps the common proxy DLL independent of the
    // optional VR shadow-bridge translation unit. Generic wheel builds leave
    // this slot null; VR builds register their process-lifetime stop callback.
    // Release/acquire publication guarantees DllMain sees either no callback or
    // the fully published VR callback without linking the optional VR worker TU.
    inline std::atomic<ShadowBridgeStopCallback> ShadowBridgeStopCallbackSlot{nullptr};

    inline void RegisterShadowBridgeStopCallback(ShadowBridgeStopCallback callback) noexcept
    {
        ShadowBridgeStopCallbackSlot.store(callback, std::memory_order_release);
    }

    inline void RequestRegisteredShadowBridgeStop() noexcept
    {
        if (const auto callback = ShadowBridgeStopCallbackSlot.load(std::memory_order_acquire))
            callback();
    }
}
