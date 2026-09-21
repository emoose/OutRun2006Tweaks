#pragma once

#include <algorithm>
#include <cctype>
#include <string>

#include "hook_mgr.hpp"
#include "plugin.hpp"

// Legacy builds shipped an FFB device-name default of "MOZA" and later migrated
// that exact default to "R3 Racing Wheel". That was useful while R3 was the only
// tested wheel, but it can prevent every other DirectInput FFB wheel from being
// selected on a clean install.
//
// Device-specific menu compatibility remains separate. For FFB output, an
// unpinned legacy default is now relaxed to an empty name filter so the engine
// chooses the first non-virtual FORCEFEEDBACK device. Once F11 saves an exact
// DeviceGuid, explicit routing is preserved and never redirected here.
namespace Settings
{
    extern Setting<std::string> WheelMenuR3DeviceName;
    extern Setting<std::string> WheelFFBDeviceName;
    extern Setting<std::string> WheelFFBDeviceGuid;
}

namespace
{
    static std::string r3_autoselect_lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    class WheelR3DeviceAutoSelect : public Hook
    {
    public:
        std::string_view description() override
        {
            return "WheelR3DeviceAutoSelect";
        }

        bool validate() override
        {
            // FFB output selection is independent of the gameplay input backend.
            // The old UseNewInput/WheelInputCompatibility gate could leave a
            // model-specific R3 default active for otherwise supported wheels.
            return true;
        }

        bool apply() override
        {
            bool changed = false;

            // Keep the menu helper's legacy R3 matching behavior. This affects
            // only front-end input compatibility, never FFB force generation.
            if (r3_autoselect_lower(Settings::WheelMenuR3DeviceName.get()) == "moza")
            {
                // Strict-first/fallback selection remains in the menu reader.
            }

            const std::string ffbName =
                r3_autoselect_lower(Settings::WheelFFBDeviceName.get());
            if (Settings::WheelFFBDeviceGuid.get().empty() &&
                (ffbName == "moza" || ffbName == "r3 racing wheel"))
            {
                Settings::WheelFFBDeviceName = "";
                changed = true;
            }

            if (changed)
            {
                spdlog::info(
                    "WheelFFB device auto-select: cleared legacy MOZA/R3-only default; using first non-virtual FFB device until an exact GUID is saved");
            }

            return true;
        }

        static WheelR3DeviceAutoSelect instance;
    };

    WheelR3DeviceAutoSelect WheelR3DeviceAutoSelect::instance;
}
