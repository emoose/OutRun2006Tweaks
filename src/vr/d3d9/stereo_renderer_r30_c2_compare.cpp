// C2 regression-isolation build.
//
// C2 is C1 plus the R30 asymmetric-FOV HUD correction. R31-R34 are intentionally
// absent so this test answers only whether R30 fixes the remaining HUD after the
// R27/R28 perspective-world classifier is restored under R29's fast LEFT+RIGHT
// path. R31 normally exports the StateBlock helpers; report conservative state
// here so any dependent code uses live/fail-closed validation.

#include "stereo_renderer_r30.cpp"

namespace OutRunVRStereo
{
    bool IsGameStateBlockRecording() noexcept
    {
        return false;
    }

    bool IsStateBlockTrackingReliable() noexcept
    {
        return false;
    }

    namespace
    {
        class VRC2CompareMarkerHook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRC2Compare";
            }

            bool validate() override { return true; }

            bool apply() override
            {
                spdlog::warn(
                    "VR C2 COMPARE: C1 R29+R27/R28 classification + R30 HUD correction ACTIVE; R31-R34 excluded");
                return true;
            }

            static VRC2CompareMarkerHook instance;
        };

        VRC2CompareMarkerHook VRC2CompareMarkerHook::instance;
    }
}
