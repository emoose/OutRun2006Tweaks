// R21 game-side host-death fail-closed overlay.
//
// R20/R13/R9 remain the renderer implementation. R21 owns the Present-boundary
// host freshness decision. R23 review hardening aligns that decision with the
// 250 ms pose-reader budget and publishes it through RuntimeEligibility so WVP
// injection, stereo replay and bootstrap share one authority.

#include "stereo_renderer_r20.cpp"

namespace OutRunVRStereo
{
    namespace
    {
        constexpr std::int64_t R21HostStaleMs =
            OutRunVR::RuntimeEligibility::HostStaleMs;

        SafetyHookInline R21PresentR9Hook{};
        std::atomic<OutRunVR::RuntimeEligibility::InstallState> R21InstallState{
            OutRunVR::RuntimeEligibility::InstallState::Pending };
        bool R21HostFailClosed = true;
        bool R21HostSoftSuspended = false;
        std::uint32_t R21LastHealthyHostPid = 0;
        ULONGLONG R21LastHealthyTickMs = 0;
        ULONGLONG R21LastTransientGraceLogMs = 0;
        std::uint64_t R21TransientInvalidReadGrace = 0;

        enum class R21HostStatus : std::uint8_t
        {
            Stale,
            SoftSuspend,
            Fresh
        };

        bool R21ComputeAgeMs(std::int64_t sampleQpc,
            std::int64_t& ageMs) noexcept
        {
            ageMs = INT64_MAX;
            if (sampleQpc <= 0)
                return false;
            LARGE_INTEGER now{};
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceCounter(&now) ||
                !QueryPerformanceFrequency(&frequency) ||
                frequency.QuadPart <= 0 || now.QuadPart < sampleQpc)
                return false;
            const std::int64_t delta = now.QuadPart - sampleQpc;
            ageMs = (delta * 1000) / frequency.QuadPart;
            return true;
        }

        R21HostStatus R21ReadHostFreshness(std::uint32_t& hostPid,
            std::uint32_t& flags, std::uint32_t& heartbeat,
            std::int64_t& sampleQpc, std::int64_t& ageMs) noexcept
        {
            hostPid = flags = heartbeat = 0;
            sampleQpc = 0;
            ageMs = INT64_MAX;

            if (!SharedState || SharedState->magic != OutRunVR::SharedMagic ||
                SharedState->protocolVersion != OutRunVR::SharedProtocolVersion ||
                SharedState->structSize != sizeof(OutRunVR::SharedPoseState))
                return R21HostStatus::Stale;

            for (int attempt = 0; attempt < 4; ++attempt)
            {
                const std::uint32_t before = SharedState->sequence;
                if (before & 1u)
                    continue;
                MemoryBarrier();
                const std::uint32_t pid = SharedState->hostPid;
                const std::uint32_t currentFlags = SharedState->flags;
                const std::uint32_t currentHeartbeat = SharedState->heartbeat;
                const std::int64_t currentSampleQpc = SharedState->sampleQpc;
                MemoryBarrier();
                const std::uint32_t after = SharedState->sequence;
                if (before != after || (after & 1u))
                    continue;

                hostPid = pid;
                flags = currentFlags;
                heartbeat = currentHeartbeat;
                sampleQpc = currentSampleQpc;

                constexpr std::uint32_t hardRequired =
                    OutRunVR::HostAlive | OutRunVR::SessionVisible;
                if (!hostPid || (flags & hardRequired) != hardRequired ||
                    !R21ComputeAgeMs(sampleQpc, ageMs) ||
                    ageMs > R21HostStaleMs)
                    return R21HostStatus::Stale;

                if ((flags & OutRunVR::HostShouldRender) == 0)
                    return R21HostStatus::SoftSuspend;
                return R21HostStatus::Fresh;
            }
            return R21HostStatus::Stale;
        }

