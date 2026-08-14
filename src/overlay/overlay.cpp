#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include "notifications.hpp"
#include "resource.h"
#include "overlay.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <format>
#include <vector>
#include <ini.h>
#include "input_manager.hpp"

Notifications Notifications::instance;


bool overlay_visible = false; // user wants overlay to show?

Overlay::ContentRect Overlay::content_rect()
{
	const ImVec2 screen = ImGui::GetIO().DisplaySize;

	// Letterboxing only covers the sides, and only outside of gameplay unless
	// it's set to always.
	float border = 0.f;
	if (Settings::UILetterboxing == 1 && !Game::is_in_game())
	{
		const float contentWidth = screen.y / (3.f / 4.f);
		border = ((screen.x - contentWidth) / 2) + 0.5f;
	}

	return { border, 0.f, screen.x - (border * 2.f), screen.y };
}

const std::vector<OverlayWindow*>& Overlay::windows()
{
	static std::vector<OverlayWindow*> sorted;
	if (sorted.empty())
	{
		sorted = windows_storage();
		std::sort(sorted.begin(), sorted.end(), [](const OverlayWindow* a, const OverlayWindow* b)
		{
			if (a->kind() != b->kind())
				return a->kind() < b->kind();
			return a->order() < b->order();
		});
	}
	return sorted;
}

