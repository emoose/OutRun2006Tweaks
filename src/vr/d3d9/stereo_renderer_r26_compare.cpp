// Safe-draw comparison build.
//
// This translation unit intentionally stops the stereo draw chain at R26/R28.
// It keeps the current R23/R22/R13/R9 recovery and correctness layers, while
// excluding the later R29-R34 fast/HUD/StateBlock dispatch overlays. The current
// D3D9Ex R15, DirectGPU transport, texture lifetime fixes, wheel, multi-device
// and FFB code remain unchanged. The game renderer owner is deliberately rolled
// back from R29 to R23 so this build matches the proven R26/R23/R22 boundary.
//
// Build only with OUTRUN_VR_SAFE_DRAW_COMPARE=ON. In normal builds this file is
// marked HEADER_FILE_ONLY by cmake.toml.

#include "stereo_renderer_r26.cpp"

namespace OutRunVRStereo
{
    namespace
    {
        class VRSafeDrawCompareMarkerHook final : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRSafeDrawCompare";
            }

            bool validate() override { return true; }

            bool apply() override
            {
                spdlog::warn(
                    "VR SAFE-DRAW COMPARE: R26/R23/R22/R13/R9 stereo draw chain ACTIVE; R29-R34 stereo overlays + renderer R29 EXCLUDED; current R15/DirectGPU/texture paths preserved");
                return true;
            }

            static VRSafeDrawCompareMarkerHook instance;
        };

        VRSafeDrawCompareMarkerHook VRSafeDrawCompareMarkerHook::instance;
    }
}
