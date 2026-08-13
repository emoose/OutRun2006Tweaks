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
		"Performance", "Controls", "Graphics", "Audio", "CDSwitcher", "Window", "Misc", "Overlay", "Bugfixes",
	};

	std::vector<std::string> sections;
	int currentSection = 0;

	char searchBuffer[64] = "";

	bool showStyleEditor = false;

	// Writing the INI on every frame of a slider drag would hit the disk
	// hundreds of times, so the write waits until nothing is being dragged.
	bool settingsDirty = false;
	bool overlaySettingsDirty = false;

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
			if (setting->section() == section && matches_search(setting->key(), search))
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

	// The overlay keeps its own settings in a separate INI, so they aren't in
	// the registry and are laid out by hand here.
	bool draw_overlay_settings()
	{
		bool changed = false;

		ImGui::SeparatorText("Overlay");

		// The font is rasterised at whatever scale is picked, so a change means
		// rebuilding the atlas. Only done once the slider is let go of, rather
		// than on every frame of the drag.
		static bool fontScaleHeld = false;
		if (ImGui::SliderFloat("Font Scale", &Overlay::GlobalFontScale, 0.5f, 2.5f))
			fontScaleHeld = true;
		if (fontScaleHeld && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			Overlay::FontsDirty = true;
			changed = true;
			fontScaleHeld = false;
		}

		changed |= ImGui::SliderFloat("Overlay Opacity", &Overlay::GlobalOpacity, 0.1f, 1.0f);

		if (ImGui::Button("Open UI Style Editor"))
			showStyleEditor = true;

		ImGui::SameLine();

		if (ImGui::Button("Reset Style"))
		{
			Overlay::apply_style();
			ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = Overlay::GlobalOpacity;
		}

		ImGui::SeparatorText("Notifications");

		changed |= ImGui::Checkbox("Enable Notifications", &Overlay::NotifyEnable);
		changed |= ImGui::Checkbox("Enable Online Lobby Notifications", &Overlay::NotifyOnlineEnable);
		changed |= ImGui::SliderInt("Display Time", &Overlay::NotifyDisplayTime, 0, 60);
		changed |= ImGui::SliderInt("Online Update Time", &Overlay::NotifyOnlineUpdateTime, 10, 60);
		{
			static const char* items[]{ "Never Hide", "Online Race", "Any Race" };
			changed |= ImGui::Combo("Hide During", &Overlay::NotifyHideMode, items, IM_ARRAYSIZE(items));
		}
		changed |= ImGui::Checkbox("Check for Updates", &Overlay::NotifyUpdateCheck);

		ImGui::SeparatorText("Chat");

		{
			static const char* items[]{ "Disable", "Enable", "During Menus Only" };
			changed |= ImGui::Combo("Chatroom", &Overlay::ChatMode, items, IM_ARRAYSIZE(items));
		}
		changed |= ImGui::SliderFloat("Chat Font Size", &Overlay::ChatFontSize, 0.5f, 2.5f);
		changed |= ImGui::Checkbox("Hide Chat Background", &Overlay::ChatHideBackground);

		if (changed)
			ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = Overlay::GlobalOpacity;

		return changed;
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
				if (setting->section() == section)
				{
					sections.emplace_back(section);
					break;
				}

		for (const Settings::SettingBase* setting : Settings::SettingBase::registry())
		{
			const std::string_view section = setting->section();
			const bool known = std::any_of(sections.begin(), sections.end(),
				[section](const std::string& known) { return std::string_view(known) == section; });

			if (!known)
				sections.emplace_back(section);
		}
	}

	void render(bool overlayEnabled) override
	{
		if (showStyleEditor)
		{
			if (ImGui::Begin("UI Style Editor", &showStyleEditor))
				ImGui::ShowStyleEditor();
			ImGui::End();
		}

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

		if (ImGui::BeginChild("##categories", ImVec2(ImGui::GetFontSize() * 9.0f, 0), ImGuiChildFlags_Borders))
		{
			for (int i : visibleSections)
				if (ImGui::Selectable(sections[i].c_str(), currentSection == i))
					currentSection = i;
		}
		ImGui::EndChild();

		ImGui::SameLine();

		if (ImGui::BeginChild("##settings", ImVec2(0, 0)))
		{
			const std::string& section = sections[currentSection];

			// A search that named the section itself keeps everything in it,
			// rather than only the settings whose own names happen to match.
			const bool wholeSection = matches_search(section, search);

			for (Settings::SettingBase* setting : Settings::SettingBase::registry())
			{
				if (setting->section() != std::string_view(section))
					continue;
				if (!wholeSection && !matches_search(setting->key(), search))
					continue;

				const std::string label(setting->key());
				settingsDirty |= draw_control(setting, label);
				draw_description(setting);
			}

			// Overlay preferences live in their own INI, so they're appended to
			// the section the overlay's one registered setting sits in. They
			// aren't registry entries, so they are shown whole or not at all.
			if (section == "Overlay" && (search.empty() || wholeSection))
				overlaySettingsDirty |= draw_overlay_settings();
		}
		ImGui::EndChild();

		// Both writes wait for the control being dragged to be released.
		if (ImGui::IsAnyItemActive())
			return;

		if (settingsDirty)
		{
			HookManager::SettingsChanged();
			Settings::write(Module::UserIniPath);
			settingsDirty = false;
		}

		if (overlaySettingsDirty)
		{
			Overlay::settings_write();
			overlaySettingsDirty = false;
		}
	}

	static SettingsWindow instance;
};
SettingsWindow SettingsWindow::instance;
