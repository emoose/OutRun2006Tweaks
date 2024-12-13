#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include "notifications.hpp"

StageTable_mb CustomStageTable[0x40];
int CustomStageTableCount = 0;

class CourseReplacement : public Hook
{
	inline static SafetyHookMid midhook{};
	static void dest(SafetyHookContext& ctx)
	{
		int* SumoScript_StageTableCount = Module::exe_ptr<int>(0x3D33C4);

		// Loaded stage table ptr is inside ctx.ebx
		// If replacement is disabled we'll copy the loaded one into our replacement table, so user has something to modify
		if (!Game::CourseReplacementEnabled || !CustomStageTableCount)
		{
			CustomStageTableCount = *SumoScript_StageTableCount;
			memcpy(CustomStageTable, (void*)ctx.ebx, sizeof(StageTable_mb) * CustomStageTableCount);
		}
		else
		{
			ctx.ebx = (uintptr_t)CustomStageTable;
			ctx.esi = ctx.ebx;
			*SumoScript_StageTableCount = CustomStageTableCount;
		}
	}

	inline static SafetyHookInline SumoLiveUpdate_Init_hook{};
	static void SumoLiveUpdate_Init_dest()
	{
		int* SumoLiveUpdate_State = Module::exe_ptr<int>(0x436D14);
		if (Game::CourseReplacementEnabled)
			*SumoLiveUpdate_State = 3;
	}

public:
	std::string_view description() override
	{
		return "CourseReplacement";
	}

	bool validate() override
	{
		return Settings::OverlayEnabled;
	}

	bool apply() override
	{
		midhook = safetyhook::create_mid(Module::exe_ptr(0x4D9A1), dest);
		SumoLiveUpdate_Init_hook = safetyhook::create_inline(Module::exe_ptr(0x9D4A0), SumoLiveUpdate_Init_dest);
		return !!midhook;
	}

	static CourseReplacement instance;
};
CourseReplacement CourseReplacement::instance;

bool update_stage(StageTable_mb* tableEntry, GameStage newStage, bool isGoalColumn)
{
	tableEntry->StageUniqueName_0 = newStage;
	tableEntry->CsInfoId_1C = tableEntry->BrInfoId_20 = newStage;

	TAG_model_info** cs_info_tbl = Module::exe_ptr<TAG_model_info*>(0x2A54E0);
	TAG_model_info** br_info_tbl = Module::exe_ptr<TAG_model_info*>(0x2A55E8);

	if (newStage == STAGE_PALM_BEACH_T || newStage == STAGE_PALM_BEACH_BT)
		tableEntry->BrInfoId_20 = STAGE_PALM_BEACH;
	else if (newStage == STAGE_BEACH_T || newStage == STAGE_BEACH_BT)
		tableEntry->BrInfoId_20 = STAGE_BEACH;
	else if (newStage == STAGE_PALM_BEACH_BR)
		tableEntry->BrInfoId_20 = STAGE_PALM_BEACH_R;
	else if (newStage == STAGE_BEACH_BR)
		tableEntry->BrInfoId_20 = STAGE_BEACH_R;

	if (isGoalColumn)
	{
		// If stage is set to one of the existing goal-stages, use the goal bunki for it
		// (otherwise will default to normal bunki, sadly goal bunki can't be shared between maps)

		if (tableEntry->BrInfoId_20 == STAGE_TULIP_GARDEN)
			tableEntry->BrInfoId_20 = STAGE_EASTER_ISLAND_R + 1; // ENDA
		else if (tableEntry->BrInfoId_20 == STAGE_METROPOLIS)
			tableEntry->BrInfoId_20 = STAGE_EASTER_ISLAND_R + 2; // ENDB
		else if (tableEntry->BrInfoId_20 == STAGE_ANCIENT_RUINS)
			tableEntry->BrInfoId_20 = STAGE_EASTER_ISLAND_R + 3; // ENDC
		else if (tableEntry->BrInfoId_20 == STAGE_CAPE_WAY)
			tableEntry->BrInfoId_20 = STAGE_EASTER_ISLAND_R + 4; // ENDD
		else if (tableEntry->BrInfoId_20 == STAGE_IMPERIAL_AVENUE)
			tableEntry->BrInfoId_20 = STAGE_EASTER_ISLAND_R + 5; // ENDE

		else if (tableEntry->BrInfoId_20 == STAGE_PALM_BEACH)
			tableEntry->BrInfoId_20 = STAGE_EASTER_ISLAND_R + 5; // ANDA (todo: what are ANDB - ANDJ for?)
	}

	tableEntry->CsInfoPtr_14 = cs_info_tbl[tableEntry->CsInfoId_1C];
	tableEntry->BrInfoPtr_18 = br_info_tbl[tableEntry->BrInfoId_20];

	return true;
}

