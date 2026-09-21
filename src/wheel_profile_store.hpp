#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "plugin.hpp"

namespace Settings
{
    extern Setting<std::string> WheelFFBDeviceName;
    extern Setting<std::string> WheelFFBDeviceGuid;
    extern Setting<bool> WheelFFBResponseCorrection;
    extern Setting<std::string> WheelFFBResponseLUT;
    extern Setting<float> WheelFFBMaxTorqueNm;
}

// Named wheel/input and force-feedback profiles live beside the DLL instead of
// inside OutRun2006Tweaks.user.ini.  The active configuration is still copied
// to the normal INI files when a profile is loaded, so selecting a profile is
// durable across restarts without making startup depend on a profile file.
namespace WheelProfileStore
{
    enum class Kind
    {
        Input,
        ForceFeedback,
    };

    inline std::string lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    inline std::string normalize_profile_name(std::string value)
    {
        value = Util::trim(value);
        if (value.size() >= 4 && lower_ascii(value.substr(value.size() - 4)) == ".ini")
            value.resize(value.size() - 4);
        return Util::trim(value);
    }

    inline bool valid_profile_name(std::string_view rawName, std::string* reason = nullptr)
    {
        const std::string name = normalize_profile_name(std::string(rawName));
        auto fail = [&](const char* why)
        {
            if (reason) *reason = why;
            return false;
        };

        if (name.empty())
            return fail("Enter a profile name.");
        if (name.size() > 64)
            return fail("Profile names are limited to 64 characters.");
        if (name == "." || name == "..")
            return fail("That profile name is reserved.");

        // Keep file names portable and block path traversal. ASCII is deliberate:
        // std::filesystem narrow-string conversion is code-page dependent on
        // Windows, so accepting arbitrary UTF-8 here would produce profiles that
        // work on some systems but not others.
        for (const unsigned char c : name)
        {
            if (!(std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.'))
                return fail("Use letters, numbers, spaces, '.', '_' or '-' in profile names.");
        }
        if (name.back() == '.' || name.back() == ' ')
            return fail("Profile names cannot end with a dot or space.");

        const std::string lower = lower_ascii(name.substr(0, name.find('.')));
        static constexpr const char* reserved[] = {
            "con", "prn", "aux", "nul",
            "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
            "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
        };
        for (const char* value : reserved)
            if (lower == value)
                return fail("That profile name is reserved by Windows.");
        return true;
    }

    inline std::filesystem::path directory(Kind kind)
    {
        return Module::DllPath.parent_path() / "OutRun2006Tweaks.profiles" /
            (kind == Kind::Input ? "Input" : "FFB");
    }

    inline bool ensure_directory(Kind kind, std::string* error = nullptr)
    {
        std::error_code ec;
        std::filesystem::create_directories(directory(kind), ec);
        if (ec)
        {
            if (error) *error = "Could not create profile directory: " + ec.message();
            return false;
        }
        return true;
    }

    inline std::optional<std::filesystem::path> profile_path(
        Kind kind, std::string_view rawName, std::string* error = nullptr)
    {
        std::string reason;
        if (!valid_profile_name(rawName, &reason))
        {
            if (error) *error = reason;
            return std::nullopt;
        }
        if (!ensure_directory(kind, error))
            return std::nullopt;
        const std::string name = normalize_profile_name(std::string(rawName));
        return directory(kind) / (name + ".ini");
    }

    inline std::vector<std::string> list_profiles(Kind kind)
    {
        std::vector<std::string> result;
        std::string ignored;
        if (!ensure_directory(kind, &ignored))
            return result;

        std::error_code ec;
        for (std::filesystem::directory_iterator it(directory(kind), ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec) || ec)
                continue;
            if (lower_ascii(it->path().extension().string()) != ".ini")
                continue;
            result.push_back(it->path().stem().string());
        }
        std::sort(result.begin(), result.end(), [](const std::string& a, const std::string& b)
        {
            return lower_ascii(a) < lower_ascii(b);
        });
        return result;
    }

    inline bool profile_exists(Kind kind, std::string_view rawName)
    {
        std::string ignored;
        auto path = profile_path(kind, rawName, &ignored);
        if (!path)
            return false;
        std::error_code ec;
        return std::filesystem::is_regular_file(*path, ec) && !ec;
    }

    inline std::filesystem::path staged_profile_path(const std::filesystem::path& finalPath)
    {
        auto staged = finalPath;
        staged += ".tmp";
        return staged;
    }

    inline bool commit_staged_profile(
        const std::filesystem::path& staged,
        const std::filesystem::path& finalPath,
        std::string* error = nullptr)
    {
        auto backup = finalPath;
        backup += ".bak";
        std::error_code ec;

        // MSVC's std::filesystem::is_regular_file(path, ec) can report
        // ERROR_FILE_NOT_FOUND through ec for a path that simply does not exist.
        // A missing destination/backup is the normal first-save case, not an error.
        const auto inspect_regular_file = [&](const std::filesystem::path& path,
                                              bool& exists,
                                              const char* label) -> bool
        {
            ec.clear();
            exists = std::filesystem::is_regular_file(path, ec);
            if (ec == std::errc::no_such_file_or_directory)
            {
                ec.clear();
                exists = false;
                return true;
            }
            if (ec)
            {
                if (error) *error = std::string("Could not inspect ") + label + ": " + ec.message();
                return false;
            }
            return true;
        };

        bool finalExists = false;
        if (!inspect_regular_file(finalPath, finalExists, "the existing profile"))
            return false;

        bool backupExists = false;
        if (!inspect_regular_file(backup, backupExists, "the profile backup"))
            return false;

        // Recover a previous interrupted replacement before starting another.
        if (!finalExists && backupExists)
        {
            std::filesystem::rename(backup, finalPath, ec);
            if (ec)
            {
                if (error) *error = "Could not recover the previous profile backup: " + ec.message();
                return false;
            }
            finalExists = true;
        }

        if (finalExists)
        {
            std::filesystem::remove(backup, ec);
            if (ec)
            {
                if (error) *error = "Could not clear a stale profile backup: " + ec.message();
                return false;
            }
            std::filesystem::rename(finalPath, backup, ec);
            if (ec)
            {
                if (error) *error = "Could not preserve the existing profile before overwrite: " + ec.message();
                return false;
            }
        }

        bool installedByCopyFallback = false;
        std::filesystem::rename(staged, finalPath, ec);
        if (ec)
        {
            const std::string renameError = ec.message();
            std::error_code copyEc;
            std::filesystem::copy_file(
                staged, finalPath, std::filesystem::copy_options::overwrite_existing, copyEc);
            if (copyEc)
            {
                if (finalExists)
                {
                    std::error_code restoreEc;
                    std::filesystem::rename(backup, finalPath, restoreEc);
                    if (restoreEc)
                    {
                        if (error) *error = "Could not install the new profile (rename: " + renameError +
                            "; copy fallback: " + copyEc.message() + ") and could not restore the backup (" +
                            restoreEc.message() + "). Destination: " + finalPath.string();
                        return false;
                    }
                }
                if (error) *error = "Could not install the completed profile (rename: " + renameError +
                    "; copy fallback: " + copyEc.message() + "). Destination: " + finalPath.string();
                return false;
            }
            installedByCopyFallback = true;
        }

        std::error_code verifyEc;
        const bool installed = std::filesystem::is_regular_file(finalPath, verifyEc) && !verifyEc;
        if (!installed)
        {
            std::error_code ignored;
            std::filesystem::remove(finalPath, ignored);
            if (finalExists)
            {
                std::error_code restoreEc;
                std::filesystem::rename(backup, finalPath, restoreEc);
                if (restoreEc)
                {
                    if (error) *error = "Profile installation could not be verified and the previous profile could not be restored: " +
                        restoreEc.message() + ". Destination: " + finalPath.string();
                    return false;
                }
            }
            if (error) *error = "Profile installation could not be verified. Destination: " + finalPath.string();
            return false;
        }

        if (installedByCopyFallback)
        {
            std::error_code ignored;
            std::filesystem::remove(staged, ignored);
        }
        if (finalExists)
        {
            std::error_code ignored;
            std::filesystem::remove(backup, ignored);
        }
        return true;
    }

    inline bool delete_profile(Kind kind, std::string_view rawName, std::string* error = nullptr)
    {
        auto path = profile_path(kind, rawName, error);
        if (!path)
            return false;
        std::error_code ec;
        const bool removed = std::filesystem::remove(*path, ec);
        if (ec || !removed)
        {
            if (error) *error = ec ? ("Could not delete profile: " + ec.message()) : "Profile file was not found.";
            return false;
        }
        return true;
    }

    inline bool parse_section(
        const std::filesystem::path& path,
        std::string_view wantedSection,
        std::unordered_map<std::string, std::string>& values,
        std::string* error = nullptr)
    {
        std::ifstream file(path);
        if (!file)
        {
            if (error) *error = "Could not open profile file.";
            return false;
        }

        const std::string wanted = lower_ascii(std::string(wantedSection));
        std::string section;
        std::string line;
        while (std::getline(file, line))
        {
            line = Util::trim(line);
            if (line.empty() || line.front() == '#' || line.front() == ';')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = lower_ascii(Util::trim(line.substr(1, line.size() - 2)));
                continue;
            }
            if (section != wanted)
                continue;
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = lower_ascii(Util::trim(line.substr(0, equals)));
            const std::string value = Util::trim(line.substr(equals + 1));
            values[key] = value;
        }
        return true;
    }

