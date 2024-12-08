#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <array>
#include <bitset>
#include <imgui.h>
#include <ini.h>

std::array<std::vector<uint16_t>, 256> ObjectNodes;
std::array<std::array<std::bitset<16384>, 256>, 128> ObjectExclusionsPerStage;
int NumObjects = 0;
int CsLengthNum = 0;

bool DrawDistanceIncreaseEnabled = false;
bool EnablePauseMenu = true;

bool DrawDist_ReadExclusions();

void DrawDist_DrawOverlay()
{
	ImGui::Begin("Draw Distance Debugger");

	ImGui::Checkbox("Countdown timer enabled", Game::Sumo_CountdownTimerEnable);
	ImGui::Checkbox("Pause menu enabled", &EnablePauseMenu);

	// get max column count
	int num_columns = 0;
	for (int i = 0; i < NumObjects; i++)
	{
		size_t size = ObjectNodes[i].size();
		if (size > num_columns)
			num_columns = size;
	}

	static ImGuiTableFlags table_flags = ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_HighlightHoveredColumn;

	ImGui::Text("Usage:");
	ImGui::Text("- If an ugly LOD object appears, pause game with ESC and press F11 to bring up this window");
	ImGui::Text("- Reduce Draw Distance below to lowest value that still has the LOD object appearing");
	ImGui::Text("- Once you find the draw-distance that shows the object, click each node checkbox to disable nodes");
	ImGui::Text("- After finding the node responsible, you can use \"Copy to clipboard\" below to copy the IDs of them, or hover over the node");
	ImGui::Text("- Post the IDs for LODs you find in the \"DrawDistanceIncrease issue reports\" github thread and we can add exclusions for them!");
	ImGui::NewLine();

	if (!DrawDistanceIncreaseEnabled)
	{
		ImGui::Text("Error: DrawDistanceIncrease must be enabled in INI before launching.");
		ImGui::End();
		return;
	}

	if (*Game::current_mode != GameState::STATE_GAME && *Game::current_mode != GameState::STATE_SMPAUSEMENU)
	{
		ImGui::Text("Nodes will be displayed once in-game.");
		ImGui::End();
		return;
	}

	int cur_stage_num = *Game::stg_stage_num;
	const char* cur_stage_name = Game::GetStageUniqueName(cur_stage_num);
	auto& objectExclusions = ObjectExclusionsPerStage[cur_stage_num];

	ImGui::Text("Stage: %d (%s)", cur_stage_num, cur_stage_name);
	ImGui::SliderInt("Draw Distance", &Settings::DrawDistanceIncrease, 0, 1024);

	if (ImGui::Button("<<<"))
		Settings::DrawDistanceIncrease -= 10;
	ImGui::SameLine();
	if (ImGui::Button("<<"))
		Settings::DrawDistanceIncrease -= 5;
	ImGui::SameLine();
	if (ImGui::Button("<"))
		Settings::DrawDistanceIncrease -= 1;
	ImGui::SameLine();
	if (ImGui::Button(">"))
		Settings::DrawDistanceIncrease += 1;
	ImGui::SameLine();
	if (ImGui::Button(">>"))
		Settings::DrawDistanceIncrease += 5;
	ImGui::SameLine();
	if (ImGui::Button(">>>"))
		Settings::DrawDistanceIncrease += 10;

	ImGui::Text("Nodes at distance +%d (track section #%d):", Settings::DrawDistanceIncrease, (CsLengthNum + Settings::DrawDistanceIncrease));

	if (num_columns > 0)
	{
		num_columns += 1;
		if (ImGui::BeginTable("NodeTable", num_columns, table_flags))
		{
			ImGui::TableSetupColumn("Object ID", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder);
			for (int n = 1; n < num_columns; n++)
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);

			for (int objectIdx = 0; objectIdx < NumObjects; objectIdx++)
			{
				ImGui::PushID(objectIdx);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				if (ImGui::Button(std::format("Object {}", objectIdx).c_str()))
				{
					bool areAllExcluded = true;
					for (int i = 0; i < ObjectNodes[objectIdx].size(); i++)
					{
						auto nodeId = ObjectNodes[objectIdx][i];
						if (!objectExclusions[objectIdx][nodeId])
						{
							areAllExcluded = false;
							break;
						}
					}

					for (int i = 0; i < ObjectNodes[objectIdx].size(); i++)
					{
						auto nodeId = ObjectNodes[objectIdx][i];
						objectExclusions[objectIdx][nodeId] = areAllExcluded ? false : true;
					}
				}

				for (int i = 0; i < ObjectNodes[objectIdx].size(); i++)
					if (ImGui::TableSetColumnIndex(i + 1))
					{
						ImGui::PushID(i + 1);

						auto nodeId = ObjectNodes[objectIdx][i];
						bool excluded = objectExclusions[objectIdx][nodeId];

						if (ImGui::Checkbox("", &excluded))
							objectExclusions[objectIdx][nodeId] = excluded;

						ImGui::SetItemTooltip("Stage %d, object 0x%X, node 0x%X", cur_stage_num, objectIdx, nodeId);

						ImGui::PopID();
					}

				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	if (ImGui::Button("Copy exclusions to clipboard"))
	{
		std::string clipboard = "";// 
		for (int objId = 0; objId < objectExclusions.size(); objId++)
		{
			std::string objLine = "";
			for (int i = 0; i < objectExclusions[objId].size(); i++)
			{
				if (objectExclusions[objId][i])
				{
					objLine += std::format(", 0x{:X}", i);
				}
			}

			if (objLine.length() > 2)
			{
				clipboard += std::format("\n0x{:X} = {}", objId, objLine.substr(2));
			}
		}
		if (!clipboard.empty())
			clipboard = std::format("# {}\n[Stage {}]{}", cur_stage_name, cur_stage_num, clipboard);

		HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, clipboard.length() + 1);
		if (hMem)
		{
			memcpy(GlobalLock(hMem), clipboard.c_str(), clipboard.length() + 1);
			GlobalUnlock(hMem);
			OpenClipboard(0);
			EmptyClipboard();
			SetClipboardData(CF_TEXT, hMem);
			CloseClipboard();
		}
	}

	static bool showReallyPrompt = false;
	if (ImGui::Button(showReallyPrompt ? "Are you sure?##clear" : "Clear all exclusions"))
	{
		if (!showReallyPrompt)
		{
			showReallyPrompt = true;
		}
		else
		{
			for (int i = 0; i < objectExclusions.size(); i++)
				objectExclusions[i].reset();
			showReallyPrompt = false;
		}
	}

	static bool showReallyPrompt2 = false;
	if (ImGui::Button(showReallyPrompt2 ? "Are you sure?##reload" : "Reload exclusions from INI"))
	{
		if (!showReallyPrompt2)
		{
			showReallyPrompt2 = true;
		}
		else
		{
			DrawDist_ReadExclusions();
			showReallyPrompt2 = false;
		}
	}

	ImGui::End();
}


static int get_number(std::string_view sectionName)
{
	int number = -1;

	// Find the position of the numeric part
	auto pos = sectionName.find_first_of("0123456789");
	if (pos != std::string::npos)
	{
		auto result = std::from_chars(sectionName.data() + pos, sectionName.data() + sectionName.size(), number);
		if (result.ec == std::errc())
			return number;
	}

	return -1;
}

bool DrawDist_ReadExclusions()
{
	for (int x = 0; x < ObjectExclusionsPerStage.size(); x++)
		for (int y = 0; y < ObjectExclusionsPerStage[x].size(); y++)
			ObjectExclusionsPerStage[x][y].reset();

	// Try reading exclusions
	std::filesystem::path& iniPath = Module::LodIniPath;
	if (!std::filesystem::exists(iniPath))
	{
		spdlog::error("DrawDist_ReadExclusions - failed to locate exclusion INI from path {}", iniPath.string());
		return false;
	}

	spdlog::info("DrawDist_ReadExclusions - reading INI from {}", iniPath.string());

	const std::wstring iniPathStr = iniPath.wstring();

	// Read INI via FILE* since INIReader doesn't support wstring
	FILE* iniFile;
	errno_t result = _wfopen_s(&iniFile, iniPathStr.c_str(), L"r");
	if (result != 0 || !iniFile)
	{
		spdlog::error("DrawDist_ReadExclusions - INI read failed! Error code {}", result);
		return false;
	}

	inih::INIReader ini;
	try
	{
		ini = inih::INIReader(iniFile);
	}
	catch (...)
	{
		spdlog::error("DrawDist_ReadExclusions - INI read failed! The file may be invalid or have duplicate settings inside");
		fclose(iniFile);
		return false;
	}
	fclose(iniFile);

	for (auto& section : ini.Sections())
	{
		int stageNum = get_number(section);
		if (stageNum >= ObjectExclusionsPerStage.size())
		{
			spdlog::error("DrawDist_ReadExclusions - INI contains invalid stage section \"{}\", skipping...", section);
			continue;
		}

		for (auto& key : ini.Keys(section))
		{
			int objectId = -1;
			try
			{
				objectId = std::stol(key, nullptr, 0);
			}
			catch (const std::invalid_argument& e)
			{
				continue;
			}
			catch (const std::out_of_range& e)
			{
				continue;
			}

			if (objectId < 0)
				continue;

			if (objectId >= ObjectExclusionsPerStage[stageNum].size())
			{
				spdlog::error("DrawDist_ReadExclusions - INI contains invalid object number \"{}\", skipping...", key);
				continue;
			}

			std::vector<int> nodes;

			auto value = ini.Get(section, key);
			std::istringstream stream(value);
			std::string token;

			// Tokenize the string using ',' as the delimiter
			while (std::getline(stream, token, ','))
			{
				// Remove leading/trailing whitespace
				token.erase(0, token.find_first_not_of(" \t"));
				token.erase(token.find_last_not_of(" \t") + 1);

				// Convert the token to an integer
				try
				{
					nodes.push_back(std::stol(token, nullptr, 0));
				}
				catch (const std::invalid_argument& e)
				{
				}
				catch (const std::out_of_range& e)
				{
				}
			}

			for (auto& node : nodes)
			{
				ObjectExclusionsPerStage[stageNum][objectId][node] = true;
			}
		}
	}

	return true;
}

class DrawDistanceIncrease : public Hook
{
	// Stage drawing/culling is based on the player cars position in the stage
	// Each ~1m of track has an ID, which indexes data inside the cs_CS_[stg]_bin.sz file
	// This data contains a list of CullingNode indexes pointing to matrices and model pointers for drawing
	//
	// The first attempt to increase draw distance looped the existing draw code, incrementing the track ID up to a specified max-distance-increase variable
	// This seemed effective but would cause some darker stage shadowing and duplicate sprites to show
	// Appears that some of the track-ID lists shared CullingNode IDs inside them, leading to models being drawn multiple times
	//
	// After learning more about the CullingNode setup, a second attempt instead tried to loop over each CullingNode list and add unique entries to a list
	// (via a statically allocated unique-CullingNode-ID array and std::bitset to track seen IDs)
	// With this the distance could be increased without the duplicate model issues, and without affecting performance too heavily
	//
	// ---
	// 
	// There are a couple issues with this setup right now though, fortunately they only really appear when distance is increased quite far (>64):
	// 
	// - Some CullingNode lists meant for later in the stage may contain LOD models for earlier parts
	//   eg. Palm Beach has turns later on where LOD models for the beginning section appear (sometimes overwriting the non-LOD models somehow?)
	//   Some kind of filter-list that defines those LOD models and only allows the default csOffset = 0 to draw them might help this
	//
	// - Maps like Canyon with vertical progression can show higher-up parts of the track early, which vanilla game wouldn't display until they were close by.
	//   With increased draw distance, those sections then appear in the sky without any connecting textures...
	//   May need to add per-stage / per-section distance limits to workaround this
	//
	// - Increasing DrawDistanceBehind and using a camera mod to view behind the car reveals backface-culling issues, since those faces were never visible in the vanilla game.
	//   Wonder how the attract videos for the game were able to use other camera angles without any backface-culling showing up...
	//   Those videos seem to be captured in-engine, were they using special versions of the map that included all faces?
	//   (or possibly all the faces are already included in the current maps, and it's something CullingNode-related which skips drawing them?
	//    I'm not hopeful about that though, doubt they would have included data for parts that wouldn't be shown)

	// Known bad CullingNode IDs:
	//  Palm Beach
	//   (obj4,node0xF1) = breaks railings at the beginning
	//  Metropolis
	//   ??? = breaks trees

	inline static uint16_t CollisionNodeIdxArray[4096];
	inline static std::array<uint8_t, 4096> CollisionNodesToDisplay;

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		int xmtSetShifted = *(int*)(ctx.esp + 0x14); // XMTSET num shifted left by 16
		uint8_t* a2 = *(uint8_t**)(ctx.esp + 0x24);
		int a4 = *(int*)(a2 + 0x70);
		int a5 = ctx.edx;

		int CsMaxLength = Game::GetMaxCsLen(0);
		CsLengthNum = ctx.ebp;

		int v6 = ctx.ebx;
		uint32_t* v11 = (uint32_t*)(v6 + 8);

		auto& objectExclusions = ObjectExclusionsPerStage[*Game::stg_stage_num];

		NumObjects = *(int*)(ctx.esp + 0x18);
		for (int ObjectNum = 0; ObjectNum < NumObjects; ObjectNum++)
		{
			memset(CollisionNodesToDisplay.data(), 0, CollisionNodesToDisplay.size());
			uint16_t* cur = CollisionNodeIdxArray;

			for (int csOffset = -Settings::DrawDistanceBehind; csOffset < (Settings::DrawDistanceIncrease + 1); csOffset++)
			{
				if (csOffset != 0)
				{
					// If current offset is below idx 0 skip to next one
					if (CsLengthNum + csOffset < 0)
						continue;

					// If we're past the max entries for the stage then break out
					if (CsLengthNum + csOffset >= (CsMaxLength - 1))
						break;
				}

				// DEBUG: clear lastadds for this objectnum here
				if (Settings::OverlayEnabled && csOffset == Settings::DrawDistanceIncrease)
				{
					ObjectNodes[ObjectNum].clear();
				}

				uint32_t sectionCollListOffset = *(uint32_t*)(v6 + *v11 + ((CsLengthNum + csOffset) * 4));
				uint16_t* sectionCollList = (uint16_t*)(v6 + *v11 + sectionCollListOffset);

				int num = 0;
				while (*sectionCollList != 0xFFFF)
				{
					// If we haven't seen this CollisionNode idx already lets add it to our IdxArray
					if (!CollisionNodesToDisplay[*sectionCollList])
					{
						CollisionNodesToDisplay[*sectionCollList] = true;

						// DEBUG: check exclusions here before adding to *cur
						// (if we're at csOffset = 0 exclusions are ignored, since this is what vanilla game would display)
						if (csOffset == 0 || !objectExclusions[ObjectNum][*sectionCollList])
						{
							*cur = *sectionCollList;
							cur++;
						}

						// DEBUG: add *sectionCollList to lastadds list here
						if (Settings::OverlayEnabled && csOffset == Settings::DrawDistanceIncrease)
							ObjectNodes[ObjectNum].push_back(*sectionCollList);

						num++;
					}

					sectionCollList++;
				}
			}

			*cur = 0xFFFF;

			Game::DrawObject_Internal(xmtSetShifted | ObjectNum, 0, CollisionNodeIdxArray, a4, a5, 0);

			v11++;
		}
	}

public:
	std::string_view description() override
	{
		return "DrawDistanceIncrease";
	}

	bool validate() override
	{
		return Settings::DrawDistanceIncrease > 0 || Settings::DrawDistanceBehind > 0;
	}

	bool apply() override
	{
		constexpr int DispStage_HookAddr = 0x4DF6D;

		Memory::VP::Nop(Module::exe_ptr(DispStage_HookAddr), 0x4B);
		dest_hook = safetyhook::create_mid(Module::exe_ptr(DispStage_HookAddr), destination);

		for (int i = 0; i < ObjectNodes.size(); i++)
			ObjectNodes[i].reserve(4096);

		DrawDistanceIncreaseEnabled = true;

		DrawDist_ReadExclusions();

		return true;
	}

	static DrawDistanceIncrease instance;
};
DrawDistanceIncrease DrawDistanceIncrease::instance;

