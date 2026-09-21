#pragma once

#include <atomic>
#include <cstdint>

namespace OutRunVR::RuntimeEligibility
{
    // Keep host freshness identical to the validated pose reader. All game-side
    // VR decisions consume this common authority instead of maintaining
    // independent clocks.
    inline constexpr std::int64_t HostStaleMs = 250;

    enum class InstallState : std::uint32_t
    {
        Pending = 0,
        Ready = 1,
        Failed = 2
    };

    inline bool IsReady(const std::atomic<InstallState>& state) noexcept
    {
        return state.load(std::memory_order_acquire) == InstallState::Ready;
    }

    inline bool IsFailed(const std::atomic<InstallState>& state) noexcept
    {
        return state.load(std::memory_order_acquire) == InstallState::Failed;
    }

    inline std::atomic<bool> HostFresh{ false };
    inline std::atomic<bool> HostRenderable{ false };
    inline std::atomic<bool> StereoAllowed{ false };
    inline std::atomic<bool> RecoveryPending{ true };

    // Recovery is deliberately two-stage. A passive, game-only clear may arm
    // the NEXT frame to latch a fresh pose, but it must not enable WVP injection
    // or stereo surface writes by itself. The next frame still has to prove a
    // current full color/depth/stencil baseline before StereoAllowed opens.
    inline std::atomic<bool> RecoveryPoseWarmup{ false };

    // Compatibility layers can hold stereo closed independently of host freshness
    // and baseline recovery. ResetEx replay health uses this gate so a later
    // BaselineVerified() cannot reopen injection while classic state is unsafe.
    inline std::atomic<bool> ExternalSafetyBlock{ false };

    inline void SetExternalSafetyBlock(bool blocked) noexcept
    {
        ExternalSafetyBlock.store(blocked, std::memory_order_release);
        if (blocked)
        {
            StereoAllowed.store(false, std::memory_order_release);
            RecoveryPending.store(true, std::memory_order_release);
            RecoveryPoseWarmup.store(false, std::memory_order_release);
        }
    }

    // R22/R23 are the final game-side safety overlays. Earlier layers may see a
    // fresh host or plausible clear while those hooks are still being installed,
    // but that must never make stereo/WVP eligible.
    inline std::atomic<bool> SafetyOverlayReady{ false };

    inline void FailClosed() noexcept
    {
        HostFresh.store(false, std::memory_order_release);
        HostRenderable.store(false, std::memory_order_release);
        StereoAllowed.store(false, std::memory_order_release);
        RecoveryPending.store(true, std::memory_order_release);
        RecoveryPoseWarmup.store(false, std::memory_order_release);
    }

    inline void MarkSafetyOverlayUnavailable() noexcept
    {
        SafetyOverlayReady.store(false, std::memory_order_release);
        FailClosed();
    }

    inline void MarkSafetyOverlayInstalled() noexcept
    {
        // Never inherit a baseline observed before the final callbacks became
        // authoritative. Force a fresh passive observation and next-frame proof.
        FailClosed();
        SafetyOverlayReady.store(true, std::memory_order_release);
    }

    inline void ObserveFreshHost() noexcept
    {
        HostFresh.store(true, std::memory_order_release);
        HostRenderable.store(true, std::memory_order_release);
        // Do not reopen stereo here. A fresh host after a hard stall still
        // needs a newly verified baseline.
    }

    inline void ObserveSoftHostSuspend() noexcept
    {
        // A transient shouldRender=false with a fresh, visible host is compositor
        // scheduling advice, not source invalidation. Keep the verified game-side
        // stereo source active so the host can immediately reuse/capture it when
        // shouldRender returns. The x64 host remains the sole authority for
        // deciding whether the current OpenXR frame actually submits a layer.
        HostFresh.store(true, std::memory_order_release);
        HostRenderable.store(true, std::memory_order_release);
    }

    inline void ArmRecoveryPoseWarmup() noexcept
    {
        if (!SafetyOverlayReady.load(std::memory_order_acquire) ||
            !HostFresh.load(std::memory_order_acquire) ||
            !HostRenderable.load(std::memory_order_acquire) ||
            ExternalSafetyBlock.load(std::memory_order_acquire) ||
            !RecoveryPending.load(std::memory_order_acquire))
            return;
        RecoveryPoseWarmup.store(true, std::memory_order_release);
    }

    inline bool PoseWarmupAllowed() noexcept
    {
        return SafetyOverlayReady.load(std::memory_order_acquire) &&
            HostFresh.load(std::memory_order_acquire) &&
            HostRenderable.load(std::memory_order_acquire) &&
            !ExternalSafetyBlock.load(std::memory_order_acquire) &&
            RecoveryPending.load(std::memory_order_acquire) &&
            RecoveryPoseWarmup.load(std::memory_order_acquire) &&
            !StereoAllowed.load(std::memory_order_acquire);
    }

    inline void BaselineVerified() noexcept
    {
        if (!SafetyOverlayReady.load(std::memory_order_acquire) ||
            !HostFresh.load(std::memory_order_acquire) ||
            !HostRenderable.load(std::memory_order_acquire) ||
            ExternalSafetyBlock.load(std::memory_order_acquire))
            return;
        RecoveryPoseWarmup.store(false, std::memory_order_release);
        RecoveryPending.store(false, std::memory_order_release);
        StereoAllowed.store(true, std::memory_order_release);
    }

    inline bool MayInjectStereo() noexcept
    {
        return SafetyOverlayReady.load(std::memory_order_acquire) &&
            HostFresh.load(std::memory_order_acquire) &&
            HostRenderable.load(std::memory_order_acquire) &&
            !ExternalSafetyBlock.load(std::memory_order_acquire) &&
            StereoAllowed.load(std::memory_order_acquire) &&
            !RecoveryPending.load(std::memory_order_acquire);
    }
}