    struct InputOptionsSnapshot
    {
        std::string inputBackend;
        std::string steeringDeadZone;
        std::string bypassSensitivity;
        std::string ffbDeviceName;
        std::string ffbDeviceGuid;
        std::string ffbResponseCorrection;
        std::string ffbResponseLut;
        std::string ffbMaxTorqueNm;
    };

    inline InputOptionsSnapshot capture_input_options()
    {
        return {
            Settings::InputBackend.to_string(),
            Settings::SteeringDeadZone.to_string(),
            Settings::BypassGameSensitivity.to_string(),
            Settings::WheelFFBDeviceName.to_string(),
            Settings::WheelFFBDeviceGuid.to_string(),
            Settings::WheelFFBResponseCorrection.to_string(),
            Settings::WheelFFBResponseLUT.to_string(),
            Settings::WheelFFBMaxTorqueNm.to_string(),
        };
    }

    inline void restore_input_options(const InputOptionsSnapshot& snapshot)
    {
        Settings::InputBackend.set_from_string(snapshot.inputBackend);
        Settings::SteeringDeadZone.set_from_string(snapshot.steeringDeadZone);
        Settings::BypassGameSensitivity.set_from_string(snapshot.bypassSensitivity);
        Settings::WheelFFBDeviceName.set_from_string(snapshot.ffbDeviceName);
        Settings::WheelFFBDeviceGuid.set_from_string(snapshot.ffbDeviceGuid);
        Settings::WheelFFBResponseCorrection.set_from_string(snapshot.ffbResponseCorrection);
        Settings::WheelFFBResponseLUT.set_from_string(snapshot.ffbResponseLut);
        Settings::WheelFFBMaxTorqueNm.set_from_string(snapshot.ffbMaxTorqueNm);
    }

