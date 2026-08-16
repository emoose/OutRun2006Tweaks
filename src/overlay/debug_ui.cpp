#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "interpolation.hpp"
#include <cmath>
#include <imgui.h>
#include "overlay.hpp"

// Debug tab: game state readout, the switches for the free-floating tool
// windows, and whether each hook managed to apply.
class DebugWindow : public OverlayWindow
{
	static void draw_game_state()
	{
		uint8_t* frontEndData = *Module::exe_ptr<uint8_t*>(0x3B17E8);
		int frontEndStep = *(int*)frontEndData;
		int frontEndEvtStep = *(int*)(frontEndData + 0x218);
		char frontEndMenuLevel = *(char*)(frontEndData + 0x220);

		EVWORK_CAR* car = Game::pl_car();

		ImGui::Text("game_mode: %d", *Game::game_mode);
		ImGui::Text("current_mode: %d", *Game::current_mode);
		ImGui::Text("Frontend step indexes: %d / %d / %d", frontEndStep, frontEndEvtStep, int(frontEndMenuLevel));
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
		ImGui::Text("Loaded Stage: %d (%s / %s)", cur_stage_num,
			Game::GetStageFriendlyName(cur_stage_num), Game::GetStageUniqueName(cur_stage_num));
	}

	// Each interpolated subsystem can be switched off on its own so an artifact
	// can be narrowed to whichever one produces it.
	static void draw_interpolation()
	{
		auto& d = Interp::Debug;

		ImGui::Checkbox("Cars", &d.doCars);
		ImGui::SameLine(); ImGui::Checkbox("Camera", &d.doCamera);
		ImGui::SameLine(); ImGui::Checkbox("Particles", &d.doParticles);
		ImGui::Checkbox("Connections", &d.doConnections);
		ImGui::SameLine(); ImGui::Checkbox("Hearts", &d.doHearts);
		ImGui::SameLine(); ImGui::Checkbox("Stage scale", &d.doStageScale);

		ImGui::Checkbox("Shift particles along car display lag (off = particle velocity)",
			&d.particleUseDispLag);
		ImGui::SliderFloat("Particle shift x", &d.particleOffsetScale, 0.0f, 20.0f, "%.1f");

#ifdef _DEBUG
		ImGui::Text("alpha %.4f (effective %.4f)", d.alpha, d.effectiveAlpha);

		// An effective alpha of 1.0 makes every interpolator a no-op, which
		// otherwise looks the same as a hook that never ran.
		if (d.effectiveAlpha >= 1.0f)
			ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "effective alpha 1.0, nothing to interpolate");

		ImGui::Text("Cars replayed: %d", d.carsReplayed);
		ImGui::Text("Particles: %d sources, %d shifted, last %.4f, car lag %.4f",
			d.particleSourcesSeen, d.particlesMoved, d.particleLastShift, d.carDispLag);

		// matrix_B0 holds the transform the car is drawn with, position_14 the
		// tick position the particles were emitted against, so the gap between
		// them is the shift particles need. Applied should match it.
		if (EVWORK_CAR* plc = Game::pl_car())
		{
			const float nx = plc->matrix_B0._41 - plc->position_14.x;
			const float ny = plc->matrix_B0._42 - plc->position_14.y;
			const float nz = plc->matrix_B0._43 - plc->position_14.z;
			ImGui::Text("Shift needed %.4f, applied %.4f",
				sqrtf(nx * nx + ny * ny + nz * nz), d.particleLastShift);
		}
#endif
	}

	// These write the game's own variables rather than any of our settings, so
	// they aren't part of the generated settings tab.
	static void draw_gameplay_toggles()
	{
		extern bool EnablePauseMenu;

		ImGui::Checkbox("Countdown timer enabled", Game::Sumo_CountdownTimerEnable);
		ImGui::Checkbox("Pause menu enabled", &EnablePauseMenu);
		ImGui::Checkbox("HUD enabled", (bool*)Game::navipub_disp_flg);
	}

	static void draw_tools()
	{
		for (OverlayWindow* window : Overlay::windows())
		{
			if (window->kind() != Kind::Tool)
				continue;
#ifndef _DEBUG
			if (window->debug_only())
				continue;
#endif
			ImGui::Checkbox(window->name(), &window->visible);
		}

		if (ImGui::Button("Open Binding Dialog"))
			Overlay::IsBindingDialogActive = true;
	}

	// A hook with no description is one that never logs either, so there is
	// nothing useful to show for it.
	static void draw_hook_status()
	{
		if (!ImGui::BeginTable("##hooks", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
			return;

		ImGui::TableSetupColumn("Hook");
		ImGui::TableSetupColumn("State");
		ImGui::TableHeadersRow();

		for (Hook* hook : HookManager::hooks())
		{
			if (!hook || hook->description().empty())
				continue;

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(hook->description().data(),
				hook->description().data() + hook->description().size());

			ImGui::TableNextColumn();
			if (hook->active())
				ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.f), "applied");
			else if (hook->error())
				ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.f), "failed");
			else
				ImGui::TextDisabled("off");
		}

		ImGui::EndTable();
	}

public:
	Kind kind() const override { return Kind::Tab; }
	const char* name() const override { return "Debug"; }
	int order() const override { return 90; }

	void init() override {}

	void render(bool overlayEnabled) override
	{
		if (ImGui::CollapsingHeader("Game state", ImGuiTreeNodeFlags_DefaultOpen))
			draw_game_state();

#ifdef _DEBUG
		if (ImGui::CollapsingHeader("Interpolation", ImGuiTreeNodeFlags_DefaultOpen))
			draw_interpolation();
#endif

		if (ImGui::CollapsingHeader("Gameplay", ImGuiTreeNodeFlags_DefaultOpen))
			draw_gameplay_toggles();

		if (ImGui::CollapsingHeader("Tools", ImGuiTreeNodeFlags_DefaultOpen))
			draw_tools();

		if (ImGui::CollapsingHeader("Hooks"))
			draw_hook_status();
	}

	static DebugWindow instance;
};
DebugWindow DebugWindow::instance;