void Overlay_CourseEditor()
{
	/*
	const char* items[] = {
		"0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
		"10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
		"20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
		"30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
		"40", "41", "42", "43", "44", "45", "46", "47", "48", "49",
		"50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
		"60", "61", "62", "63", "64", "65"
	};
	*/

	ImGui::Begin("Course Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

	// TODO: this points to list of internal-names, fill out the array above with actual stage names instead
	const char** stageNames = Module::exe_ptr<const char*>(0x235F40);

	if (!CustomStageTableCount)
	{
		ImGui::Text("Waiting for game to load stage scripts...");
		ImGui::End();
		return;
	}

	{
		bool disable_checkbox = Game::is_in_game();
		if (disable_checkbox)
			ImGui::BeginDisabled();

		ImGui::Checkbox("Enable Course Override (use in C2C OutRun mode)", &Game::CourseReplacementEnabled);

		if (disable_checkbox)
			ImGui::EndDisabled();
	}

	// TODO: editor should be disabled if playing online, since our lobbyname hack to update clients probably won't update quick enough
	// should only really be enabled when in the lobby & user is host, or user is playing offline

	StageTable_mb* stg_tbl = *Module::exe_ptr<StageTable_mb*>(0x3D3188); // stg_tbl only points to current stage

	int curPlayingColumn = -1;
	if (stg_tbl)
		curPlayingColumn = stg_tbl->StageTableIdx_4;

	bool editor_disabled = (Game::is_in_game() && Game::pl_car()->is_in_bunki()) || !Game::CourseReplacementEnabled;
	if (editor_disabled)
		ImGui::BeginDisabled();

	float comboHeight = ImGui::GetFrameHeight(); // Height of a single combobox
	float verticalSpacing = 10.0f; // Additional spacing between comboboxes
	float comboWidth = 200.0f; // Width of comboboxes

	float windowHeight = ceil(ImGui::GetWindowHeight());

	bool has_updated = false;

	int num = 0;
	StageTable_mb* curStage = CustomStageTable;
	for (int col = 0; col < 5; ++col)
	{
		float columnHeight = (comboHeight + verticalSpacing) * (col + 1) - verticalSpacing;

		// Start a new column for each level of the pyramid
		ImGui::BeginGroup();

		// Center the column vertically
		float startY = (windowHeight - columnHeight) * 0.5f;
		ImGui::SetCursorPosY(startY);

		// if player is already past this column (or playing it), disable dropdowns
		bool col_disabled = (curPlayingColumn >= num) && !editor_disabled && Game::is_in_game();

		for (int row = 0; row <= col; ++row)
		{
			if (col_disabled)
				ImGui::BeginDisabled();

			bool playingThisStage = (curPlayingColumn == num) && Game::is_in_game();

			// Highlight the current track
			if (playingThisStage)
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.75f, 0.0f, 0.75f));

			ImGui::PushID(col * 10 + row);
			ImGui::SetNextItemWidth(comboWidth);

			int selected = curStage->StageUniqueName_0;

			std::string name = std::format("{}.{}", (col + 1), (row + 1));
			if (ImGui::Combo(name.c_str(), &selected, stageNames, 0x42))
				has_updated |= update_stage(curStage, GameStage(selected), col == 4);

			curStage++;

			// Add vertical spacing between comboboxes
			if (row <= col)
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + verticalSpacing);

			ImGui::PopID();

			if (playingThisStage)
				ImGui::PopStyleColor();

			if (col_disabled)
				ImGui::EndDisabled();

			num++;
		}

		ImGui::EndGroup();

		if (col + 1 < 5)
			ImGui::SameLine(0, 20.0f);
	}

	if (editor_disabled)
	{
		ImGui::EndDisabled();

		if (Game::CourseReplacementEnabled && Game::is_in_game() && Game::pl_car()->is_in_bunki())
			ImGui::Text("(disabled during bunki)");
	}

	ImGui::End();

	/* TODO: auto-share stuff */
#if 0
	if (has_updated && *Game::SumoNet_CurNetDriver && (*Game::SumoNet_CurNetDriver)->is_hosting_online())
	{
		char lobbyNameEncoded[16] = { 0 };
		char checkByte = 0;
		for (int i = 0; i < 15; i++)
		{
			checkByte += char(CustomStageTable[i].StageUniqueName_0);
			lobbyNameEncoded[i] = char(33) + char(CustomStageTable[i].StageUniqueName_0);
		}

		lobbyNameEncoded[15] = char(33) + checkByte;
		if (lobbyNameEncoded[15] < 33) // rollover?
			lobbyNameEncoded[15] += 33;

		memcpy(Game::SumoNet_LobbyInfo->LobbyName_48, lobbyNameEncoded, 16);

		char lobbyNameDecoded[16] = { 0 };
		checkByte = 0;
		int allsum = 0;

		for (int i = 0; i < 15; i++)
		{
			char decoded = lobbyNameEncoded[i] - 33;
			checkByte += decoded;
			allsum += decoded;
		}

		checkByte = checkByte + 33;
		if (checkByte < 33)
			checkByte += 33;

		bool valid = lobbyNameEncoded[15] == checkByte && allsum != 0;
	}
#endif
}
