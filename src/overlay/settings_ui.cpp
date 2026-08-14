#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include "overlay.hpp"

// Settings tab: one control per registered setting, grouped into the INI
// sections they belong to. 
// Adds all Setting<T> instances that have been registered.
class SettingsWindow : public OverlayWindow
{
	// Section order of the shipped INI. Anything registered under a section not
	// listed here is added to the end rather than dropped.
	static inline const char* SectionOrder[] = {
		"Performance", "Graphics", "Controls", "Audio", "CDSwitcher", "Window", "Misc", "Overlay", "Bugfixes",
	};

	std::vector<std::string> sections;
	int currentSection = 0;

	char searchBuffer[64] = "";

	// Writing the INI on every frame of a slider drag would hit the disk
	// hundreds of times, so the write waits until nothing is being dragged.
	bool settingsDirty = false;
	std::vector<Settings::SettingBase*> pendingNotify;

	// Case-insensitive substring match, so "vib" finds VibrationStrength. An
	// empty search matches everything.
	static bool matches_search(std::string_view text, std::string_view search)
	{
		if (search.empty())
			return true;
		if (search.size() > text.size())
			return false;

		const auto lower = [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); };

		for (size_t start = 0; start + search.size() <= text.size(); start++)
		{
			size_t i = 0;
			while (i < search.size() && lower(text[start + i]) == lower(search[i]))
				i++;

			if (i == search.size())
				return true;
		}

