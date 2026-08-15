#include "settings.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <format>
#include <stdexcept>

#include <spdlog/spdlog.h>

// ini.h names std::filesystem::path and std::runtime_error without including
// the headers that define them, so it only compiles when they arrive first.
#include <ini.h>

extern void CDSwitcher_ReadIni(const std::filesystem::path& iniPath); // hooks_audio.cpp

namespace Settings
{
	std::vector<SettingBase*>& SettingBase::registry()
	{
		static std::vector<SettingBase*> settings;
		return settings;
	}

	SettingBase::SettingBase(Type type, std::string_view section, std::string_view key,
		std::string_view description, std::vector<const char*> valueNames)
		: type_(type)
		, section_(section)
		, key_(key)
		, description_(description)
		, valueNames_(std::move(valueNames))
	{
		registry().emplace_back(this);
	}

	void SettingBase::watch(std::function<void()> onChanged)
	{
		handlers_.emplace_back(std::move(onChanged));
	}

	void SettingBase::needs_restart()
	{
		needsRestart_ = true;
	}

	void SettingBase::needs_restart(std::function<bool()> predicate)
	{
		restartPredicate_ = std::move(predicate);
	}

	bool SettingBase::restart_required() const
	{
		if (restartPredicate_)
			return restartPredicate_();
		return needsRestart_;
	}

	void SettingBase::notify() const
	{
		for (const auto& handler : handlers_)
			handler();
	}

	void mark_startup_values()
	{
		for (SettingBase* setting : SettingBase::registry())
			setting->mark_startup_value();
	}

	template <typename T>
	std::string Setting<T>::to_string() const
	{
		if constexpr (std::is_same_v<T, bool>)
			return value_ ? "true" : "false";
		else if constexpr (std::is_same_v<T, std::string>)
			return value_;
		else
			return std::format("{}", value_);
	}