    inline bool append_input_options(const std::filesystem::path& path, std::string* error = nullptr)
    {
        std::ofstream file(path, std::ios::out | std::ios::app);
        if (!file)
        {
            if (error) *error = "Could not append wheel-specific input options to the profile.";
            return false;
        }
        file << "\n[WheelInputProfile]\n";
        file << "Version = 1\n";
        file << "InputBackend = " << Settings::InputBackend.to_string() << "\n";
        file << "SteeringDeadZone = " << Settings::SteeringDeadZone.to_string() << "\n";
        file << "BypassGameSensitivity = " << Settings::BypassGameSensitivity.to_string() << "\n";
        file << "FFBDeviceName = " << Settings::WheelFFBDeviceName.to_string() << "\n";
        file << "FFBDeviceGuid = " << Settings::WheelFFBDeviceGuid.to_string() << "\n";
        file << "FFBResponseCorrection = " << Settings::WheelFFBResponseCorrection.to_string() << "\n";
        file << "FFBResponseLUT = " << Settings::WheelFFBResponseLUT.to_string() << "\n";
        file << "FFBMaxTorqueNm = " << Settings::WheelFFBMaxTorqueNm.to_string() << "\n";
        file.flush();
        if (!file)
        {
            if (error) *error = "Failed while writing wheel-specific input options.";
            file.close();
            return false;
        }
        file.close();
        if (file.fail())
        {
            if (error) *error = "Failed while closing wheel-specific input options.";
            return false;
        }
        return true;
    }

    inline bool load_input_options(const std::filesystem::path& path, std::string* error = nullptr)
    {
        std::unordered_map<std::string, std::string> values;
        if (!parse_section(path, "WheelInputProfile", values, error))
            return false;

        const InputOptionsSnapshot before = capture_input_options();
        std::vector<Settings::SettingBase*> changed;
        const auto apply = [&](const char* key, Settings::SettingBase& setting) -> bool
        {
            const auto it = values.find(lower_ascii(key));
            if (it == values.end())
                return true; // Older profiles remain valid.
            const std::string oldValue = setting.to_string();
            if (!setting.set_from_string(it->second))
                return false;
            if (setting.to_string() != oldValue)
                changed.push_back(&setting);
            return true;
        };

        if (!apply("InputBackend", Settings::InputBackend) ||
            !apply("SteeringDeadZone", Settings::SteeringDeadZone) ||
            !apply("BypassGameSensitivity", Settings::BypassGameSensitivity) ||
            !apply("FFBDeviceName", Settings::WheelFFBDeviceName) ||
            !apply("FFBDeviceGuid", Settings::WheelFFBDeviceGuid) ||
            !apply("FFBResponseCorrection", Settings::WheelFFBResponseCorrection) ||
            !apply("FFBResponseLUT", Settings::WheelFFBResponseLUT) ||
            !apply("FFBMaxTorqueNm", Settings::WheelFFBMaxTorqueNm))
        {
            restore_input_options(before);
            if (error) *error = "Input profile contains an invalid wheel-specific option.";
            return false;
        }

        for (Settings::SettingBase* setting : changed)
            setting->notify();
        return true;
    }