		return false;
	}

	// A section is worth listing if it is named by the search itself, or holds a
	// setting that is.
	static bool section_matches(std::string_view section, std::string_view search)
	{
		if (matches_search(section, search))
			return true;

		for (const Settings::SettingBase* setting : Settings::SettingBase::registry())
			if (!setting->hidden() && setting->section() == section && matches_search(setting->key(), search))
				return true;

		return false;
	}

	void draw_search_box()
	{
		// Square clear button pinned to the right, with the field filling the rest.
		const float buttonWidth = ImGui::GetFrameHeight();
		const float spacing = ImGui::GetStyle().ItemSpacing.x;

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - spacing);
		ImGui::InputTextWithHint("##search", "Search settings...", searchBuffer, sizeof(searchBuffer));

		ImGui::SameLine();

		ImGui::BeginDisabled(searchBuffer[0] == '\0');
		if (ImGui::Button("X", ImVec2(buttonWidth, buttonWidth)))
			searchBuffer[0] = '\0';
		ImGui::EndDisabled();

		ImGui::Spacing();
	}

	static void draw_description(const Settings::SettingBase* setting)
	{
		if (setting->description().empty() || !ImGui::IsItemHovered())
			return;

		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
		ImGui::TextUnformatted(setting->description().data(),
			setting->description().data() + setting->description().size());
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	// Picks a control from the setting's type: a named int is a combo box, a
	// bounded number is a slider, anything else is a plain input field.
	static bool draw_control(Settings::SettingBase* setting, const std::string& label)
	{
		switch (setting->type())
		{
		case Settings::Type::Bool:
			return ImGui::Checkbox(label.c_str(), static_cast<bool*>(setting->value_ptr()));

		case Settings::Type::Int:
		{
			auto* value = static_cast<Settings::Setting<int>*>(setting);
			const auto& names = setting->value_names();

			if (!names.empty())
				return ImGui::Combo(label.c_str(), value->ptr(), names.data(), int(names.size()));
			if (value->has_range())
				return ImGui::SliderInt(label.c_str(), value->ptr(), value->range().min, value->range().max);
			return ImGui::InputInt(label.c_str(), value->ptr());
		}

		case Settings::Type::Float:
		{
			auto* value = static_cast<Settings::Setting<float>*>(setting);
			if (value->has_range())
				return ImGui::SliderFloat(label.c_str(), value->ptr(), value->range().min, value->range().max);
			return ImGui::InputFloat(label.c_str(), value->ptr());
		}

		case Settings::Type::String:
		{
			auto* value = static_cast<Settings::Setting<std::string>*>(setting);

			// ImGui edits a fixed buffer, so the value is copied in and out
			// around the call rather than edited in place.
			char buffer[256];
			strncpy_s(buffer, value->get().c_str(), sizeof(buffer) - 1);

			if (ImGui::InputText(label.c_str(), buffer, sizeof(buffer)))
			{
				*value = std::string(buffer);
				return true;
			}
			return false;
		}
		}

		return false;
	}

public:
	Kind kind() const override { return Kind::Tab; }
	const char* name() const override { return "Settings"; }
	int order() const override { return 0; }

	void init() override
	{
		// Build the section list once, in the shipped INI's order.
		for (const char* section : SectionOrder)
			for (const Settings::SettingBase* setting : Settings::SettingBase::registry())
				if (!setting->hidden() && setting->section() == section)
				{
					sections.emplace_back(section);
					break;
				}

		for (const Settings::SettingBase* setting : Settings::SettingBase::registry())
		{
			if (setting->hidden()) continue;

			const std::string_view section = setting->section();
			const bool known = std::any_of(sections.begin(), sections.end(),
				[section](const std::string& known) { return std::string_view(known) == section; });

			if (!known)
				sections.emplace_back(section);
		}
	}

	// Names the settings that have moved since launch and can't be picked up
	// while the game runs. Computed rather than latched, so reverting one clears
	// it again.
	void draw_restart_notice()
	{
		std::string pending;
		for (const Settings::SettingBase* setting : Settings::SettingBase::registry())
		{
			if (!setting->changed_since_startup() || !setting->restart_required())
				continue;

			if (!pending.empty())
				pending += ", ";
			pending += setting->key();
		}

		if (pending.empty())
		{
			// Kept on its own line either way, so the panes above don't resize
			// as the notice comes and goes.
			ImGui::TextDisabled(" ");
			return;
		}

		ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_CheckMark],
			"Restart to apply: %s", pending.c_str());
	}

	void render(bool overlayEnabled) override
	{
		if (sections.empty())
			return;

		draw_search_box();
		const std::string_view search = searchBuffer;

		// Categories with nothing matching drop out of the list, and the
		// selection follows to the first one that still has something in it.
		std::vector<int> visibleSections;
		for (int i = 0; i < int(sections.size()); i++)
			if (section_matches(sections[i], search))
				visibleSections.emplace_back(i);

		if (visibleSections.empty())
		{
			ImGui::TextDisabled("No settings match \"%s\".", searchBuffer);
			return;
		}

		if (std::find(visibleSections.begin(), visibleSections.end(), currentSection) == visibleSections.end())
			currentSection = visibleSections.front();

		// One line kept clear at the bottom for the restart notice.
		const float footerHeight = ImGui::GetTextLineHeightWithSpacing();

		if (ImGui::BeginChild("##categories", ImVec2(ImGui::GetFontSize() * 9.0f, -footerHeight), ImGuiChildFlags_Borders))
		{
			for (int i : visibleSections)
				if (ImGui::Selectable(sections[i].c_str(), currentSection == i))
					currentSection = i;
		}
		ImGui::EndChild();

		ImGui::SameLine();

		if (ImGui::BeginChild("##settings", ImVec2(0, -footerHeight)))
		{
			const std::string& section = sections[currentSection];

			// A search that named the section itself keeps everything in it,
			// rather than only the settings whose own names happen to match.
			const bool wholeSection = matches_search(section, search);

			if (section == "Controls" && search.empty())
				if (ImGui::Button("Configure Input Bindings"))
					Overlay::IsBindingDialogActive = true;

			for (Settings::SettingBase* setting : Settings::SettingBase::registry())
			{
				if (setting->hidden() || setting->section() != std::string_view(section))
					continue;
				if (!wholeSection && !matches_search(setting->key(), search))
					continue;

				const std::string label(setting->key());

				if (draw_control(setting, label))
				{
					settingsDirty = true;
					if (std::find(pendingNotify.begin(), pendingNotify.end(), setting) == pendingNotify.end())
						pendingNotify.emplace_back(setting);
				}

				// Failsafe if user has entered framerate above 0 but less than 60
				// (since game is mainly meant for playing at 60 - it /should/ allow running at lower framerates though, but will likely break things)
				if (settingsDirty && setting == &Settings::FramerateLimit)
				{
					int val = Settings::FramerateLimit.get();
					if (val > 0 && val < 60)
						*Settings::FramerateLimit.ptr() = 60;
				}

				draw_description(setting);

				if (setting->restart_required())
				{
					ImGui::SameLine();
					ImGui::TextDisabled("(restart)");
				}
			}
		}
		ImGui::EndChild();

		draw_restart_notice();

		// Waits for the control being dragged to be released.
		if (ImGui::IsAnyItemActive())
			return;

		if (settingsDirty)
		{
			for (Settings::SettingBase* setting : pendingNotify)
				setting->notify();
			pendingNotify.clear();

			Settings::write(Module::UserIniPath);
			settingsDirty = false;
		}
	}

	static SettingsWindow instance;
};
SettingsWindow SettingsWindow::instance;