        void R21ApplyHostFailClosedAtPresent() noexcept
        {
            std::uint32_t hostPid = 0;
            std::uint32_t flags = 0;
            std::uint32_t heartbeat = 0;
            std::int64_t sampleQpc = 0;
            std::int64_t ageMs = INT64_MAX;
            const R21HostStatus status = R21ReadHostFreshness(
                hostPid, flags, heartbeat, sampleQpc, ageMs);

            if (status == R21HostStatus::Fresh)
            {
                R21LastHealthyHostPid = hostPid;
                R21LastHealthyTickMs = GetTickCount64();
                OutRunVR::RuntimeEligibility::ObserveFreshHost();

                if (R21HostFailClosed)
                {
                    R9StereoSeeded = false;
                    R9MonoSeeded = false;
                    R9MonoBackupGap = false;
                    R20StereoEligibilityGate.store(false, std::memory_order_release);
                    R21HostFailClosed = false;
                    R21HostSoftSuspended = false;
                    spdlog::info(
                        "VR R21/R23: fresh host recovered pid={} heartbeat={}; waiting for a new verified stereo baseline before WVP/stereo resume",
                        hostPid, heartbeat);
                }
                else
                {
                    const bool resumedSoft = R21HostSoftSuspended;
                    R21HostSoftSuspended = false;
                    R20StereoEligibilityGate.store(
                        OutRunVR::RuntimeEligibility::MayInjectStereo(),
                        std::memory_order_release);
                    if (resumedSoft)
                    {
                        spdlog::info(
                            "VR R21 SOFT-RESUME: shouldRender returned; game-side stereo source stayed continuous");
                    }
                }
                return;
            }

            if (status == R21HostStatus::SoftSuspend)
            {
                R21LastHealthyHostPid = hostPid;
                R21LastHealthyTickMs = GetTickCount64();
                OutRunVR::RuntimeEligibility::ObserveSoftHostSuspend();
                R20StereoEligibilityGate.store(
                    OutRunVR::RuntimeEligibility::MayInjectStereo(),
                    std::memory_order_release);
                if (!R21HostSoftSuspended)
                {
                    R21HostSoftSuspended = true;
                    spdlog::info(
                        "VR R21 SOFT-SUSPEND: shouldRender=0 is host-only scheduling advice; verified game-side stereo source remains active without baseline reset");
                }
                return;
            }

            // A single invalid/zero shared-state sample was observed in the
            // supplied run while the host heartbeat resumed on the very next
            // Present. Honor the same 250 ms freshness budget before destroying
            // the verified baseline. This still fail-closes promptly on a real
            // host death, but avoids a full recovery cycle for a transient
            // seqlock/mapping handoff.
            const ULONGLONG nowMs = GetTickCount64();
            if (!R21HostFailClosed && R21LastHealthyTickMs != 0 &&
                nowMs >= R21LastHealthyTickMs &&
                nowMs - R21LastHealthyTickMs <=
                    static_cast<ULONGLONG>(R21HostStaleMs))
            {
                ++R21TransientInvalidReadGrace;
                if (R21LastTransientGraceLogMs == 0 ||
                    nowMs - R21LastTransientGraceLogMs >= 5000)
                {
                    R21LastTransientGraceLogMs = nowMs;
                    spdlog::info(
                        "VR R21 TRANSIENT-GRACE: one invalid host sample kept the verified stereo baseline alive within {}ms freshness budget (count={})",
                        R21HostStaleMs, R21TransientInvalidReadGrace);
                }
                return;
            }

            R21HostSoftSuspended = false;
            OutRunVR::RuntimeEligibility::FailClosed();
            R20StereoEligibilityGate.store(false, std::memory_order_release);

            if (R9MonoSeeded && !R9MonoBackupGap)
                R9StereoSeeded = false;
            else
            {
                R9StereoSeeded = false;
                R9MonoSeeded = false;
            }

            if (!R21HostFailClosed)
            {
                R21HostFailClosed = true;
                spdlog::warn(
                    "VR R21 FAIL-CLOSED: OpenXR host stale/unavailable (lastPid={} currentPid={} heartbeat={} flags=0x{:08x} ageMs={} threshold={}ms); shared WVP/stereo/bootstrap gate closed",
                    R21LastHealthyHostPid, hostPid, heartbeat,
                    static_cast<unsigned>(flags),
                    ageMs == INT64_MAX ? -1 : ageMs,
                    R21HostStaleMs);
            }
        }

