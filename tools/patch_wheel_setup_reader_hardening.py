from pathlib import Path

path = Path("src/overlay/wheel_setup_ui.cpp")
text = path.read_text(encoding="utf-8")


def replace_once(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    text = text.replace(old, new, 1)
    print(f"patched: {label}")


# Never silently bind a saved wheel profile to a different device merely because
# the original wheel is absent.  Empty selection may still intentionally choose
# the first attached device for first-run convenience.
replace_once(
'''            int index = -1;\n            for (size_t i = 0; i < devices_.size(); ++i)\n            {\n                if (devices_[i].name == wanted)\n                {\n                    index = int(i);\n                    break;\n                }\n            }\n            if (index < 0 && !devices_.empty())\n                index = 0;\n            if (index < 0)\n                return false;\n            return open(index);\n''',
'''            int index = -1;\n            for (size_t i = 0; i < devices_.size(); ++i)\n            {\n                if (devices_[i].name == wanted)\n                {\n                    index = int(i);\n                    break;\n                }\n            }\n            if (index < 0 && wanted.empty() && !devices_.empty())\n                index = 0;\n            if (index < 0)\n                return false;\n            return open(index);\n''',
    "strict saved universal-wheel selection",
)

# A disconnected/replugged DirectInput object can stay permanently stale.  On
# final poll/state failure throw away both the handle and cached enumeration so
# the next frame performs a fresh EnumDevices instead of retrying a dead GUID.
replace_once(
'''            HRESULT hr = device_->Poll();\n            if (FAILED(hr))\n                hr = device_->Acquire();\n            if (FAILED(hr))\n                return false;\n\n            hr = device_->GetDeviceState(sizeof(state_), &state_);\n            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n            {\n                if (FAILED(device_->Acquire()))\n                    return false;\n                hr = device_->GetDeviceState(sizeof(state_), &state_);\n            }\n            return SUCCEEDED(hr);\n''',
'''            HRESULT hr = device_->Poll();\n            if (FAILED(hr))\n            {\n                hr = device_->Acquire();\n                if (SUCCEEDED(hr))\n                    hr = device_->Poll();\n            }\n            if (FAILED(hr))\n            {\n                release_device();\n                devices_.clear();\n                return false;\n            }\n\n            hr = device_->GetDeviceState(sizeof(state_), &state_);\n            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)\n            {\n                if (SUCCEEDED(device_->Acquire()))\n                {\n                    device_->Poll();\n                    hr = device_->GetDeviceState(sizeof(state_), &state_);\n                }\n            }\n            if (FAILED(hr))\n            {\n                release_device();\n                devices_.clear();\n                return false;\n            }\n            return true;\n''',
    "universal reader stale-handle recovery",
)

# Virtual devices can advertise a harmless-looking product name while their
# instance name still says vJoy/ViGEm/etc.  Reject either form.
replace_once(
'''            const std::string product = instance->tszProductName;\n            if (is_virtual_name(product))\n                return DIENUM_CONTINUE;\n\n            DeviceInfo info{};\n''',
'''            const std::string product = instance->tszProductName;\n            const std::string instanceName = instance->tszInstanceName;\n            if (is_virtual_name(product) || is_virtual_name(instanceName))\n                return DIENUM_CONTINUE;\n\n            DeviceInfo info{};\n''',
    "universal reader virtual-instance filter",
)

# Do not retain a half-open device if Acquire itself fails.
replace_once(
'''            device->Acquire();\n            device_ = device;\n            selectedName_ = devices_[index].name;\n            return true;\n''',
'''            hr = device->Acquire();\n            if (FAILED(hr) && hr != S_FALSE)\n            {\n                device->Release();\n                return false;\n            }\n            device_ = device;\n            selectedName_ = devices_[index].name;\n            return true;\n''',
    "universal reader Acquire validation",
)

# The UI's filtered enumeration index is not guaranteed to equal OutRun's
# internal legacy-device slot (virtual/non-wheel devices may be omitted here).
# Expose the original-game slot explicitly and never overwrite it merely by
# choosing a reader/FFB device from the combo.
replace_once(
'''        static bool copy_current_mapping()\n        {\n''',
'''        static int regular_device_count()\n        {\n            return std::max(device_count() - 1, 0);\n        }\n\n        static bool copy_current_mapping()\n        {\n''',
    "expose regular legacy slot count",
)

replace_once(
'''        void select_device(const DeviceInfo& info, int index)\n        {\n            Settings::WheelUniversalDeviceName = info.name;\n            Settings::WheelUniversalLegacyDeviceIndex = std::clamp(index, 0, 2);\n            Settings::WheelFFBDeviceName = info.name;\n            gReader.select_by_name(info.name);\n            status_ = "Selected wheel; FFB device will follow this product name.";\n        }\n''',
'''        void select_device(const DeviceInfo& info)\n        {\n            Settings::WheelUniversalDeviceName = info.name;\n            Settings::WheelFFBDeviceName = info.name;\n            gReader.select_by_name(info.name);\n            status_ = UniversalWheelProfile::regular_device_count() > 1\n                ? "Selected wheel/FFB device. Confirm the OutRun legacy input slot below when multiple controllers are present."\n                : "Selected wheel; FFB device follows this product name.";\n        }\n''',
    "decouple UI device index from game slot",
)
replace_once(
'''                    if (ImGui::Selectable(devices[i].name.c_str(), selected))\n                        select_device(devices[i], int(i));\n''',
'''                    if (ImGui::Selectable(devices[i].name.c_str(), selected))\n                        select_device(devices[i]);\n''',
    "wheel combo select signature",
)

# Place the explicit slot selector beside the selected-device capabilities.  A
# single regular controller is unambiguous and is kept on slot 1 automatically.
needle = '''            if (!devices.empty())\n            {\n                for (const auto& dev : devices)\n                    if (dev.name == Settings::WheelUniversalDeviceName.get())\n                        ImGui::TextDisabled("%lu axes / %lu buttons / %lu POV / FFB %s",\n                            dev.axes, dev.buttons, dev.povs, dev.ffb ? "yes" : "no");\n            }\n\n            if (ImGui::Checkbox("Enable F11 universal wheel profile", Settings::WheelUniversalSetupEnable.ptr()))\n'''
replacement = '''            if (!devices.empty())\n            {\n                for (const auto& dev : devices)\n                    if (dev.name == Settings::WheelUniversalDeviceName.get())\n                        ImGui::TextDisabled("%lu axes / %lu buttons / %lu POV / FFB %s",\n                            dev.axes, dev.buttons, dev.povs, dev.ffb ? "yes" : "no");\n            }\n\n            const int regularSlots = UniversalWheelProfile::regular_device_count();\n            if (regularSlots == 1)\n                Settings::WheelUniversalLegacyDeviceIndex = 0;\n            if (regularSlots > 0)\n            {\n                int currentSlot = std::clamp(\n                    int(Settings::WheelUniversalLegacyDeviceIndex), 0,\n                    std::min(regularSlots - 1, 2));\n                const std::string slotPreview =\n                    "OutRun slot " + std::to_string(currentSlot + 1);\n                if (ImGui::BeginCombo("Legacy input slot", slotPreview.c_str()))\n                {\n                    for (int slot = 0; slot < std::min(regularSlots, 3); ++slot)\n                    {\n                        const std::string label =\n                            "OutRun slot " + std::to_string(slot + 1);\n                        const bool selected = slot == currentSlot;\n                        if (ImGui::Selectable(label.c_str(), selected))\n                            Settings::WheelUniversalLegacyDeviceIndex = slot;\n                        if (selected)\n                            ImGui::SetItemDefaultFocus();\n                    }\n                    ImGui::EndCombo();\n                }\n                if (ImGui::IsItemHovered())\n                    ImGui::SetTooltip(\n                        "This is OutRun's original DirectInput slot, not the filtered Wheel combo index. Usually slot 1 when only one wheel is connected.");\n            }\n\n            if (ImGui::Checkbox("Enable F11 universal wheel profile", Settings::WheelUniversalSetupEnable.ptr()))\n'''
if text.count(needle) != 1:
    raise SystemExit("legacy slot UI insertion point not found exactly once")
text = text.replace(needle, replacement, 1)
print("patched: explicit legacy input slot UI")

path.write_text(text, encoding="utf-8")
print("Applied universal wheel reader hardening and explicit legacy-slot selection")
