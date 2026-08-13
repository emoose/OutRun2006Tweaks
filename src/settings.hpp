#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace inih
{
	class INIReader;
}

namespace Settings
{
	// The value types an INI setting can hold. Lets the INI code and the
	// settings UI work with a setting without knowing which Setting<T> it has.
	enum class Type
	{
		Bool,
		Int,
		Float,
		String,
	};

	// Everything about a setting that doesn't depend on its value type, so that
	// every setting can be walked through one list.
	class SettingBase
	{
	public:
		SettingBase(Type type, std::string_view section, std::string_view key,
			std::string_view description, std::vector<const char*> valueNames);

		virtual ~SettingBase() = default;

		Type type() const { return type_; }
		std::string_view section() const { return section_; }
		std::string_view key() const { return key_; }
		std::string_view description() const { return description_; }

		// Name of each value of an int setting whose values are really an enum,
		// in value order starting at 0. Empty for every other setting.
		const std::vector<const char*>& value_names() const { return valueNames_; }

		// Address of the value itself, for ImGui controls and for the game code
		// that has a pointer to a setting patched into it.
		virtual void* value_ptr() = 0;

		virtual std::string to_string() const = 0;

		// Reads this setting's key, leaving the current value in place when the
		// INI doesn't contain it.
		virtual void read(const inih::INIReader& ini) = 0;

		// Records the current value as the one the shipped INI supplied, which
		// is what write() compares against.
		virtual void mark_base_value() = 0;

		// Adds the key only when the value differs from the shipped INI, so the
		// user INI holds nothing but genuine overrides. Returns whether it wrote.
		virtual bool write(inih::INIReader& ini) const = 0;

		// Records the value the game started with, for changed_since_startup.
		virtual void mark_startup_value() = 0;
		virtual bool changed_since_startup() const = 0;

		// Runs whenever this setting changes. Only needed where a hook baked the
		// value into something at apply time: a setting nothing declares is read
		// live and takes effect on its own.
		void watch(std::function<void()> onChanged);

		// Declares that the setting requires a restart to apply.
		void needs_restart();

		// Evaluates a predicate to tell if setting needs restart to apply.
		void needs_restart(std::function<bool()> predicate);

		// Whether the value as it stands can only take effect after a restart.
		bool restart_required() const;

		// Calls the registered handlers. The settings window does this once the
		// control being dragged has been let go of, not on every frame of it.
		void notify() const;

		// Every registered setting, in no particular order. Function-local
		// static so the vector exists before the first setting registers into
		// it.
		static std::vector<SettingBase*>& registry();

	private:
		Type type_;
		std::string_view section_;
		std::string_view key_;
		std::string_view description_;
		std::vector<const char*> valueNames_;

		std::vector<std::function<void()>> handlers_;
		std::function<bool()> restartPredicate_;
		bool needsRestart_ = false;
	};

	// Bounds of a numeric setting. Clamps whatever an INI supplies, and gives
	// the settings UI the range for the slider it draws.
	template <typename T>
	struct Range
	{
		T min;
		T max;
	};

	template <typename T>
	constexpr Type deduced_type()
	{
		if constexpr (std::is_same_v<T, bool>)
			return Type::Bool;
		else if constexpr (std::is_same_v<T, int>)
			return Type::Int;
		else if constexpr (std::is_same_v<T, float>)
			return Type::Float;
		else
			return Type::String;
	}

	template <typename T>
	class Setting : public SettingBase
	{
	public:
		// For a plain value, or for an int whose values are an enum: pass the
		// name of each value in order and the settings UI draws a combo box.
		Setting(std::string_view section, std::string_view key, T defaultValue,
			std::string_view description = "", std::vector<const char*> valueNames = {})
			: SettingBase(deduced_type<T>(), section, key, description, std::move(valueNames))
			, value_(defaultValue)
			, baseValue_(defaultValue)
			, startupValue_(defaultValue)
		{
			// Naming the values fixes the range too: they start at 0 and run to
			// one less than the count, and an INI holding anything else would
			// index past the end of the names.
			if constexpr (std::is_same_v<T, int>)
			{
				if (!value_names().empty())
				{
					range_ = { 0, int(value_names().size()) - 1 };
					hasRange_ = true;
				}
			}
		}

		// For a numeric value with limits.
		Setting(std::string_view section, std::string_view key, T defaultValue,
			std::string_view description, Range<T> range)
			: SettingBase(deduced_type<T>(), section, key, description, {})
			, value_(defaultValue)
			, baseValue_(defaultValue)
			, startupValue_(defaultValue)
			, range_(range)
			, hasRange_(true)
		{
		}

		// Reads as the value itself, so existing uses such as
		// `if (Settings::UseNewInput)` and `Settings::FramerateLimit + 9`
		// carry on working unchanged.
		operator const T& () const { return value_; }

		// For the places the conversion above can't reach: calling a member on a
		// string setting, a range-for over one, and anything passed to a
		// template that would otherwise deduce Setting<T> rather than T, such as
		// spdlog's formatter or Memory::VP::Patch.
		const T& get() const { return value_; }

		Setting& operator=(const T& value) { value_ = value; return *this; }

		// Templates so that explicitly instantiating the class doesn't try to
		// compile them for the string setting, which has no -= of its own.
		template <typename U> Setting& operator+=(const U& v) { value_ += v; return *this; }
		template <typename U> Setting& operator-=(const U& v) { value_ -= v; return *this; }

		T* ptr() { return &value_; }
		void* value_ptr() override { return &value_; }

		// The value the game started with, for a needs_restart predicate that
		// only blocks certain transitions.
		const T& startup_value() const { return startupValue_; }
		bool changed_since_startup() const override { return value_ != startupValue_; }
		void mark_startup_value() override { startupValue_ = value_; }

		bool has_range() const { return hasRange_; }
		const Range<T>& range() const { return range_; }

		std::string to_string() const override;
		void read(const inih::INIReader& ini) override;
		void mark_base_value() override { baseValue_ = value_; }
		bool write(inih::INIReader& ini) const override;

	private:
		T value_;

		// Value the shipped INI left this setting at, so write() can tell an
		// override apart from a default.
		T baseValue_;

		// Value in force once the hooks had applied.
		T startupValue_;

		Range<T> range_{};
		bool hasRange_ = false;
	};

	// Only the four types an INI can carry are ever instantiated, so their
	// members are compiled once in settings.cpp rather than in every hook file.
	extern template class Setting<bool>;
	extern template class Setting<int>;
	extern template class Setting<float>;
	extern template class Setting<std::string>;

	// Reads every registered setting, leaving any the file doesn't mention at
	// the value it already holds.
	bool read(const std::filesystem::path& iniPath);

	// Call once the shipped INI has been read and before the user INI is, so
	// that write() can tell which settings are overrides.
	void mark_base_values();

	// Writes only the settings that differ from the shipped INI. Aimed at the
	// user INI: inih's writer rebuilds a file from the keys it parsed, which
	// would drop every comment in the shipped one.
	bool write(const std::filesystem::path& iniPath);

	// Call once the hooks have applied. Everything after this counts as a change
	// made during the session, which is what the restart notice is built from.
	void mark_startup_values();

	void to_log();
}