	template <typename T>
	bool Setting<T>::set_from_string(std::string_view text)
	{
		try
		{
			if constexpr (std::is_same_v<T, std::string>)
			{
				value_ = std::string(text);
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				if (!_stricmp(std::string(text).c_str(), "true") ||
					!_stricmp(std::string(text).c_str(), "yes") ||
					text == "1")
				{
					value_ = true;
				}
				else if (!_stricmp(std::string(text).c_str(), "false") ||
					!_stricmp(std::string(text).c_str(), "no") ||
					text == "0")
				{
					value_ = false;
				}
				else
				{
					return false;
				}
			}
			else if constexpr (std::is_integral_v<T>)
			{
				std::string s(text);
				size_t pos = 0;

				long long parsed = std::stoll(s, &pos, 0);
				if (pos != s.size())
					return false;

				value_ = static_cast<T>(parsed);
			}
			else if constexpr (std::is_floating_point_v<T>)
			{
				std::string s(text);
				size_t pos = 0;

				double parsed = std::stod(s, &pos);
				if (pos != s.size())
					return false;

				value_ = static_cast<T>(parsed);
			}
			else
			{
				return false;
			}

			if constexpr (!std::is_same_v<T, std::string>)
			{
				if (hasRange_)
					value_ = std::clamp(value_, range_.min, range_.max);
			}

			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	template <typename T>
	void Setting<T>::read(const inih::INIReader& ini)
	{
		// The three-argument Get hands back the value it was given when the key
		// isn't in the file, so a setting missing from one INI keeps whatever an
		// earlier INI or its own default left it at.
		value_ = ini.Get<T>(std::string(section()), std::string(key()), value_);

		if constexpr (!std::is_same_v<T, std::string>)
		{
			if (hasRange_)
				value_ = std::clamp(value_, range_.min, range_.max);
		}
	}

	template <typename T>
	bool Setting<T>::write(inih::INIReader& ini) const
	{
		if (value_ == baseValue_)
			return false;

		// Written through to_string rather than Set<T> so that a bool comes out
		// as true/false instead of the 1/0 that streaming it would produce.
		ini.Set<std::string>(std::string(section()), std::string(key()), to_string());
		return true;
	}

	template class Setting<bool>;
	template class Setting<int>;
	template class Setting<float>;
	template class Setting<std::string>;

	// Settings register in whatever order their translation units initialise,
	// which link order decides. Sorting gives the log and the user INI a stable
	// layout across builds.
	static std::vector<SettingBase*> sorted_settings()
	{
		std::vector<SettingBase*> sorted = SettingBase::registry();
		std::sort(sorted.begin(), sorted.end(), [](const SettingBase* a, const SettingBase* b)
		{
			if (a->section() != b->section())
				return a->section() < b->section();
			return a->key() < b->key();
		});
		return sorted;
	}

	bool read(const std::filesystem::path& iniPath)
	{
		spdlog::info("Settings::read - reading INI from {}", iniPath.string());

		inih::INIReader ini;
		try
		{
			ini = inih::INIReader(iniPath);
		}
		catch (...)
		{
			spdlog::error("Settings::read - INI read failed! The file might not exist, or may have duplicate settings inside");
			return false;
		}

		for (SettingBase* setting : SettingBase::registry())
			setting->read(ini);

		// INIReader doesn't preserve the order of the keys/values in a section
		// Will need to try opening INI ourselves to grab cd tracks...
		CDSwitcher_ReadIni(iniPath);

		return true;
	}

	bool read_cmd_line(int argc, wchar_t** argv)
	{
		bool changed = false;

		for (int i = 1; i < argc; ++i)
		{
			std::wstring argument = argv[i];

			// Generic key=value syntax.
			//
			// Examples:
			//   -width=1920
			//   width=1920
			//   screen_width=1920
			auto equals = argument.find(L'=');

			if (equals != std::wstring::npos)
			{
				std::wstring key = argument.substr(0, equals);
				std::wstring value = argument.substr(equals + 1);

				// Allow the conventional leading '-'.
				if (!key.empty() && key[0] == L'-')
					key.erase(0, 1);

				// Convert the command-line strings to UTF-8.
				int keySize = WideCharToMultiByte(CP_UTF8, 0, key.data(), int(key.size()), nullptr, 0, nullptr, nullptr);

				int valueSize = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);

				std::string keyUtf8(keySize, '\0');
				std::string valueUtf8(valueSize, '\0');

				WideCharToMultiByte(CP_UTF8, 0, key.data(), int(key.size()), keyUtf8.data(), keySize, nullptr, nullptr);

				WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), valueUtf8.data(), valueSize, nullptr, nullptr);

				for (SettingBase* setting : SettingBase::registry())
				{
					if (_stricmp(setting->key().data(), keyUtf8.c_str()) != 0)
						continue;

					if (setting->set_from_string(valueUtf8))
					{
						spdlog::warn("Setting overridden from command line: {}={}", keyUtf8, valueUtf8);

						changed = true;
					}
					else
					{
						spdlog::error("Invalid value for command line setting {}: {}", keyUtf8, valueUtf8);
					}

					break;
				}

				continue;
			}
		}

		return changed;
	}

	void mark_base_values()
	{
		for (SettingBase* setting : SettingBase::registry())
			setting->mark_base_value();
	}

	bool write(const std::filesystem::path& iniPath)
	{
		if (Settings::DisableSettingsWrite)
		{
			spdlog::error("Settings::write - DisableSettingsWrite is true, aborting write");
			return false;
		}
		inih::INIReader ini;

		int numWritten = 0;
		for (const SettingBase* setting : sorted_settings())
			if (setting->write(ini))
				numWritten++;

		inih::INIWriter writer;
		try
		{
			writer.write(iniPath, ini);
		}
		catch (...)
		{
			spdlog::error("Settings::write - INI write failed!");
			return false;
		}

		spdlog::info("Settings::write - wrote {} changed setting(s) to {}", numWritten, iniPath.string());
		return true;
	}

	void to_log()
	{
		spdlog::info("Settings values:");

		std::unordered_map<std::string_view, std::string_view> seen_keys;

		std::string_view section;
		for (const SettingBase* setting : sorted_settings())
		{
			if (setting->section() != section)
			{
				section = setting->section();
				spdlog::info(" [{}]", section);
			}

#ifdef _DEBUG
			auto [it, inserted] = seen_keys.emplace(setting->key(), setting->section());

			if (!inserted)
			{
				spdlog::warn(
					"Duplicate setting key '{}' in [{}] and [{}]",
					setting->key(),
					it->second,
					setting->section());
			}
#endif

			spdlog::info(" - {}: {}", setting->key(), setting->to_string());
		}
	}
}
