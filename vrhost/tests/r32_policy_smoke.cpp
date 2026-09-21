#include "vr/d3d9/r32_policy.hpp"

#include <cassert>
#include <cstdint>
#include <limits>

int main()
{
    using namespace OutRunVR::R32;

    // Reset after a long run must not retain an old absolute mono-safety epoch.
    assert(RearmMonoSafetyEpoch(1) == 3);
    assert(RearmMonoSafetyEpoch(42, 2) == 44);
    assert(RearmMonoSafetyEpoch(0, 2) == 3);

    const auto maxValue = (std::numeric_limits<std::uint64_t>::max)();
    assert(RearmMonoSafetyEpoch(maxValue - 1, 4) == maxValue);

    assert(EffectSnapshotResult(true) ==
        EffectSnapshotDecision::UseCapturedPolicy);
    assert(EffectSnapshotResult(false) ==
        EffectSnapshotDecision::ForceZeroDisparity);

    static_assert(ProducerFenceBudgetMs <= 2,
        "R32 producer fence must stay below the old 12 ms synchronous budget");

    // A timed-out D3D9 producer copy cannot make its ring slot reusable until
    // the old EVENT query has actually completed.
    assert(ClassifyPendingFence(false, false, false, false) ==
        PendingFenceDecision::ReuseSlot);
    assert(ClassifyPendingFence(true, true, true, false) ==
        PendingFenceDecision::ReuseSlot);
    assert(ClassifyPendingFence(true, true, false, true) ==
        PendingFenceDecision::BlockReuse);
    assert(ClassifyPendingFence(true, false, false, false) ==
        PendingFenceDecision::QueryError);
    assert(ClassifyPendingFence(true, true, false, false) ==
        PendingFenceDecision::QueryError);

    return 0;
}
