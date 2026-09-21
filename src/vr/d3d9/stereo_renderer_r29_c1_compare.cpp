// C1 regression-isolation build.
//
// Keep R29's steady-state LEFT+RIGHT fast path, but restore the R23/R27/R28
// perspective-world classifier through OUTRUN_VR_R29_R28_CLASSIFICATION_COMPARE.
// R30-R34 are intentionally absent. R31 normally exports the StateBlock helper
// functions below; this isolated build has no R31 owner, so both report
// conservative/unreliable and R29/R28 fall back to live validation.

#include "stereo_renderer_r29.cpp"

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
        class VRC1CompareMarkerHook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRC1Compare";
            }

            bool validate() override { return true; }

            bool apply() override
            {
                spdlog::warn(
                    "VR C1 COMPARE: R29 fast L+R + R23/R27/R28 perspective classification ACTIVE; R30-R34 excluded");
                return true;
            }

            static VRC1CompareMarkerHook instance;
        };

        VRC1CompareMarkerHook VRC1CompareMarkerHook::instance;
    }
}
