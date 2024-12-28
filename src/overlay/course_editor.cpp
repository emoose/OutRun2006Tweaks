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
#include "overlay.hpp"

std::array<StageTable_mb, 0x40> CustomStageTable;
int CustomStageTableCount = 0;

constexpr uint8_t kLobbyFlag_UsingCustomCourse = 2;
constexpr char kLobbyCode_AsciiOffset = 0x30; // start at ascii '0', seems safest spot

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

void sharecode_generate();

bool update_stage(int stageTableId, GameStage newStage)
{
	auto* tableEntry = &CustomStageTable[stageTableId];
	tableEntry->StageUniqueName_0 = newStage;
	tableEntry->CsInfoId_1C = newStage;

	tableEntry->BrInfoId_20 = 0; // 0 allows highway to always spawn

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

	TAG_model_info** cs_info_tbl = Module::exe_ptr<TAG_model_info*>(0x2A54E0);
	TAG_model_info** br_info_tbl = Module::exe_ptr<TAG_model_info*>(0x2A55E8);

	tableEntry->CsInfoPtr_14 = cs_info_tbl[tableEntry->CsInfoId_1C];
	tableEntry->BrInfoPtr_18 = br_info_tbl[tableEntry->BrInfoId_20];

	sharecode_generate();

	return true;
}

bool lobbycode_generate()
{
	if (!*Game::SumoNet_CurNetDriver || !(*Game::SumoNet_CurNetDriver)->is_hosting())
		return false;

	if (!Overlay::CourseReplacementEnabled)
	{
		Game::SumoNet_LobbyInfo->unk_field_74_A &= ~kLobbyFlag_UsingCustomCourse;
		memset(Game::SumoNet_LobbyInfo->LobbyName, 0, 16);
		return false;
	}

	// Encode course list as ascii characters by adding kLobbyCode_AsciiOffset to them
	char lobbyNameEncoded[16] = { 0 };
	for (int i = 0; i < 15; i++)
		lobbyNameEncoded[i] = kLobbyCode_AsciiOffset + char(CustomStageTable[i].StageUniqueName_0);

	lobbyNameEncoded[15] = 0;

	memcpy(Game::SumoNet_LobbyInfo->LobbyName, lobbyNameEncoded, 16);

	// Hijack field_74 as indicator that course has been edited
	Game::SumoNet_LobbyInfo->unk_field_74_A = Game::SumoNet_LobbyInfo->unk_field_74_A | kLobbyFlag_UsingCustomCourse;

	return true;
}

bool lobbycode_apply(const char* lobbyName, bool& hasUpdated)
{
	hasUpdated = false;

	if (strlen(lobbyName) < 15)
		return false;

	for (int i = 0; i < 15; i++)
	{
		auto stage = GameStage(lobbyName[i] - kLobbyCode_AsciiOffset);
		if (CustomStageTable[i].StageUniqueName_0 != stage)
			hasUpdated = true;
		update_stage(i, stage);
	}

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
	strcpy(Overlay::CourseReplacementCode, code.c_str());
}

void sharecode_apply()
{
	std::string code = Overlay::CourseReplacementCode;

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

		update_stage(num, GameStage(stageNum));

		num++;

		start = end + 1;
	} while (end != std::string::npos);
}

class CourseReplacement : public Hook
{
	static bool UpdateCourseFromLobbyInfo()
	{
		static auto lastNotificationTime = std::chrono::steady_clock::now() - std::chrono::seconds(30);

		// if we're in lobby and not the host, try applying the course code
		if (*Game::SumoNet_CurNetDriver &&
			(*Game::SumoNet_CurNetDriver)->is_driver_valid() &&
			!(*Game::SumoNet_CurNetDriver)->is_hosting())
		{
			// Is field_74 set to our kLobbyFlag_UsingCustomCourse flag?
			if ((Game::SumoNet_LobbyInfo->unk_field_74_A & kLobbyFlag_UsingCustomCourse) == kLobbyFlag_UsingCustomCourse)
			{
				// Try parsing courses from lobby name
				bool hasUpdated = false;
				bool result = lobbycode_apply(Game::SumoNet_LobbyInfo->LobbyName, hasUpdated);

				if (hasUpdated)
				{
					// 15 sec cooldown between showing notification, in case host is actively tweaking stuff
					auto now = std::chrono::steady_clock::now();
					if (std::chrono::duration_cast<std::chrono::seconds>(now - lastNotificationTime).count() > 15)
					{
						Notifications::instance.add("Lobby is using custom course, changes applied.");
						lastNotificationTime = now;
					}
				}

				return result;
			}
		}
		return false;
	}

