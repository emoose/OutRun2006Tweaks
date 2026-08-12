#include "settings.hpp"

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

	void mark_base_values()
	{
		for (SettingBase* setting : SettingBase::registry())
			setting->mark_base_value();
	}

	bool write(const std::filesystem::path& iniPath)
	{
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

		std::string_view section;
		for (const SettingBase* setting : sorted_settings())
		{
			if (setting->section() != section)
			{
				section = setting->section();
				spdlog::info(" [{}]", section);
			}

			spdlog::info(" - {}: {}", setting->key(), setting->to_string());
		}
	}
}
