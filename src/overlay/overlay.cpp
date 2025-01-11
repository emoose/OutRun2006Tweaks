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
#include <ini.h>

Notifications Notifications::instance;

bool show_demo_window = true;

bool f11_prev_state = false; // previously seen F11 state

bool overlay_visible = false; // user wants overlay to show?

class GlobalsWindow : public OverlayWindow
{
public:
	void init() override {}
	void render(bool overlayEnabled) override
	{
		if (!overlayEnabled)
			return;

		extern bool EnablePauseMenu;

		bool settingsChanged = false;

		if (ImGui::Begin("Globals", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Info");
			EVWORK_CAR* car = Game::pl_car();
			ImGui::Text("game_mode: %d", *Game::game_mode);
			ImGui::Text("current_mode: %d", *Game::current_mode);
			ImGui::Text("Lobby is active: %d", (*Game::SumoNet_CurNetDriver && (*Game::SumoNet_CurNetDriver)->is_in_lobby()));
			ImGui::Text("Lobby is host: %d", (*Game::SumoNet_CurNetDriver && (*Game::SumoNet_CurNetDriver)->is_hosting()));
			ImGui::Text("Is MP gamemode: %d", (*Game::game_mode == 3 || *Game::game_mode == 4));
			ImGui::Text("Car kind: %d", int(car->car_kind_11));
			ImGui::Text("Car position: %.3f %.3f %.3f", car->position_14.x, car->position_14.y, car->position_14.z);
			ImGui::Text("OnRoadPlace coli %d, stg %d, section %d",
				car->OnRoadPlace_5C.loadColiType_0,
				car->OnRoadPlace_5C.curStageIdx_C,
				car->OnRoadPlace_5C.roadSectionNum_8);

			GameStage cur_stage_num = *Game::stg_stage_num;
			ImGui::Text("Loaded Stage: %d (%s / %s)", cur_stage_num, Game::GetStageFriendlyName(cur_stage_num), Game::GetStageUniqueName(cur_stage_num));

			if (Settings::DrawDistanceIncrease > 0)
				if (ImGui::Button("Open Draw Distance Debugger"))
					Game::DrawDistanceDebugEnabled = true;

#ifdef _DEBUG
			if (ImGui::Button("Open Binding Dialog"))
				Overlay::IsBindingDialogActive = true;
#endif

			ImGui::Separator();
			ImGui::Text("Gameplay");

			ImGui::Checkbox("Countdown timer enabled", Game::Sumo_CountdownTimerEnable);
			ImGui::Checkbox("Pause menu enabled", &EnablePauseMenu);
			ImGui::Checkbox("HUD enabled", (bool*)Game::navipub_disp_flg);

			ImGui::Separator();
			ImGui::Text("Controls");

			ImGui::SliderFloat("SteeringDeadZone", &Settings::SteeringDeadZone, 0.01, 1.0f);
			ImGui::SliderInt("VibrationStrength", &Settings::VibrationStrength, 0, 10);
			ImGui::SliderFloat("ImpulseVibrationLeftMultiplier", &Settings::ImpulseVibrationLeftMultiplier, 0.1, 1);
			ImGui::SliderFloat("ImpulseVibrationRightMultiplier", &Settings::ImpulseVibrationRightMultiplier, 0.1, 1);

			ImGui::Separator();
			ImGui::Text("Graphics");

			ImGui::SliderInt("FramerateLimit", &Settings::FramerateLimit, 30, 300);
			ImGui::SliderInt("DrawDistanceIncrease", &Settings::DrawDistanceIncrease, 0, 4096);
			ImGui::SliderInt("DrawDistanceBehind", &Settings::DrawDistanceBehind, 0, 4096);
		}

		ImGui::End();

		if (settingsChanged)
			Overlay::settings_write();
	}
	static GlobalsWindow instance;
};
GlobalsWindow GlobalsWindow::instance;

class UISettingsWindow : public OverlayWindow
{
public:
	void init() override {}
	void render(bool overlayEnabled) override
	{
		if (!overlayEnabled)
			return;

		ImVec2 screenSize = ImGui::GetIO().DisplaySize;

		// Calculate starting position for the latest notification
		// (move it outside of letterbox if letterboxing enabled)
		float contentWidth = screenSize.y / (3.f / 4.f);
		float borderWidth = ((screenSize.x - contentWidth) / 2) + 0.5f;
		if (Settings::UILetterboxing != 1 || Game::is_in_game())
			borderWidth = 0;

		float startX = screenSize.x - notificationSize.x - borderWidth - 10.f;  // 10px padding from the right
		float curY = (screenSize.y / 4.0f);

		{
			bool settingsChanged = false;

			if (ImGui::Begin("UI Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::SetWindowPos(ImVec2(startX, curY), ImGuiCond_FirstUseEver);

				if (ImGui::TreeNodeEx("Global", ImGuiTreeNodeFlags_DefaultOpen))
				{
					static bool fontScaleChanged = false;
					if (ImGui::SliderFloat("Font Scale", &Overlay::GlobalFontScale, 0.5f, 2.5f))
						fontScaleChanged |= true;

					if (fontScaleChanged)
						if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
						{
							settingsChanged |= true;
							fontScaleChanged = false;
						}

					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.51f, 0.00f, 0.14f, 0.00f));
					if (ImGui::Button("-"))
					{
						if (Overlay::GlobalFontScale > 1.0f)
						{
							Overlay::GlobalFontScale -= 0.05f;
							settingsChanged = true;
						}
					}

					ImGui::SameLine();

					if (ImGui::Button("+"))
					{
						if (Overlay::GlobalFontScale < 5.0f)
						{
							Overlay::GlobalFontScale += 0.05f;
							settingsChanged = true;
						}
					}

					ImGui::PopStyleColor();

					settingsChanged |= ImGui::SliderFloat("Overlay Opacity", &Overlay::GlobalOpacity, 0.1f, 1.0f);

					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Notifications", ImGuiTreeNodeFlags_DefaultOpen))
				{
					settingsChanged |= ImGui::Checkbox("Enable Notifications", &Overlay::NotifyEnable);
					settingsChanged |= ImGui::Checkbox("Enable Online Lobby Notifications", &Overlay::NotifyOnlineEnable);

					settingsChanged |= ImGui::SliderInt("Display Time", &Overlay::NotifyDisplayTime, 0, 60);
					settingsChanged |= ImGui::SliderInt("Online Update Time", &Overlay::NotifyOnlineUpdateTime, 10, 60);

					static const char* items[]{ "Never Hide", "Online Race", "Any Race" };
					settingsChanged |= ImGui::Combo("Hide During", &Overlay::NotifyHideMode, items, IM_ARRAYSIZE(items));

					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Chat", ImGuiTreeNodeFlags_DefaultOpen))
				{
					static const char* items[]{ "Disable", "Enable", "During Menus Only" };
					settingsChanged |= ImGui::Combo("Chatroom", &Overlay::ChatMode, items, IM_ARRAYSIZE(items));
					settingsChanged |= ImGui::SliderFloat("Chat Font Size", &Overlay::ChatFontSize, 0.5f, 2.5f);
					settingsChanged |= ImGui::Checkbox("Hide Chat Background", &Overlay::ChatHideBackground);

					ImGui::TreePop();
				}
			}

			ImGui::End();

			if (settingsChanged)
			{
				ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = Overlay::GlobalOpacity;
				ImGui::GetIO().FontGlobalScale = Overlay::GlobalFontScale;

				Overlay::settings_write();
			}
		}
	}
	static UISettingsWindow instance;
};
UISettingsWindow UISettingsWindow::instance;

void Overlay::init()
{
	Overlay::settings_read();

	Notifications::instance.add("OutRun2006Tweaks v" MODULE_VERSION_STR " by emoose!\nPress F11 to open overlay.", 0,
		[]() {
			std::string url = "https://github.com/emoose/OutRun2006Tweaks";
			ShellExecuteA(nullptr, "open", url.c_str(), 0, 0, SW_SHOWNORMAL);
		});

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
	io.FontGlobalScale = Overlay::GlobalFontScale;

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsLight();
	
	ImGuiStyle& style = ImGui::GetStyle();
	style.PopupRounding = 20.0f;
	style.WindowRounding = 20.0f;
	style.ChildRounding = 20.0f;
	style.FrameRounding = 6.0f;  // For buttons and other frames
	style.ScrollbarRounding = 6.0f;
	style.GrabRounding = 6.0f;   // For sliders and scrollbars
	style.TabRounding = 6.0f;

	ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = Overlay::GlobalOpacity;
	ImGui::GetIO().FontGlobalScale = Overlay::GlobalFontScale;
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
		for (const auto& wnd : s_windows)
			wnd->init();
		s_hasInited = true;
	}

	if (ImGui::IsKeyReleased(ImGuiKey_F11))
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

	for (const auto& wnd : s_windows)
		wnd->render(overlay_visible);

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

bool Overlay::settings_read()
{
	spdlog::info("Overlay::settings_read - reading INI from {}", Module::OverlayIniPath.string());

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

	GlobalFontScale = ini.Get("Overlay", "FontScale", GlobalFontScale);
	GlobalOpacity = ini.Get("Overlay", "Opacity", GlobalOpacity);

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
	ini.Set("Overlay", "FontScale", GlobalFontScale);
	ini.Set("Overlay", "Opacity", GlobalOpacity);

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