    inline bool is_ffb_profile_setting(const Settings::SettingBase* setting)
    {
        if (!setting || setting->section() != "WheelFFB")
            return false;
        const std::string key = std::string(setting->key());
        // Device routing and diagnostics stay global. A feel profile must never
        // silently redirect torque to another wheel or turn verbose logging on.
        return key != "DeviceName" && key != "DeviceGuid" &&
            key != "Telemetry" && key != "DebugLog" &&
            key != "ResponseCorrection" && key != "ResponseLUT" &&
            key != "MaxTorqueNm";
    }

    inline std::vector<Settings::SettingBase*> ffb_settings()
    {
        std::vector<Settings::SettingBase*> result;
        for (Settings::SettingBase* setting : Settings::SettingBase::registry())
            if (is_ffb_profile_setting(setting))
                result.push_back(setting);
        std::sort(result.begin(), result.end(), [](const Settings::SettingBase* a, const Settings::SettingBase* b)
        {
            return a->key() < b->key();
        });
        return result;
    }

    inline bool save_ffb_profile(std::string_view rawName, std::string* error = nullptr)
    {
        auto path = profile_path(Kind::ForceFeedback, rawName, error);
        if (!path)
            return false;

        const auto staged = staged_profile_path(*path);
        {
            std::error_code ignored;
            std::filesystem::remove(staged, ignored);
        }
        std::ofstream file(staged, std::ios::out | std::ios::trunc);
        if (!file)
        {
            if (error) *error = "Could not open staged FFB profile for writing.";
            return false;
        }
        file << "# OutRun2006Tweaks named force-feedback feel profile.\n";
        file << "# DeviceName/DeviceGuid and diagnostic logging are intentionally not stored here.\n\n";
        file << "[Profile]\nType = ForceFeedback\nVersion = 1\n\n";
        file << "[WheelFFB]\n";
        for (const Settings::SettingBase* setting : ffb_settings())
            file << setting->key() << " = " << setting->to_string() << "\n";
        file.flush();
        if (!file)
        {
            if (error) *error = "Failed while writing staged FFB profile.";
            file.close();
            std::error_code ignored;
            std::filesystem::remove(staged, ignored);
            return false;
        }
        file.close();
        if (file.fail())
        {
            if (error) *error = "Failed while closing staged FFB profile.";
            std::error_code ignored;
            std::filesystem::remove(staged, ignored);
            return false;
        }
        if (!commit_staged_profile(staged, *path, error))
        {
            std::error_code ignored;
            std::filesystem::remove(staged, ignored);
            return false;
        }
        return true;
    }

    inline bool load_ffb_profile(
        std::string_view rawName, int* appliedCount = nullptr, std::string* error = nullptr)
    {
        auto path = profile_path(Kind::ForceFeedback, rawName, error);
        if (!path)
            return false;

        std::unordered_map<std::string, std::string> values;
        if (!parse_section(*path, "WheelFFB", values, error))
            return false;

        auto settings = ffb_settings();
        std::vector<std::string> before;
        before.reserve(settings.size());
        for (Settings::SettingBase* setting : settings)
            before.push_back(setting->to_string());

        int applied = 0;
        std::vector<Settings::SettingBase*> changed;
        for (size_t i = 0; i < settings.size(); ++i)
        {
            Settings::SettingBase* setting = settings[i];
            const auto it = values.find(lower_ascii(std::string(setting->key())));
            if (it == values.end())
                continue; // Forward/backward-compatible partial profile.
            if (!setting->set_from_string(it->second))
            {
                for (size_t restore = 0; restore < settings.size(); ++restore)
                    settings[restore]->set_from_string(before[restore]);
                if (error) *error = "FFB profile contains an invalid value for " + std::string(setting->key()) + ".";
                return false;
            }
            if (setting->to_string() != before[i])
                changed.push_back(setting);
            ++applied;
        }

        if (applied == 0)
        {
            if (error) *error = "FFB profile contains no recognized force settings.";
            return false;
        }

        for (Settings::SettingBase* setting : changed)
            setting->notify();
        if (appliedCount) *appliedCount = applied;
        return true;
    }
}