        HRESULT __stdcall PresentDestR21(IDirect3DDevice9* device,
            const RECT* sourceRect, const RECT* destRect,
            HWND destWindowOverride, const RGNDATA* dirtyRegion)
        {
            if (IsGameDevice(device))
                R21ApplyHostFailClosedAtPresent();
            return R21PresentR9Hook.stdcall<HRESULT>(device, sourceRect,
                destRect, destWindowOverride, dirtyRegion);
        }

        DWORD WINAPI R21GameInstallThread(void*)
        {
            using State = OutRunVR::RuntimeEligibility::InstallState;
            R21InstallState.store(State::Pending, std::memory_order_release);
            OutRunVR::RuntimeEligibility::FailClosed();
            R20StereoEligibilityGate.store(false, std::memory_order_release);

            for (int attempt = 0; attempt < 4800; ++attempt)
            {
                const std::uint32_t r9 = R9InstallState.load(std::memory_order_acquire);
                const std::uint32_t r13 = R13InstallState.load(std::memory_order_acquire);

                if (r9 == R9InstallFailed || r13 == R13InstallFailed ||
                    OutRunVR::RuntimeEligibility::IsFailed(R20InstallState))
                {
                    R21InstallState.store(State::Failed, std::memory_order_release);
                    spdlog::error(
                        "VR R21: prerequisite R9/R13/R20 transaction failed; host-death Present guard not installed");
                    return 0;
                }

                if (r9 == R9InstallReady && r13 == R13InstallReady &&
                    OutRunVR::RuntimeEligibility::IsReady(R20InstallState))
                {
                    R21PresentR9Hook = safetyhook::create_inline(
                        reinterpret_cast<void*>(&PresentDestR9), PresentDestR21,
                        safetyhook::InlineHook::StartDisabled);
                    const bool enabled = R21PresentR9Hook &&
                        R21PresentR9Hook.enable().has_value();
                    if (!enabled)
                    {
                        R21PresentR9Hook = {};
                        R21InstallState.store(State::Failed, std::memory_order_release);
                        spdlog::error(
                            "VR R21: failed to enable R9 Present host-death guard transaction");
                        return 0;
                    }
                    R21InstallState.store(State::Ready, std::memory_order_release);
                    spdlog::info(
                        "VR R21/R23 GAME: common host freshness gate ACTIVE threshold={}ms; disabled-first transaction READY; recovery requires a new verified baseline",
                        R21HostStaleMs);
                    return 0;
                }
                Sleep(25);
            }

            R21InstallState.store(State::Failed, std::memory_order_release);
            spdlog::warn(
                "VR R21: timed out waiting for prerequisite renderer transactions; host-death guard FAILED");
            return 0;
        }

        class VRHostDeathFailClosedR21Hook : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRHostDeathFailClosedR21";
            }

            bool validate() override { return true; }

            bool apply() override
            {
                using State = OutRunVR::RuntimeEligibility::InstallState;
                R21InstallState.store(State::Pending, std::memory_order_release);
                HANDLE thread = CreateThread(
                    nullptr, 0, R21GameInstallThread, nullptr, 0, nullptr);
                if (!thread)
                {
                    R21InstallState.store(State::Failed, std::memory_order_release);
                    spdlog::error(
                        "VR R21: failed to create host-death guard installer thread: {}",
                        GetLastError());
                    return false;
                }
                CloseHandle(thread);
                return true;
            }

            static VRHostDeathFailClosedR21Hook instance;
        };

        VRHostDeathFailClosedR21Hook VRHostDeathFailClosedR21Hook::instance;
    }
}