// TODO: move this to game.h, should probably remove the resize function
struct TDrawBuffer
{
	int NumBuffers_0;
	int MaxBuffers_4;
	int unk_8[1];
	int MaxBuffers_C;
	struct TDrawEntry** BufferPtrs_10;
	struct TDrawEntry* Buffer_14;
	void* UnkBuffer_18;

	void resize(int new_size)
	{
		int bufferPtrsSize_Imm = new_size * 4;
		int buffersSize_Imm = new_size * 0x3C;
		int unkbuffersSize_Imm = new_size * 0x40;

		MaxBuffers_4 = new_size;
		MaxBuffers_C = new_size;

		BufferPtrs_10 = (TDrawEntry**)malloc(bufferPtrsSize_Imm);
		Buffer_14 = (TDrawEntry*)malloc(buffersSize_Imm);
		UnkBuffer_18 = malloc(unkbuffersSize_Imm);
	}
};

class DrawBufferExtension : public Hook
{
	inline static SafetyHookInline drawbufferinit_hook = {};
	static void drawbufferinit_dest()
	{
		drawbufferinit_hook.call();

		TDrawBuffer* s_ImmDrawBuffer = Module::exe_ptr<TDrawBuffer>(0x00464EF8);
		TDrawBuffer* s_AftDrawBuffer = Module::exe_ptr<TDrawBuffer>(0x004612D8);

		constexpr int s_ImmDrawBufferSizeVanilla = 0x100;
		constexpr int s_AftDrawBufferSizeVanilla = 0x600;

		s_ImmDrawBuffer->resize(s_ImmDrawBufferSizeVanilla * 0x10);
		s_AftDrawBuffer->resize(s_AftDrawBufferSizeVanilla * 2);
	}

public:
	std::string_view description() override
	{
		return "DrawBufferExtension";
	}

