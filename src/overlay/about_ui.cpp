#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <ctime>
#include <format>
#include <string>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include <imgui.h>
#include "resource.h"
#include "overlay.hpp"

class AboutWindow : public OverlayWindow
{
	struct ScopedFontScale
	{
		explicit ScopedFontScale(float scale) { ImGui::SetWindowFontScale(scale); }
		~ScopedFontScale() { ImGui::SetWindowFontScale(1.0f); }
	};

	// Indents the cursor so text of this width ends up centred in the row.
	static void centre_next(const char* text)
	{
		const float avail = ImGui::GetContentRegionAvail().x;
		const float width = ImGui::CalcTextSize(text).x;

		if (width < avail)
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - width) * 0.5f);
	}

	static void text_centred(const char* text)
	{
		centre_next(text);
		ImGui::TextUnformatted(text);
	}

	static void text_centred_dim(const char* text)
	{
		centre_next(text);
		ImGui::TextDisabled("%s", text);
	}

	static int current_year()
	{
		static const int year = []
		{
			const std::time_t now = std::time(nullptr);
			std::tm local{};
			localtime_s(&local, &now);
			return local.tm_year + 1900;
		}();

		return year;
	}

	static void draw_link(const char* label, const char* url)
	{
		const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];

		ImGui::PushStyleColor(ImGuiCol_Text, accent);
		ImGui::TextUnformatted(label);
		ImGui::PopStyleColor();

		if (!ImGui::IsItemHovered())
			return;

		// Underline on hover, along the bottom of the text just drawn.
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y),
			ImGui::GetColorU32(accent));

		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		ImGui::SetTooltip("%s", url);

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			ShellExecuteA(nullptr, "open", url, 0, 0, SW_SHOWNORMAL);
	}

	static void link_centred(const char* label, const char* url)
	{
		centre_next(label);
		draw_link(label, url);
	}

public:
	Kind kind() const override { return Kind::Tab; }
	const char* name() const override { return "About"; }
	int order() const override { return 99; }

	void init() override {}

	void render(bool overlayEnabled) override
	{
		const float lineHeight = ImGui::GetTextLineHeight();

		ImGui::Dummy(ImVec2(0.0f, lineHeight));

		{
			ScopedFontScale big(2.0f);
			text_centred("OutRun2006Tweaks");
		}

		{
			ScopedFontScale small(0.9f);
			text_centred_dim("v" MODULE_VERSION_STR);
		}

		ImGui::Dummy(ImVec2(0.0f, lineHeight)); 

		const std::string copyright = std::format("Copyright (c) 2024 - {} emoose", current_year());
		text_centred_dim(copyright.c_str());
		link_centred("https://github.com/emoose/OutRun2006Tweaks", "https://github.com/emoose/OutRun2006Tweaks");

		ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.5f));

		const float measure = min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 34.0f);
		const float margin = (ImGui::GetContentRegionAvail().x - measure) * 0.5f;

		text_centred("The tweaks are free, and so are the online services.");

		ImGui::Indent(margin);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + measure);
		ImGui::TextUnformatted("Multiplayer, leaderboards and ghost downloads all run on servers paid out of pocket, keeping them up is an ongoing cost.");
		ImGui::TextUnformatted("If any of this has added something to your time with the game, a coffee goes a long way towards keeping it running.");
		ImGui::PopTextWrapPos();
		ImGui::Unindent(margin);

		ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.75f));

		link_centred("https://ko-fi.com/emoose", "https://ko-fi.com/emoose");
	}

	static AboutWindow instance;
};
AboutWindow AboutWindow::instance;