void Overlay::render_shell()
{
	const ContentRect content = content_rect();

	const ImVec2 windowSize(min(810.f, content.width - 40.f), min(560.f, content.height - 40.f));

	ImGui::SetNextWindowSize(windowSize, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(
		ImVec2(
			content.x + (content.width - windowSize.x) * 0.5f,
			content.y + (content.height - windowSize.y) * 0.5f
		),
		ImGuiCond_FirstUseEver
	);

	if (ImGui::Begin("OutRun2006Tweaks", nullptr, ImGuiWindowFlags_NoCollapse))
	{
		if (ImGui::BeginTabBar("##shell"))
		{
			for (OverlayWindow* window : windows())
			{
				if (window->kind() != OverlayWindow::Kind::Tab)
					continue;
#ifndef _DEBUG
				if (window->debug_only())
					continue;
#endif
				if (ImGui::BeginTabItem(window->name()))
				{
					window->render(true);
					ImGui::EndTabItem();
				}
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}

void Overlay::init()
{
	Overlay::settings_read();

	if (Overlay::CourseReplacementEnabled)
		Notifications::instance.add("Note: Course Editor Override is enabled from previous session.");

	void ServerNotifications_Init();
	ServerNotifications_Init();

	void UpdateCheck_Init();
	UpdateCheck_Init();
}

void Overlay::init_imgui()
{
	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	// Window positions and which tools were left open. Without this ImGui writes
	// imgui.ini into whatever the working directory happens to be; keep it next
	// to the DLL with the rest of our files. ImGui holds onto the pointer, so
	// the string has to outlive this call.
	static std::string imguiIniPath = (Module::DllPath.parent_path() / "OutRun2006Tweaks.imgui.ini").string();
	io.IniFilename = imguiIniPath.c_str();

	Overlay::apply_style();
	Overlay::rebuild_fonts();
}

void Overlay::rebuild_fonts()
{
	FontsDirty = false;

	// ImGui's built-in ProggyClean is a bitmap face drawn for exactly 13px, so
	// the font scale smears it. Rasterising a real typeface at the size actually
	// wanted keeps it sharp at any scale, and leaves ImGui's own scale
	// multiplier at its default of 1.0.
	constexpr float BaseFontSize = 13.0f;
	const float sizePixels = std::floor(BaseFontSize * std::clamp(GlobalFontScale, 0.5f, 5.0f));

	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();

	// Read the file rather than handing ImGui the path, so a Windows directory
	// with characters outside the active code page still works.
	std::vector<uint8_t> fontData;
	{
		wchar_t windowsDir[MAX_PATH]{};
		if (GetWindowsDirectoryW(windowsDir, MAX_PATH))
		{
			const std::filesystem::path fontPath = std::filesystem::path(windowsDir) / "Fonts" / "segoeui.ttf";

			std::ifstream file(fontPath, std::ios::binary | std::ios::ate);
			if (file)
			{
				fontData.resize(size_t(file.tellg()));
				file.seekg(0);
				if (!file.read(reinterpret_cast<char*>(fontData.data()), fontData.size()))
					fontData.clear();
			}
		}
	}

	bool loaded = false;
	if (!fontData.empty())
	{
		// AddFontFromMemoryTTF takes ownership and frees with ImGui's allocator,
		// so it gets a buffer allocated by the same one.
		void* owned = IM_ALLOC(fontData.size());
		memcpy(owned, fontData.data(), fontData.size());

		loaded = io.Fonts->AddFontFromMemoryTTF(owned, int(fontData.size()), sizePixels) != nullptr;
	}

	if (!loaded)
	{
		spdlog::warn("Overlay::rebuild_fonts - Segoe UI unavailable, falling back to the built-in font");

		ImFontConfig config;
		config.SizePixels = sizePixels;
		io.Fonts->AddFontDefault(&config);
	}

#if IMGUI_VERSION_NUM >= 19200
	// From 1.92 ImGui rasterises glyphs on demand at whatever size the style
	// asks for, so the size a font was added at no longer decides how big it
	// draws. Without this the style's own default of 13 wins and the scale
	// setting does nothing.
	ImGui::GetStyle().FontSizeBase = sizePixels;
#endif
}

// Metrics shared by every theme. Roomier than ImGui's defaults.
static void apply_common_metrics(ImGuiStyle& style)
{
	style.WindowPadding = ImVec2(12.0f, 12.0f);
	style.FramePadding = ImVec2(10.0f, 5.0f);
	style.ItemSpacing = ImVec2(10.0f, 7.0f);
	style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
	style.CellPadding = ImVec2(8.0f, 5.0f);
	style.IndentSpacing = 20.0f;
	style.ScrollbarSize = 13.0f;
	style.GrabMinSize = 11.0f;

	style.WindowBorderSize = 1.0f;
	style.ChildBorderSize = 1.0f;
	style.PopupBorderSize = 1.0f;
	style.FrameBorderSize = 0.0f;
	style.TabBarBorderSize = 1.0f;
	style.SeparatorTextBorderSize = 2.0f;

	style.WindowRounding = 10.0f;
	style.ChildRounding = 8.0f;
	style.PopupRounding = 10.0f;
	style.FrameRounding = 5.0f;
	style.ScrollbarRounding = 8.0f;
	style.GrabRounding = 5.0f;
	style.TabRounding = 6.0f;

	style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
	style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
	style.SeparatorTextPadding = ImVec2(16.0f, 6.0f);
}

// Dark Coast theme
static void apply_theme_dark_coast(ImGuiStyle& style)
{
	ImGui::StyleColorsDark();
	ImVec4* colors = style.Colors;

	const ImVec4 accent = ImVec4(0.98f, 0.49f, 0.16f, 1.00f);
	const ImVec4 accentBright = ImVec4(1.00f, 0.62f, 0.30f, 1.00f);
	const ImVec4 accentDim = ImVec4(0.55f, 0.27f, 0.09f, 1.00f);

	const ImVec4 ground = ImVec4(0.05f, 0.06f, 0.09f, 1.00f);
	const ImVec4 raised = ImVec4(0.11f, 0.13f, 0.17f, 1.00f);
	const ImVec4 raisedHover = ImVec4(0.17f, 0.20f, 0.26f, 1.00f);
	const ImVec4 line = ImVec4(0.24f, 0.28f, 0.35f, 0.55f);

	const auto tint = [](const ImVec4& c, float alpha) { return ImVec4(c.x, c.y, c.z, alpha); };

	colors[ImGuiCol_Text] = ImVec4(0.91f, 0.91f, 0.89f, 1.00f);
	colors[ImGuiCol_TextDisabled] = ImVec4(0.46f, 0.50f, 0.57f, 1.00f);

	colors[ImGuiCol_WindowBg] = ground;
	// Insets like the settings category rail read as a dip in the window rather
	// than a surface of their own, so they darken whatever is behind them
	// instead of painting over it. That keeps them working at any window opacity.
	colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.16f);
	// Popups and tooltips stay near-opaque, since they are read rather than
	// looked past.
	colors[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.08f, 0.11f, 0.97f);
	colors[ImGuiCol_Border] = line;
	colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

	colors[ImGuiCol_FrameBg] = raised;
	colors[ImGuiCol_FrameBgHovered] = raisedHover;
	colors[ImGuiCol_FrameBgActive] = accentDim;

	colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.08f, 0.11f, 1.00f);
	colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.11f, 0.09f, 1.00f);
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.07f, 0.08f, 0.11f, 0.75f);
	colors[ImGuiCol_MenuBarBg] = raised;

	colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.20f);
	colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.28f, 0.35f, 1.00f);
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
	colors[ImGuiCol_ScrollbarGrabActive] = accent;

	colors[ImGuiCol_CheckMark] = accentBright;
	colors[ImGuiCol_SliderGrab] = accent;
	colors[ImGuiCol_SliderGrabActive] = accentBright;

	colors[ImGuiCol_Button] = raised;
	colors[ImGuiCol_ButtonHovered] = raisedHover;
	colors[ImGuiCol_ButtonActive] = accentDim;

	// Selections and collapsing headers are washes of the accent rather than
	// solid fills, so a selected row doesn't outshout the text on it.
	colors[ImGuiCol_Header] = tint(accent, 0.26f);
	colors[ImGuiCol_HeaderHovered] = tint(accent, 0.40f);
	colors[ImGuiCol_HeaderActive] = tint(accent, 0.55f);

	colors[ImGuiCol_Separator] = line;
	colors[ImGuiCol_SeparatorHovered] = accentDim;
	colors[ImGuiCol_SeparatorActive] = accent;

	colors[ImGuiCol_ResizeGrip] = tint(accent, 0.20f);
	colors[ImGuiCol_ResizeGripHovered] = tint(accent, 0.50f);
	colors[ImGuiCol_ResizeGripActive] = accent;

	colors[ImGuiCol_Tab] = ImVec4(0.09f, 0.10f, 0.14f, 1.00f);
	colors[ImGuiCol_TabHovered] = tint(accent, 0.40f);
	colors[ImGuiCol_TabSelected] = ImVec4(0.16f, 0.15f, 0.15f, 1.00f);
	colors[ImGuiCol_TabSelectedOverline] = accent;
	colors[ImGuiCol_TabDimmed] = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
	colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.13f, 0.12f, 0.13f, 1.00f);
	colors[ImGuiCol_TabDimmedSelectedOverline] = accentDim;

	colors[ImGuiCol_TableHeaderBg] = raised;
	colors[ImGuiCol_TableBorderStrong] = line;
	colors[ImGuiCol_TableBorderLight] = ImVec4(0.20f, 0.23f, 0.29f, 0.35f);
	colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
}