	bool validate() override
	{
		return Settings::DrawDistanceIncrease > 0 || Settings::DrawDistanceBehind > 0;
	}

	bool apply() override
	{
		drawbufferinit_hook = safetyhook::create_inline(Module::exe_ptr(0x5160), drawbufferinit_dest);
		return true;
	}

	static DrawBufferExtension instance;
};
DrawBufferExtension DrawBufferExtension::instance;

class PauseMenuVisibility : public Hook
{
	inline static SafetyHookInline sprani_hook = {};
	static void sprani_dest()
	{
		if (EnablePauseMenu)
			sprani_hook.call();
	}

	inline static SafetyHookInline pauseframedisp_hook = {};
	static void __fastcall pauseframedisp_dest(void* thisptr, void* unused)
	{
		if (EnablePauseMenu)
			pauseframedisp_hook.thiscall(thisptr);
	}

public:
	std::string_view description() override
	{
		return "PauseMenuVisibility";
	}

	bool validate() override
	{
		return Settings::OverlayEnabled;
	}

	bool apply() override
	{
		sprani_hook = safetyhook::create_inline(Module::exe_ptr(0x28170), sprani_dest);
		pauseframedisp_hook = safetyhook::create_inline(Module::exe_ptr(0x8C5F0), pauseframedisp_dest);
		return true;
	}

	static PauseMenuVisibility instance;
};
PauseMenuVisibility PauseMenuVisibility::instance;
