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

void Overlay_GlobalsWindow()
{
	extern bool EnablePauseMenu;

	bool settingsChanged = false;

	ImGui::Begin("Globals", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.51f, 0.00f, 0.14f, 0.00f));
	if (ImGui::Button("-"))
	{
		if (Overlay::GlobalFontScale > 1.0f)
		{
			Overlay::GlobalFontScale -= 0.05f;
			settingsChanged = true;
		}

		ImGui::GetIO().FontGlobalScale = Overlay::GlobalFontScale;
	}

	ImGui::SameLine();

	if (ImGui::Button("+"))
	{
		if (Overlay::GlobalFontScale < 4.0f)
		{
			Overlay::GlobalFontScale += 0.05f;
			settingsChanged = true;
		}

		ImGui::GetIO().FontGlobalScale = Overlay::GlobalFontScale;
	}

	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::Text("Overlay Font Size");

	ImGui::Separator();
	ImGui::Text("Info");
	EVWORK_CAR* car = Game::pl_car();
	ImGui::Text("game_mode: %d", *Game::game_mode);
	ImGui::Text("current_mode: %d", *Game::current_mode);
	ImGui::Text("Lobby is online: %d", (*Game::SumoNet_CurNetDriver && (*Game::SumoNet_CurNetDriver)->is_online_driver()));
	ImGui::Text("Lobby is online host: %d", (*Game::SumoNet_CurNetDriver && (*Game::SumoNet_CurNetDriver)->is_hosting_online()));
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

	ImGui::End();

	if (settingsChanged)
		Overlay::settings_write();
}

void Overlay_NotificationSettings()
{
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

		ImGui::Begin("Notification Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
		ImGui::SetWindowPos(ImVec2(startX, curY), ImGuiCond_FirstUseEver);

		settingsChanged |= ImGui::SliderInt("Display Time", &Overlay::NotifyDisplayTime, 0, 60);
		settingsChanged |= ImGui::Checkbox("Enable Online Lobby Notifications", &Overlay::NotifyOnlineEnable);
		settingsChanged |= ImGui::SliderInt("Online Update Time", &Overlay::NotifyOnlineUpdateTime, 10, 60);

		static const char* items[]{ "Never Hide", "Online Race", "Any Race" };
		settingsChanged |= ImGui::Combo("Hide During", &Overlay::NotifyHideMode, items, IM_ARRAYSIZE(items));

		ImGui::End();

		if (settingsChanged)
			Overlay::settings_write();
	}
}

void Overlay_Init()
{
	Overlay::settings_read();

	Notifications::instance.add("OutRun2006Tweaks v" MODULE_VERSION_STR " by emoose!\nPress F11 to open overlay.");

	if (Overlay::CourseReplacementEnabled)
		Notifications::instance.add("Note: Course Editor Override is enabled from previous session.");

	void ServerNotifications_Init();
	ServerNotifications_Init();
}

bool Overlay_Update()
{
	bool f11_pressed = (GetAsyncKeyState(VK_F11) & 1);
	if (f11_prev_state && !f11_pressed) // f11 was pressed and now released?
	{
		overlay_visible = !overlay_visible;
		if (overlay_visible)
			ShowCursor(true);
		else
			ShowCursor(false);
	}

	f11_prev_state = f11_pressed;

	// Start the Dear ImGui frame
	ImGui::NewFrame();

	if (!overlay_visible)
		Notifications::instance.render();
	else
	{
#ifdef _DEBUG
		static bool show_demo_window = true;
		// 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
		if (show_demo_window)
		{
			ImGui::ShowDemoWindow(&show_demo_window);
		}
#endif

		Overlay_GlobalsWindow();
		Overlay_NotificationSettings();

		void Overlay_CourseEditor();
		Overlay_CourseEditor();

		if (Game::DrawDistanceDebugEnabled)
		{
			void DrawDist_DrawOverlay();
			DrawDist_DrawOverlay();
		}
	}

	ImGui::EndFrame();

	return true;
}

namespace Overlay
{
	bool settings_read()
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

		NotifyDisplayTime = ini.Get("Notifications", "DisplayTime", NotifyDisplayTime);
		NotifyOnlineEnable = ini.Get("Notifications", "OnlineEnable", NotifyOnlineEnable);
		NotifyOnlineUpdateTime = ini.Get("Notifications", "OnlineUpdateTime", NotifyOnlineUpdateTime);
		NotifyHideMode = ini.Get("Notifications", "HideMode", NotifyHideMode);

		CourseReplacementEnabled = ini.Get("CourseReplacement", "Enabled", CourseReplacementEnabled);
		std::string CourseCode;
		CourseCode = ini.Get("CourseReplacement", "Code", CourseCode);
		strcpy_s(CourseReplacementCode, CourseCode.c_str());

		return true;
	}

	bool settings_write()
	{
		inih::INIReader ini;
		ini.Set("Overlay", "FontScale", GlobalFontScale);

		ini.Set("Notifications", "DisplayTime", NotifyDisplayTime);
		ini.Set("Notifications", "OnlineEnable", NotifyOnlineEnable);
		ini.Set("Notifications", "OnlineUpdateTime", NotifyOnlineUpdateTime);
		ini.Set("Notifications", "HideMode", NotifyHideMode);

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
};