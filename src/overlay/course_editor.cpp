#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include "notifications.hpp"
#include <array>
#include <random>

std::array<StageTable_mb, 0x40> CustomStageTable;
int CustomStageTableCount = 0;

char ShareCode[256];

const std::vector<GameStage> stages_or2_day = {
	STAGE_PALM_BEACH,
	STAGE_DEEP_LAKE,
	STAGE_INDUSTRIAL_COMPLEX,
	STAGE_ALPINE,
	STAGE_SNOW_MOUNTAIN,
	STAGE_CLOUDY_HIGHLAND,
	STAGE_CASTLE_WALL,
	STAGE_GHOST_FOREST,
	STAGE_CONIFEROUS_FOREST,
	STAGE_DESERT,
	STAGE_TULIP_GARDEN,
	STAGE_METROPOLIS,
	STAGE_ANCIENT_RUINS,
	STAGE_CAPE_WAY,
	STAGE_IMPERIAL_AVENUE
};
const std::vector<GameStage> stages_or2_night = {
	STAGE_PALM_BEACH_BT,
	STAGE_PALM_BEACH_BR,
};
const std::vector<GameStage> stages_or2_reverse = {
	STAGE_PALM_BEACH_R,
	STAGE_DEEP_LAKE_R,
	STAGE_INDUSTRIAL_COMPLEX_R,
	STAGE_ALPINE_R,
	STAGE_SNOW_MOUNTAIN_R,
	STAGE_CLOUDY_HIGHLAND_R,
	STAGE_CASTLE_WALL_R,
	STAGE_GHOST_FOREST_R,
	STAGE_CONIFEROUS_FOREST_R,
	STAGE_DESERT_R,
	STAGE_TULIP_GARDEN_R,
	STAGE_METROPOLIS_R,
	STAGE_ANCIENT_RUINS_R,
	STAGE_CAPE_WAY_R,
	STAGE_IMPERIAL_AVENUE_R,
};
const std::vector<GameStage> stages_or2sp_day = {
	STAGE_BEACH,
	STAGE_SEQUOIA,
	STAGE_NIAGARA,
	STAGE_LAS_VEGAS,
	STAGE_ALASKA,
	STAGE_GRAND_CANYON,
	STAGE_SAN_FRANCISCO,
	STAGE_AMAZON,
	STAGE_MACHU_PICCHU,
	STAGE_YOSEMITE,
	STAGE_MAYA,
	STAGE_NEW_YORK,
	STAGE_PRINCE_EDWARD,
	STAGE_FLORIDA,
	STAGE_EASTER_ISLAND
};
const std::vector<GameStage> stages_or2sp_night = {
	STAGE_BEACH_BT,
	STAGE_BEACH_BR
};
const std::vector<GameStage> stages_or2sp_reverse = {
	STAGE_BEACH_R,
	STAGE_SEQUOIA_R,
	STAGE_NIAGARA_R,
	STAGE_LAS_VEGAS_R,
	STAGE_ALASKA_R,
	STAGE_GRAND_CANYON_R,
	STAGE_SAN_FRANCISCO_R,
	STAGE_AMAZON_R,
	STAGE_MACHU_PICCHU_R,
	STAGE_YOSEMITE_R,
	STAGE_MAYA_R,
	STAGE_NEW_YORK_R,
	STAGE_PRINCE_EDWARD_R,
	STAGE_FLORIDA_R,
	STAGE_EASTER_ISLAND_R,
};

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
			CustomStageTableCount = min(*SumoScript_StageTableCount, CustomStageTable.size());
			memcpy(CustomStageTable.data(), (void*)ctx.ebx, sizeof(StageTable_mb) * CustomStageTableCount);
		}
		else
		{
			ctx.ebx = (uintptr_t)CustomStageTable.data();
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

void sharecode_generate();

bool update_stage(StageTable_mb* tableEntry, GameStage newStage, int stageTableId)
{
	tableEntry->StageUniqueName_0 = newStage;
	tableEntry->CsInfoId_1C = newStage;

	tableEntry->BrInfoId_20 = 0; // 0 allows highway to always spawn

	TAG_model_info** cs_info_tbl = Module::exe_ptr<TAG_model_info*>(0x2A54E0);
	TAG_model_info** br_info_tbl = Module::exe_ptr<TAG_model_info*>(0x2A55E8);

	if (stageTableId > 9) // goal stage
	{
		// TODO: allow swapping to 2SP goal-bunkis somehow?
		// TODO2: is there a generic goal-bunki we can use? I guess c2c missions probably use one for the non-goal stages?
		if (stageTableId == 10)
			tableEntry->BrInfoId_20 = STAGE_TULIP_GARDEN; // goal A
		else if (stageTableId == 11)
			tableEntry->BrInfoId_20 = STAGE_METROPOLIS; // goal B
		else if (stageTableId == 12)
			tableEntry->BrInfoId_20 = STAGE_ANCIENT_RUINS; // goal C
		else if (stageTableId == 13)
			tableEntry->BrInfoId_20 = STAGE_CAPE_WAY; // goal D
		else if (stageTableId == 14)
			tableEntry->BrInfoId_20 = STAGE_IMPERIAL_AVENUE; // goal E
	}

	tableEntry->CsInfoPtr_14 = cs_info_tbl[tableEntry->CsInfoId_1C];
	tableEntry->BrInfoPtr_18 = br_info_tbl[tableEntry->BrInfoId_20];

	sharecode_generate();

	return true;
}

void sharecode_generate()
{
	std::string code = "";
	for (int i = 0; i < 15; i++)
	{
		auto& stg = CustomStageTable[i];
		code += std::format("{:X}-", int(stg.StageUniqueName_0));
	}
	if (code.empty())
		return;

	code = code.substr(0, code.length() - 1);
	strcpy(ShareCode, code.c_str());
}

void sharecode_apply()
{
	std::string code = ShareCode;

	size_t start = 0;
	size_t end;

	int num = 0;
	do
	{
		if (num >= CustomStageTable.size())
			break;

		end = code.find('-', start);
		std::string part = code.substr(start, (end == std::string::npos) ? std::string::npos : end - start);

		int stageNum = 0;
		try
		{
			stageNum = std::stol(part, nullptr, 16);
		}
		catch (...)
		{
			stageNum = 0;
		}
		if (stageNum < 0 || stageNum >= 0x42)
			stageNum = 0;

		update_stage(&CustomStageTable[num], GameStage(stageNum), num);

		num++;

		start = end + 1;
	} while (end != std::string::npos);
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

		if (ImGui::Checkbox("Enable Course Override (use in C2C OutRun mode)", &Game::CourseReplacementEnabled))
			sharecode_generate();

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
	float comboWidth = 225.0f; // Width of comboboxes

	float windowHeight = ImGui::GetCursorPosY() + floor(comboHeight * 5 + (verticalSpacing * 6));

	bool has_updated = false;

	int num = 0;
	StageTable_mb* curStage = CustomStageTable.data();
	for (int col = 0; col < 5; ++col)
	{
		float columnHeight = floor((comboHeight + verticalSpacing) * (col + 1) - verticalSpacing);

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
				has_updated |= update_stage(curStage, GameStage(selected), num);

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

	ImGui::Text("Share Code: ");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(500.f);
	ImGui::InputText("##ShareCode", ShareCode, 256);
	ImGui::SameLine();
	if (ImGui::Button("Apply Code"))
		sharecode_apply();
	ImGui::SameLine();
	if (ImGui::Button("Copy Code"))
	{
		HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, strlen(ShareCode) + 1);
		if (hMem)
		{
			memcpy(GlobalLock(hMem), ShareCode, strlen(ShareCode) + 1);
			GlobalUnlock(hMem);
			OpenClipboard(0);
			EmptyClipboard();
			SetClipboardData(CF_TEXT, hMem);
			CloseClipboard();
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Paste Code"))
	{
		if (OpenClipboard(0))
		{
			HANDLE hClipboardData = GetClipboardData(CF_UNICODETEXT);
			if (hClipboardData == nullptr)
				hClipboardData = GetClipboardData(CF_TEXT);

			if (hClipboardData != nullptr)
			{
				void* clipboardData = GlobalLock(hClipboardData);
				if (clipboardData)
				{
					if (hClipboardData == GetClipboardData(CF_UNICODETEXT))
					{
						wchar_t* wideText = static_cast<wchar_t*>(clipboardData);

						// Convert unicode to char
						int bufferSize = WideCharToMultiByte(CP_UTF8, 0, wideText, -1, nullptr, 0, nullptr, nullptr);
						if (bufferSize > 0 && bufferSize < sizeof(ShareCode))
							WideCharToMultiByte(CP_UTF8, 0, wideText, -1, ShareCode, bufferSize, nullptr, nullptr);
						else
						{
							// Handle the case where the buffer is too small, truncate if necessary
							WideCharToMultiByte(CP_UTF8, 0, wideText, -1, ShareCode, sizeof(ShareCode) - 1, nullptr, nullptr);
							ShareCode[sizeof(ShareCode) - 1] = '\0'; // Null-terminate
						}
					}
					else if (hClipboardData == GetClipboardData(CF_TEXT))
					{
						char* asciiText = static_cast<char*>(clipboardData);

						if (strlen(asciiText) < sizeof(ShareCode))
							strcpy(ShareCode, asciiText);
						else
						{
							// If the clipboard text is too large, truncate
							strncpy(ShareCode, asciiText, sizeof(ShareCode) - 1);
							ShareCode[sizeof(ShareCode) - 1] = '\0'; // Null-terminate
						}
					}
					sharecode_apply();

					GlobalUnlock(hClipboardData);
				}
			}
			CloseClipboard();
		}
	}

	if (ImGui::TreeNode("Randomizer"))
	{
		static bool or2_day = true;
		static bool or2_night = true;
		static bool or2_reverse = true;
		static bool or2sp_day = true;
		static bool or2sp_night = true;
		static bool or2sp_reverse = true;
		static bool allow_duplicates = false;

		ImGui::Checkbox("OutRun2 Day (15 tracks)", &or2_day);
		ImGui::Checkbox("OutRun2 Night (2 tracks)", &or2_night);
		ImGui::Checkbox("OutRun2 Reverse (15 tracks)", &or2_reverse);
		ImGui::Checkbox("OutRun2SP Day (15 tracks)", &or2sp_day);
		ImGui::Checkbox("OutRun2SP Night (2 tracks)", &or2sp_night);
		ImGui::Checkbox("OutRun2SP Reverse (15 tracks)", &or2sp_reverse);
		ImGui::Checkbox("Allow Duplicates", &allow_duplicates);

		if (ImGui::Button("Randomize"))
		{
			std::vector<GameStage> chosen;
			if (or2_day)
				chosen.insert(chosen.end(), stages_or2_day.begin(), stages_or2_day.end());
			if (or2_night)
				chosen.insert(chosen.end(), stages_or2_night.begin(), stages_or2_night.end());
			if (or2_reverse)
				chosen.insert(chosen.end(), stages_or2_reverse.begin(), stages_or2_reverse.end());
			if (or2sp_day)
				chosen.insert(chosen.end(), stages_or2sp_day.begin(), stages_or2sp_day.end());
			if (or2sp_night)
				chosen.insert(chosen.end(), stages_or2sp_night.begin(), stages_or2sp_night.end());
			if (or2sp_reverse)
				chosen.insert(chosen.end(), stages_or2sp_reverse.begin(), stages_or2sp_reverse.end());

			// Randomize selection
			if (!chosen.empty())
			{
				std::random_device rd;
				std::mt19937 gen(rd());
				std::uniform_int_distribution<> dist(0, chosen.size() - 1);

				std::vector<GameStage> seen;
				for (int i = 0; i < 15; i++)
				{
					GameStage random_stage = chosen[dist(gen)];
					if (!allow_duplicates && chosen.size() >= 15)
					{
						while (std::find(seen.begin(), seen.end(), random_stage) != seen.end())
						{
							// Regenerate stage if it's been seen before
							random_stage = chosen[dist(gen)];
						}
					}

					update_stage(&CustomStageTable[i], random_stage, i);
					seen.push_back(random_stage);
				}
			}
		}
		ImGui::TreePop();
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
