#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include "overlay.hpp"

// Overlay tab: the overlay's own preferences. These live in a separate INI to
// the mod's settings and aren't in the Setting<> registry, so they are laid out
// by hand here rather than generated like the Settings tab's controls.
class OverlaySettingsWindow : public OverlayWindow
{
	bool showStyleEditor = false;

	// Writing the INI on every frame of a slider drag would hit the disk
	// hundreds of times, so the write waits until nothing is being dragged.
	bool settingsDirty = false;

	bool draw_appearance()
	{
		bool changed = false;

		ImGui::SeparatorText("Appearance");

		if (ImGui::Combo("Theme", &Overlay::CurrentTheme, Overlay::theme_names(), Overlay::Theme_Count))
		{
			Overlay::apply_style();
			changed = true;
		}

		// The font is rasterised at whatever scale is picked, so a change means
		// rebuilding the atlas. Only done once the slider is let go of, rather
		// than on every frame of the drag.
		static bool fontScaleHeld = false;
		if (ImGui::SliderFloat("Font Scale", &Overlay::GlobalFontScale, 0.5f, 3.5f))
			fontScaleHeld = true;
		if (fontScaleHeld && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			Overlay::FontsDirty = true;
			changed = true;
			fontScaleHeld = false;
		}

		if (ImGui::SliderFloat("Opacity", &Overlay::GlobalOpacity, 0.1f, 1.0f))
		{
			ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = Overlay::GlobalOpacity;
			changed = true;
		}

		// Debug only, as custom styles aren't currently saved
#ifdef _DEBUG
		if (ImGui::Button("Open UI Style Editor"))
			showStyleEditor = true;

		ImGui::SameLine();

		// Puts back whatever the picked theme says, discarding anything the style
		// editor changed.
		if (ImGui::Button("Reset Style"))
			Overlay::apply_style();
#endif

		return changed;
	}

	bool draw_notifications()
	{
		bool changed = false;

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

		return changed;
	}

	bool draw_chat()
	{
		bool changed = false;

		ImGui::SeparatorText("Chat");

		{
			static const char* items[]{ "Disable", "Enable", "During Menus Only" };
			changed |= ImGui::Combo("Chatroom", &Overlay::ChatMode, items, IM_ARRAYSIZE(items));
		}
		changed |= ImGui::SliderFloat("Chat Font Size", &Overlay::ChatFontSize, 0.5f, 3.5f);
		changed |= ImGui::Checkbox("Hide Chat Background", &Overlay::ChatHideBackground);

		return changed;
	}

public:
	Kind kind() const override { return Kind::Tab; }
	const char* name() const override { return "Overlay"; }
	int order() const override { return 5; }

	void init() override {}

	void render(bool overlayEnabled) override
	{
		if (showStyleEditor)
		{
			if (ImGui::Begin("UI Style Editor", &showStyleEditor))
				ImGui::ShowStyleEditor();
			ImGui::End();
		}

		settingsDirty |= draw_appearance();
		settingsDirty |= draw_notifications();
		settingsDirty |= draw_chat();

		// Waits for the control being dragged to be released.
		if (settingsDirty && !ImGui::IsAnyItemActive())
		{
			Overlay::settings_write();
			settingsDirty = false;
		}
	}

	static OverlaySettingsWindow instance;
};
OverlaySettingsWindow OverlaySettingsWindow::instance;