// Coast 2 Coast theme
static void apply_theme_coast2coast(ImGuiStyle& style)
{
	ImGui::StyleColorsLight();
	ImVec4* colors = style.Colors;

	const ImVec4 accent = ImVec4(0.808f, 0.067f, 0.149f, 1.00f);     // #CE1126
	const ImVec4 accentBright = ImVec4(0.910f, 0.157f, 0.235f, 1.00f);
	const ImVec4 accentDim = ImVec4(0.561f, 0.047f, 0.102f, 1.00f);

	const ImVec4 ground = ImVec4(0.831f, 0.831f, 0.851f, 1.00f);     // window body
	const ImVec4 client = ImVec4(0.925f, 0.925f, 0.941f, 1.00f);     // fields and lists
	const ImVec4 raised = ImVec4(0.769f, 0.769f, 0.796f, 1.00f);     // button faces
	const ImVec4 raisedHover = ImVec4(0.722f, 0.722f, 0.753f, 1.00f);
	const ImVec4 line = ImVec4(0.604f, 0.604f, 0.643f, 1.00f);
	const ImVec4 ink = ImVec4(0.086f, 0.086f, 0.102f, 1.00f);

	const auto tint = [](const ImVec4& c, float alpha) { return ImVec4(c.x, c.y, c.z, alpha); };

	colors[ImGuiCol_Text] = ink;
	colors[ImGuiCol_TextDisabled] = ImVec4(0.408f, 0.408f, 0.439f, 1.00f);

	colors[ImGuiCol_WindowBg] = ground;
	colors[ImGuiCol_ChildBg] = client;
	colors[ImGuiCol_PopupBg] = ImVec4(0.898f, 0.898f, 0.914f, 0.98f);
	colors[ImGuiCol_Border] = line;
	colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

	colors[ImGuiCol_FrameBg] = client;
	colors[ImGuiCol_FrameBgHovered] = tint(accent, 0.18f);
	colors[ImGuiCol_FrameBgActive] = tint(accent, 0.32f);

	colors[ImGuiCol_TitleBg] = raised;
	colors[ImGuiCol_TitleBgActive] = tint(accent, 0.85f);
	colors[ImGuiCol_TitleBgCollapsed] = tint(raised, 0.75f);
	colors[ImGuiCol_MenuBarBg] = raised;

	colors[ImGuiCol_ScrollbarBg] = ImVec4(0.000f, 0.000f, 0.000f, 0.07f);
	colors[ImGuiCol_ScrollbarGrab] = tint(accent, 0.35f);
	colors[ImGuiCol_ScrollbarGrabHovered] = tint(accent, 0.60f);
	colors[ImGuiCol_ScrollbarGrabActive] = accent;

	colors[ImGuiCol_CheckMark] = accent;
	colors[ImGuiCol_SliderGrab] = accent;
	colors[ImGuiCol_SliderGrabActive] = accentDim;

	colors[ImGuiCol_Button] = raised;
	colors[ImGuiCol_ButtonHovered] = tint(accent, 0.75f);
	colors[ImGuiCol_ButtonActive] = tint(accent, 0.90f);

	colors[ImGuiCol_Header] = tint(accent, 0.55f);
	colors[ImGuiCol_HeaderHovered] = tint(accent, 0.75f);
	colors[ImGuiCol_HeaderActive] = tint(accent, 0.90f);

	colors[ImGuiCol_Separator] = line;
	colors[ImGuiCol_SeparatorHovered] = tint(accent, 0.70f);
	colors[ImGuiCol_SeparatorActive] = accent;

	colors[ImGuiCol_ResizeGrip] = tint(accent, 0.35f);
	colors[ImGuiCol_ResizeGripHovered] = tint(accent, 0.65f);
	colors[ImGuiCol_ResizeGripActive] = accent;

	colors[ImGuiCol_Tab] = raised;
	colors[ImGuiCol_TabHovered] = tint(accent, 0.45f);
	colors[ImGuiCol_TabSelected] = tint(accent, 0.28f);
	colors[ImGuiCol_TabSelectedOverline] = accent;
	colors[ImGuiCol_TabDimmed] = tint(raised, 0.80f);
	colors[ImGuiCol_TabDimmedSelected] = tint(accent, 0.16f);
	colors[ImGuiCol_TabDimmedSelectedOverline] = accentBright;

	colors[ImGuiCol_TableHeaderBg] = tint(accent, 0.22f);
	colors[ImGuiCol_TableBorderStrong] = line;
	colors[ImGuiCol_TableBorderLight] = ImVec4(0.678f, 0.678f, 0.714f, 1.00f);
	colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.00f, 0.00f, 0.00f, 0.04f);

	// Crisper than Dark Coast: a light ground needs a drawn edge to tell a field
	// from the window behind it, where the dark theme can rely on the fill alone.
	style.WindowRounding = 5.0f;
	style.ChildRounding = 3.0f;
	style.PopupRounding = 5.0f;
	style.FrameRounding = 3.0f;
	style.ScrollbarRounding = 4.0f;
	style.GrabRounding = 3.0f;
	style.TabRounding = 3.0f;

	style.FrameBorderSize = 1.0f;
}