	inline static SafetyHookMid midhook{};
	static void dest(SafetyHookContext& ctx)
	{
		int* SumoScript_StageTableCount = Module::exe_ptr<int>(0x3D33C4);

		bool doCourseReplacement = Overlay::CourseReplacementEnabled;

		bool hasLobbyCourseReplacement = UpdateCourseFromLobbyInfo();
		if (hasLobbyCourseReplacement)
			doCourseReplacement = true;

		// Loaded stage table ptr is inside ctx.ebx
		// If replacement is disabled we'll copy the loaded one into our replacement table, so user has something to modify

		if (!doCourseReplacement || !CustomStageTableCount)
		{
			CustomStageTableCount = min(*SumoScript_StageTableCount, CustomStageTable.size());
			memcpy(CustomStageTable.data(), (void*)ctx.ebx, sizeof(StageTable_mb) * CustomStageTableCount);

			// Now stage table is inited, load in any saved code we have
			static bool loadedSavedCode = false;
			if (!loadedSavedCode && !Game::is_in_game())
			{
				if (strlen(Overlay::CourseReplacementCode))
					sharecode_apply();
				loadedSavedCode = true;
			}
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
		SumoLiveUpdate_Init_hook.call();

		int* SumoLiveUpdate_State = Module::exe_ptr<int>(0x436D14);
		if (Overlay::CourseReplacementEnabled)
			*SumoLiveUpdate_State = 3;
	}

	inline static SafetyHookInline SumoNet_RecvGameLobbyInfoEx_hook{};
	static void SumoNet_RecvGameLobbyInfoEx_dest(void* a1)
	{
		SumoNet_RecvGameLobbyInfoEx_hook.call(a1);
		UpdateCourseFromLobbyInfo();
	}

	inline static SafetyHookMid SumoNet_UpdateLobbyInfoFromUI_hook{};
	static void SumoNet_UpdateLobbyInfoFromUI_dest(SafetyHookContext& ctx)
	{
		// User has tried to update lobby settings (or game has decided to refresh them)
		// Make sure we overwrite lobby name if course replacement is enabled
		if (lobbycode_generate())
			Notifications::instance.add("Note: course replacement is enabled, lobby name is used to share with other players");
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
		SumoNet_RecvGameLobbyInfoEx_hook = safetyhook::create_inline(Module::exe_ptr(0x115410), SumoNet_RecvGameLobbyInfoEx_dest);
		SumoNet_UpdateLobbyInfoFromUI_hook = safetyhook::create_mid(Module::exe_ptr(0x91411), SumoNet_UpdateLobbyInfoFromUI_dest);
		return !!midhook;
	}

	static CourseReplacement instance;
};
CourseReplacement CourseReplacement::instance;

// Course editor window

class CourseEditor : public OverlayWindow
{
public:
	void init() override {}
	void render(bool overlayEnabled) override
	{
		if (!overlayEnabled)
			return;

		ImGui::Begin("Course Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

		if (!CustomStageTableCount)
		{
			ImGui::Text("Waiting for game to load stage scripts...");
			ImGui::End();
			return;
		}

		bool has_updated = false;

		bool is_in_lobby = (*Game::SumoNet_CurNetDriver && (*Game::SumoNet_CurNetDriver)->is_in_lobby());
		bool is_lobby_nonhost = is_in_lobby &&
			!(*Game::SumoNet_CurNetDriver)->is_hosting();
		bool is_lobby_host = is_in_lobby &&
			(*Game::SumoNet_CurNetDriver)->is_hosting();

		// Enable Course Override checkbox
		{
			bool disable_checkbox = Game::is_in_game() || is_lobby_nonhost;

			if (disable_checkbox)
				ImGui::BeginDisabled();

			bool replacementEnabled = Overlay::CourseReplacementEnabled;
			if (is_lobby_nonhost)
				replacementEnabled = (Game::SumoNet_LobbyInfo->unk_field_74_A & kLobbyFlag_UsingCustomCourse);

			if (ImGui::Checkbox("Enable Course Override (use in C2C OutRun mode)", &replacementEnabled))
			{
				Overlay::CourseReplacementEnabled = replacementEnabled;
				has_updated = true;
			}

			if (disable_checkbox)
				ImGui::EndDisabled();
		}

		bool editor_disabled = !Overlay::CourseReplacementEnabled ||
			(Game::is_in_game() && Game::pl_car()->is_in_bunki()) ||
			(is_lobby_nonhost || (is_lobby_host && Game::is_in_game()));

		if (ImGui::TreeNodeEx("Stages", ImGuiTreeNodeFlags_DefaultOpen))
		{
			StageTable_mb* stg_tbl = *Module::exe_ptr<StageTable_mb*>(0x3D3188); // stg_tbl only points to current stage

			int curPlayingIndex = -1;
			if (stg_tbl)
				curPlayingIndex = stg_tbl->StageTableIdx_4;

			if (editor_disabled)
				ImGui::BeginDisabled();

			float comboHeight = ImGui::GetFrameHeight(); // Height of a single combobox
			float verticalSpacing = 10.0f; // Additional spacing between comboboxes
			float comboWidth = 200.f; // Width of comboboxes

			float windowHeight = ImGui::GetCursorPosY() + (comboHeight * 3) + floor(comboHeight * 5 + (verticalSpacing * 6));

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

				// if player is already past this track (or playing it), disable dropdowns
				bool col_disabled = (curPlayingIndex >= num) && !editor_disabled && Game::is_in_game();

				for (int row = 0; row <= col; ++row)
				{
					if (col_disabled)
						ImGui::BeginDisabled();

					bool playingThisStage = (curPlayingIndex == num) && Game::is_in_game();

					// Highlight the current track
					if (playingThisStage)
						ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.75f, 0.0f, 0.75f));

					ImGui::PushID(col * 10 + row);
					ImGui::SetNextItemWidth(comboWidth);

					int selected = curStage->StageUniqueName_0;

					std::string name = std::format("##{}.{}", (col + 1), (row + 1));
					if (ImGui::Combo(name.c_str(), &selected, Game::StageNames, int(STAGE_COUNT)))
						has_updated |= update_stage(num, GameStage(selected));

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

			char* ShareCode = Overlay::CourseReplacementCode;

			ImGui::Text("Share Code: ");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(500.f);
			ImGui::InputText("##ShareCode", ShareCode, 256);
			ImGui::SameLine();
			if (ImGui::Button("Apply Code"))
			{
				has_updated = true;
				sharecode_apply();
			}
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

			if (editor_disabled)
				ImGui::EndDisabled();

			ImGui::TreePop();
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

			if (editor_disabled)
				ImGui::BeginDisabled();

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

						update_stage(i, random_stage);
						seen.push_back(random_stage);
					}
					has_updated = true;
				}
			}

			if (editor_disabled)
				ImGui::EndDisabled();

			ImGui::TreePop();
		}

		if (editor_disabled)
		{
			if (is_lobby_nonhost)
				ImGui::Text("(editor is only available for host)");
			else if (is_lobby_host && Game::is_in_game())
				ImGui::Text("(editor is unavailable during online game)");
			else if (Overlay::CourseReplacementEnabled && Game::is_in_game() && Game::pl_car()->is_in_bunki())
				ImGui::Text("(editor is unavailable during bunki)");
		}

		ImGui::End();

		if (has_updated)
		{
			Overlay::settings_write();
			sharecode_generate();
			lobbycode_generate();
		}
	}
	static CourseEditor instance;
};
CourseEditor CourseEditor::instance;