// Luna Blue theme
static void apply_theme_luna(ImGuiStyle& style)
{
	ImGui::StyleColorsDark();
	ImVec4* colors = style.Colors;

	const ImVec4 navy = ImVec4(0.039f, 0.141f, 0.416f, 1.00f);   // #0A246A window frame
	const ImVec4 ground = ImVec4(0.075f, 0.239f, 0.612f, 1.00f); // window body
	const ImVec4 sunken = ImVec4(0.047f, 0.169f, 0.451f, 1.00f); // input fields, scrollbar tracks
	const ImVec4 blueDeep = ImVec4(0.000f, 0.329f, 0.890f, 1.00f);// #0054E3 active caption
	const ImVec4 blueMid = ImVec4(0.102f, 0.373f, 0.816f, 1.00f); // #1A5FD0 control faces
	const ImVec4 blueLit = ImVec4(0.118f, 0.435f, 0.867f, 1.00f); // #1E6FDD hover
	// Bright end of Luna's gradients. Only ever used where no text sits on it,
	// since white on it is well under a readable contrast.
	const ImVec4 blueSky = ImVec4(0.227f, 0.576f, 1.000f, 1.00f); // #3A93FF
	// Start button green, kept to marks and edges that carry no text on them.
	const ImVec4 greenLit = ImVec4(0.451f, 0.851f, 0.451f, 1.00f);
	const ImVec4 white = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);

	const auto tint = [](const ImVec4& c, float alpha) { return ImVec4(c.x, c.y, c.z, alpha); };

	colors[ImGuiCol_Text] = white;
	colors[ImGuiCol_TextDisabled] = ImVec4(0.659f, 0.765f, 0.925f, 1.00f);

	colors[ImGuiCol_WindowBg] = ground;
	colors[ImGuiCol_ChildBg] = tint(navy, 0.35f);
	colors[ImGuiCol_PopupBg] = ImVec4(0.063f, 0.208f, 0.545f, 0.98f);
	colors[ImGuiCol_Border] = blueSky;
	colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

	colors[ImGuiCol_FrameBg] = sunken;
	colors[ImGuiCol_FrameBgHovered] = ImVec4(0.098f, 0.294f, 0.678f, 1.00f);
	colors[ImGuiCol_FrameBgActive] = blueDeep;

	colors[ImGuiCol_TitleBg] = navy;
	colors[ImGuiCol_TitleBgActive] = blueDeep;
	colors[ImGuiCol_TitleBgCollapsed] = tint(navy, 0.80f);
	colors[ImGuiCol_MenuBarBg] = blueDeep;

	colors[ImGuiCol_ScrollbarBg] = tint(navy, 0.55f);
	colors[ImGuiCol_ScrollbarGrab] = blueLit;
	colors[ImGuiCol_ScrollbarGrabHovered] = blueSky;
	colors[ImGuiCol_ScrollbarGrabActive] = white;

	// Green for anything that reads as "on", the way the Start button was the one
	// warm thing on the bar.
	colors[ImGuiCol_CheckMark] = greenLit;
	colors[ImGuiCol_SliderGrab] = blueSky;
	colors[ImGuiCol_SliderGrabActive] = white;

	colors[ImGuiCol_Button] = blueMid;
	colors[ImGuiCol_ButtonHovered] = blueLit;
	colors[ImGuiCol_ButtonActive] = blueDeep;

	// Washes rather than solid, so the white text over a selected row keeps its
	// contrast against the window behind it.
	colors[ImGuiCol_Header] = tint(blueSky, 0.40f);
	colors[ImGuiCol_HeaderHovered] = tint(blueSky, 0.55f);
	colors[ImGuiCol_HeaderActive] = tint(blueSky, 0.70f);

	colors[ImGuiCol_Separator] = tint(blueSky, 0.50f);
	colors[ImGuiCol_SeparatorHovered] = blueSky;
	colors[ImGuiCol_SeparatorActive] = white;

	colors[ImGuiCol_ResizeGrip] = tint(blueSky, 0.35f);
	colors[ImGuiCol_ResizeGripHovered] = tint(blueSky, 0.70f);
	colors[ImGuiCol_ResizeGripActive] = white;

	colors[ImGuiCol_Tab] = ImVec4(0.063f, 0.208f, 0.545f, 1.00f);
	colors[ImGuiCol_TabHovered] = blueLit;
	colors[ImGuiCol_TabSelected] = blueMid;
	colors[ImGuiCol_TabSelectedOverline] = greenLit;
	colors[ImGuiCol_TabDimmed] = ImVec4(0.055f, 0.176f, 0.478f, 1.00f);
	colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.098f, 0.302f, 0.663f, 1.00f);
	colors[ImGuiCol_TabDimmedSelectedOverline] = tint(greenLit, 0.60f);

	colors[ImGuiCol_TableHeaderBg] = blueDeep;
	colors[ImGuiCol_TableBorderStrong] = blueSky;
	colors[ImGuiCol_TableBorderLight] = tint(blueSky, 0.35f);
	colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);

	// Fat and glossy: everything is a lozenge, and controls are padded well past
	// what the text needs. This is most of what separates it from the other
	// themes once the colours are in place.
	style.WindowRounding = 9.0f;
	style.ChildRounding = 7.0f;
	style.PopupRounding = 9.0f;
	style.FrameRounding = 9.0f;
	style.ScrollbarRounding = 12.0f;
	style.GrabRounding = 9.0f;
	style.TabRounding = 9.0f;

	style.FramePadding = ImVec2(13.0f, 7.0f);
	style.GrabMinSize = 14.0f;
	style.ScrollbarSize = 16.0f;

	style.FrameBorderSize = 1.0f;
	style.WindowBorderSize = 2.0f;
}

void Overlay::apply_style()
{
	ImGuiStyle& style = ImGui::GetStyle();

	apply_common_metrics(style);

	switch (CurrentTheme)
	{
	case Theme_Coast2Coast: apply_theme_coast2coast(style); break;
	case Theme_Luna:        apply_theme_luna(style); break;
	case Theme_Dark:        ImGui::StyleColorsDark(); break;
	case Theme_Light:       ImGui::StyleColorsLight(); break;
	case Theme_Classic:     ImGui::StyleColorsClassic(); break;
	default:                apply_theme_dark_coast(style); break;
	}

	// Every theme is drawn over game content, so the window opacity the user
	// picked applies whichever one is in use.
	style.Colors[ImGuiCol_WindowBg].w = GlobalOpacity;
}

const char* const* Overlay::theme_names()
{
	static const char* names[Theme_Count] = { "Dark Coast", "Coast 2 Coast", "Luna Blue", "ImGui Dark", "ImGui Light", "ImGui Classic" };
	return names;
}

// Shown once, the first time the overlay is opened on an install that has no
// overlay ini yet.
static void render_first_run_intro()
{
	constexpr const char* SpritesUrl = "https://github.com/envido32/OR2006Sprites/releases";
	constexpr const char* LeaderboardsUrl = "http://clarissa.port0.org";
	constexpr const char* Title = "Welcome##firstrun";

	if (!ImGui::IsPopupOpen(Title))
		ImGui::OpenPopup(Title);

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(display.x * 0.2f, 0.0f), ImGuiCond_Appearing);

	if (!ImGui::BeginPopupModal(Title, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize))
		return;

	// 0 wraps at the window's right edge.
	ImGui::PushTextWrapPos(0.0f);

	ImGui::SeparatorText("Welcome to the OutRun2006Tweaks overlay!");
	ImGui::Spacing();

	ImGui::TextUnformatted("Here you can configure Tweaks, setup a custom course, and adjust the overlay theme.");
	ImGui::TextUnformatted("(to change the games bindings, head to the game's Options > Controls menu)");
	ImGui::Spacing();

	ImGui::TextUnformatted("If this is your first time playing OutRun 2006 with Tweaks, a few things worth knowing:");
	ImGui::Spacing();

	ImGui::Bullet();
	ImGui::TextUnformatted("Letterboxing is enabled in menus by default, but racing is full widescreen.");

	ImGui::Bullet();
	ImGui::TextUnformatted("Online play and leaderboards are back! Create an account from the game's menus to "
		"race online, download ghosts to run against, and post times.");
	ImGui::Indent();
	if (ImGui::Button("Open leaderboards website"))
		ShellExecuteA(nullptr, "open", LeaderboardsUrl, 0, 0, SW_SHOWNORMAL);
	ImGui::Unindent();

	ImGui::Bullet();
	ImGui::TextUnformatted("The game's default UI textures were made for 2006 machines and are very low resolution. "
		"HD replacements can be downloaded from the OR2006Sprites repo:");

	ImGui::Indent();
	if (ImGui::Button(SpritesUrl))
		ShellExecuteA(nullptr, "open", SpritesUrl, 0, 0, SW_SHOWNORMAL);
	ImGui::Unindent();

	ImGui::Spacing();
	ImGui::TextUnformatted("Have fun, and enjoy the Beautiful Journey!");
	ImGui::TextDisabled("- emoose");

	ImGui::PopTextWrapPos();

	ImGui::Spacing();
	ImGui::Separator();

	if (ImGui::Button("Let's go", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
	{
		Overlay::IsFirstRun = false;
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

void ForceShowCursor(bool show)
{
	int counter = 0;

	// Adjust the counter until the cursor visibility matches the desired state
	do
	{
		counter = ShowCursor(show);
	} while ((show && counter < 0) || (!show && counter >= 0));
}

bool Overlay::render()
{
	IsActive = false;

	if (!s_hasInited)
	{
		for (const auto& wnd : windows())
			wnd->init();

		// Named here rather than in Overlay::init, which runs from apply() before
		// the game has a window: InputManager loads its bindings off WindowInit,
		// so until then the toggle has no key to report.
		Notifications::instance.add(
			std::format("OutRun2006Tweaks v" MODULE_VERSION_STR " by emoose!\nPress {} to open overlay.",
				InputManager_ModActionDisplayName(ModAction::OverlayToggle)), 0,
			[]() {
				std::string url = "https://github.com/emoose/OutRun2006Tweaks";
				ShellExecuteA(nullptr, "open", url.c_str(), 0, 0, SW_SHOWNORMAL);
			});

		s_hasInited = true;
	}

	// The bound action when the new input system is on, F11 otherwise. The
	// binding dialog owns every input while it is up, so the action reports
	// nothing there and the ImGui path is held off to match.
	// The action reports held rather than an edge, so the press is taken against
	// last frame here: the input manager updates on game ticks, which do not line
	// up with rendered frames.
	static bool overlayKeyHeld = false;
	const bool overlayKeyNow = InputManager_ModActionHeld(ModAction::OverlayToggle);

	bool toggleOverlay = overlayKeyNow && !overlayKeyHeld;
	overlayKeyHeld = overlayKeyNow;

	if (!Settings::UseNewInput && !Overlay::IsBindingDialogActive)
		toggleOverlay |= ImGui::IsKeyReleased(ImGuiKey_F11);

	if (toggleOverlay)
	{
		overlay_visible = !overlay_visible;
		ForceShowCursor(overlay_visible);
	}

	// Start the Dear ImGui frame
	ImGui::NewFrame();

	// Notifications are rendered before any other window
	Notifications::instance.render();

	if (Overlay::RequestBindingDialog)
	{
		ForceShowCursor(true);
		Overlay::IsBindingDialogActive = true;
		Overlay::RequestBindingDialog = false;
	}

	// Drawn whether or not the overlay is open.
	for (OverlayWindow* window : windows())
		if (window->kind() == OverlayWindow::Kind::Hud)
			window->render(overlay_visible);

	if (overlay_visible)
	{
		render_shell();

		// Written as soon as the intro is up rather than when it is dismissed, so
		// an alt-F4 partway through still counts as having seen it.
		if (Overlay::IsFirstRun)
		{
			static bool introSettingsWritten = false;
			if (!introSettingsWritten)
			{
				Overlay::settings_write();
				introSettingsWritten = true;
			}

			render_first_run_intro();
		}

		// Tools sit outside the shell so they can be moved and resized freely,
		// and are switched on from the list in the Debug tab.
		for (OverlayWindow* window : windows())
			if (window->kind() == OverlayWindow::Kind::Tool && window->visible)
				window->render(true);
	}

	if (Overlay::RequestMouseHide)
	{
		if (!overlay_visible)
			ForceShowCursor(false);
		Overlay::RequestMouseHide = false;
	}

	ImGui::EndFrame();

	if (overlay_visible)
		IsActive = true;

	return IsActive;
}

// Creates a unique machine ID hash, that we can use when checking if this is the
// first run of the game (in case user has overlay.ini from a different machine)
static std::string machine_id()
{
	static const std::string id = []
	{
		char name[MAX_COMPUTERNAME_LENGTH + 1]{};
		DWORD length = DWORD(std::size(name));
		if (!GetComputerNameA(name, &length))
			length = 0;

		// FNV-1a
		uint64_t hash = 0xCBF29CE484222325;
		for (DWORD i = 0; i < length; i++)
		{
			hash ^= uint8_t(name[i]);
			hash *= 0x100000001B3;
		}

		return std::format("{:016X}", hash);
	}();

	return id;
}

bool Overlay::settings_read()
{
	spdlog::info("Overlay::settings_read - reading INI from {}", Module::OverlayIniPath.string());

	// Checked before the read rather than from its result, which also fails on a
	// file that exists but is malformed - that user is not a new one.
	IsFirstRun = !std::filesystem::exists(Module::OverlayIniPath);

	inih::INIReader ini;
	try
	{
		ini = inih::INIReader(Module::OverlayIniPath);
	}
	catch (...)
	{
		spdlog::error("Overlay::settings_read - INI read failed! The file might not exist, or may have duplicate settings inside");
		return false;
	}

	// An id from some other machine means the ini came along with a copied game
	// folder, so this player still hasn't seen the introduction.
	if (!IsFirstRun)
	{
		std::string machineId;
		machineId = ini.Get("Overlay", "UniqueId", machineId);
		if (machineId != machine_id())
		{
			spdlog::info("Overlay::settings_read - INI was written on a different machine, treating as first run");
			IsFirstRun = true;
		}
	}

	GlobalFontScale = ini.Get("Overlay", "FontScale", GlobalFontScale);
	GlobalOpacity = ini.Get("Overlay", "Opacity", GlobalOpacity);
	CurrentTheme = std::clamp(ini.Get("Overlay", "Theme", CurrentTheme), 0, int(Theme_Count) - 1);

	NotifyEnable = ini.Get("Notifications", "Enable", NotifyEnable);
	NotifyDisplayTime = ini.Get("Notifications", "DisplayTime", NotifyDisplayTime);
	NotifyOnlineEnable = ini.Get("Notifications", "OnlineEnable", NotifyOnlineEnable);
	NotifyOnlineUpdateTime = ini.Get("Notifications", "OnlineUpdateTime", NotifyOnlineUpdateTime);
	NotifyHideMode = ini.Get("Notifications", "HideMode", NotifyHideMode);
	NotifyUpdateCheck = ini.Get("Notifications", "CheckForUpdates", NotifyUpdateCheck);

	ChatMode = ini.Get("Chat", "ChatMode", ChatMode);
	ChatFontSize = ini.Get("Chat", "FontSize", ChatFontSize);
	ChatHideBackground = ini.Get("Chat", "HideBackground", ChatHideBackground);

	CourseReplacementEnabled = ini.Get("CourseReplacement", "Enabled", CourseReplacementEnabled);
	std::string CourseCode;
	CourseCode = ini.Get("CourseReplacement", "Code", CourseCode);
	strcpy_s(CourseReplacementCode, CourseCode.c_str());

	return true;
}

bool Overlay::settings_write()
{
	inih::INIReader ini;
	ini.Set("Overlay", "UniqueId", machine_id());
	ini.Set("Overlay", "FontScale", GlobalFontScale);
	ini.Set("Overlay", "Opacity", GlobalOpacity);
	ini.Set("Overlay", "Theme", CurrentTheme);

	ini.Set("Notifications", "Enable", NotifyEnable);
	ini.Set("Notifications", "DisplayTime", NotifyDisplayTime);
	ini.Set("Notifications", "OnlineEnable", NotifyOnlineEnable);
	ini.Set("Notifications", "OnlineUpdateTime", NotifyOnlineUpdateTime);
	ini.Set("Notifications", "HideMode", NotifyHideMode);
	ini.Set("Notifications", "CheckForUpdates", NotifyUpdateCheck);

	ini.Set("Chat", "ChatMode", ChatMode);
	ini.Set("Chat", "FontSize", ChatFontSize);
	ini.Set("Chat", "HideBackground", ChatHideBackground);

	ini.Set("CourseReplacement", "Enabled", CourseReplacementEnabled);
	ini.Set("CourseReplacement", "Code", std::string(CourseReplacementCode));

	inih::INIWriter writer;
	try
	{
		writer.write(Module::OverlayIniPath, ini);
	}
	catch (...)
	{
		spdlog::error("Overlay::settings_write - INI write failed!");
		return false;
	}
	return true;
}

OverlayWindow::OverlayWindow()
{
	Overlay::add_window(this);
}
