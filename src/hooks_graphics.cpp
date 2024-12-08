#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <array>
#include <bitset>
#include <iostream>
#include <imgui.h>

std::array<std::vector<uint16_t>, 256> ObjectNodes;
std::array<std::bitset<16384>, 256> ObjectExclusions;
int ExclusionsStageNum = 0; // stageid the exclusions are setup for, if stageid doesn't match current exclusions will be cleared
int NumObjects = 0;

bool DrawDistanceIncreaseEnabled = false;

void Overlay_DrawDistOverlay()
{
	uint32_t cur_stage_num = Game::GetStageUniqueNum(Game::GetNowStageNum(8));

	ImGui::Begin("Draw Distance Debugger");

	ImGui::Checkbox("Countdown timer enabled", Game::Sumo_CountdownTimerEnable);

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
	ImGui::Text("- When an ugly LOD object appears, pause the game with ESC and press F11 to bring up this window");
	ImGui::Text("- Reduce the Draw Distance below to the lowest value which still has the LOD object appearing");
	ImGui::Text("- Once you find the draw-distance that shows the object, click each node checkbox to disable nodes");
	ImGui::Text("- After you find the node responsible, you can use \"Copy to clipboard\" below to copy the IDs of them, or hover over the node");
	ImGui::Text("- Post the IDs for LODs you find in the \"DrawDistanceIncrease issue reports\" github thread and we can add exclusions for them!");
	ImGui::NewLine();

	if (!DrawDistanceIncreaseEnabled)
	{
		ImGui::Text("Error: DrawDistanceIncrease must be enabled in INI before launching.");
		ImGui::End();
		return;
	}

	ImGui::SliderInt("Draw Distance", &Settings::DrawDistanceIncrease, 0, 1024);
	if (ImGui::Button("<<<"))
		Settings::DrawDistanceIncrease--;
	ImGui::SameLine();
	if (ImGui::Button(">>>"))
		Settings::DrawDistanceIncrease++;

	if (num_columns > 0)
	{
		ImGui::Text("Nodes at DrawDistance %d:", Settings::DrawDistanceIncrease);

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
						if (!ObjectExclusions[objectIdx][nodeId])
						{
							areAllExcluded = false;
							break;
						}
					}

					for (int i = 0; i < ObjectNodes[objectIdx].size(); i++)
					{
						auto nodeId = ObjectNodes[objectIdx][i];
						ObjectExclusions[objectIdx][nodeId] = areAllExcluded ? false : true;
					}
				}

				for (int i = 0; i < ObjectNodes[objectIdx].size(); i++)
					if (ImGui::TableSetColumnIndex(i + 1))
					{
						ImGui::PushID(i + 1);

						auto nodeId = ObjectNodes[objectIdx][i];
						bool excluded = ObjectExclusions[objectIdx][nodeId];

						if (ImGui::Checkbox("", &excluded))
							ObjectExclusions[objectIdx][nodeId] = excluded;

						ImGui::SetItemTooltip("Stage %d, object 0x%X, node 0x%X", cur_stage_num, objectIdx, nodeId);

						ImGui::PopID();
					}

				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	if (ImGui::Button("Clear all exclusions"))
	{
		for (int i = 0; i < ObjectExclusions.size(); i++)
			ObjectExclusions[i].reset();
	}
	if (ImGui::Button("Copy exclusions to clipboard"))
	{
		std::string clipboard = "";// 
		for (int objId = 0; objId < ObjectExclusions.size(); objId++)
		{
			std::string objLine = "";
			for (int i = 0; i < ObjectExclusions[objId].size(); i++)
			{
				if (ObjectExclusions[objId][i])
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
			clipboard = std::format("[Stage {}]{}", cur_stage_num, clipboard);

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

	ImGui::End();
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
	inline static std::bitset<4096> CollisionNodesToDisplay;

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		int xmtSetShifted = *(int*)(ctx.esp + 0x14); // XMTSET num shifted left by 16
		uint8_t* a2 = *(uint8_t**)(ctx.esp + 0x24);
		int a4 = *(int*)(a2 + 0x70);
		int a5 = ctx.edx;

		int CsMaxLength = Game::GetMaxCsLen(0);
		int CsLengthNum = ctx.ebp;

		int v6 = ctx.ebx;
		uint32_t* v11 = (uint32_t*)(v6 + 8);

		uint32_t cur_stage_num = Game::GetStageUniqueNum(Game::GetNowStageNum(8));
		if (Settings::OverlayEnabled && ExclusionsStageNum != cur_stage_num)
		{
			for (int i = 0; i < ObjectExclusions.size(); i++)
				ObjectExclusions[i].reset();
			ExclusionsStageNum = cur_stage_num;
		}

		NumObjects = *(int*)(ctx.esp + 0x18);
		for (int ObjectNum = 0; ObjectNum < NumObjects; ObjectNum++)
		{
			CollisionNodesToDisplay.reset();
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
						// (if we're at csOffset = 0, exclusions are ignored, since that is what vanilla game would display)
						if (!Settings::OverlayEnabled || !ObjectExclusions[ObjectNum][*sectionCollList] || csOffset == 0)
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

		return true;
	}

	static DrawDistanceIncrease instance;
};
DrawDistanceIncrease DrawDistanceIncrease::instance;

class UseHiDefCharacters : public Hook
{
	inline static const char ChrDrGh00_path[] = "Media\\CHR_DR_GH00.bin";
	inline static const char ChrDrGh00_gamepath[] = "\\Media\\CHR_DR_GH00.bin";
	inline static const char ChrDrGh00Usa_path[] = "Media\\CHR_DR_GH00_USA.bin";
	inline static const char ChrDrGh00Usa_gamepath[] = "\\Media\\CHR_DR_GH00_USA.bin";

public:
	std::string_view description() override
	{
		return "UseHiDefCharacters";
	}

	bool validate() override
	{
		return Settings::UseHiDefCharacters;
	}

	bool apply() override
	{
		// Switch Chr\CHR_DR_GH00*.bin usages to read from Media\CHR_DR_GH00*.bin instead, if they exist
		// (Chr\ versions are missing hair anims which Media\ versions fortunately include - Media\ versions are otherwise unused)
		{
			if (std::filesystem::exists(ChrDrGh00_path))
				Game::chrset_info[ChrSet::CHR_DR_GH00].bin_ptr = ChrDrGh00_gamepath;
			if (std::filesystem::exists(ChrDrGh00Usa_path))
				Game::chrset_info[ChrSet::CHR_DR_GH00_USA].bin_ptr = ChrDrGh00Usa_gamepath;
		}

		int* driver_chrsets = Module::exe_ptr<int>(0x2549B0);
		int* heroine_chrsets = Module::exe_ptr<int>(0x2549C8);

		// Switch Alberto CHR_DR_M00 -> CHR_DR_MH00
		driver_chrsets[0] = ChrSet::CHR_DR_MH00;
		Memory::VP::Patch(Module::exe_ptr(0x87F41 + 1), { uint8_t(ChrSet::CHR_DR_MH00) }); // O2SP

		// Switch Jennifer CHR_DR_L00 -> CHR_DR_LH00
		heroine_chrsets[3] = ChrSet::CHR_DR_LH00;
		Memory::VP::Patch(Module::exe_ptr(0x8803A + 1), { uint8_t(ChrSet::CHR_DR_LH00) }); // O2SP

		// Switch Clarissa CHR_DR_G00_* -> CHR_DR_GH00_*
		heroine_chrsets[4] = ChrSet::CHR_DR_GH00; // (game code handles switching heroine_chrsets to USA variant, so we don't need to check RestoreJPClarissa here)

		// Clarissa O2SP code (this is also patched by RestoreJPClarissa, so make sure both set it to same value...)
		Memory::VP::Patch(Module::exe_ptr(0x88044 + 1), { Settings::RestoreJPClarissa ? uint8_t(ChrSet::CHR_DR_GH00) : uint8_t(ChrSet::CHR_DR_GH00_USA) });

		return true;
	}

	static UseHiDefCharacters instance;
};
UseHiDefCharacters UseHiDefCharacters::instance;

class RestoreCarBaseShadow : public Hook
{
	static void __cdecl CalcPeraShadow(int a1, int a2, int a3, float a4)
	{
		// CalcPeraShadow code from C2C Xbox
		EVWORK_CAR* car = Game::event(8)->data<EVWORK_CAR>();

		Game::mxPushLoadMatrix(&car->matrix_B0);
		Game::mxTranslate(0.0f, 0.05f, 0.0f);

		// Xbox C2C would multiply a4 by 0.5, halving the opacity, which on PC made it almost invisible..
		Game::DrawObjectAlpha_Internal(a1, a4 * Settings::CarBaseShadowOpacity, 0, -1);
		Game::mxPopMatrix();
	}

public:
	std::string_view description() override
	{
		return "RestoreCarBaseShadow";
	}

	bool validate() override
	{
		return Settings::CarBaseShadowOpacity > 0;
	}

	bool apply() override
	{
		constexpr int DispCarModel_Common_HookAddr = 0x69EB4;
		constexpr int DispSelCarModel_HookAddr = 0x6AC76; // O2SP car selection
		constexpr int Sumo_DispSelCarModel_HookAddr = 0x6B766; // C2C car selection

		// These three funcs contain nullsub_1 calls which were CalcPeraShadow calls in O2SP / C2CXbox
		// We can't just hook nullsub_1 since a bunch of other nulled out code also calls it, instead we'll rewrite them to call our CalcPeraShadow func
		Memory::VP::InjectHook(Module::exe_ptr(DispCarModel_Common_HookAddr), CalcPeraShadow, Memory::HookType::Call);
		Memory::VP::InjectHook(Module::exe_ptr(DispSelCarModel_HookAddr), CalcPeraShadow, Memory::HookType::Call);
		Memory::VP::InjectHook(Module::exe_ptr(Sumo_DispSelCarModel_HookAddr), CalcPeraShadow, Memory::HookType::Call);

		return true;
	}

	static RestoreCarBaseShadow instance;
};
RestoreCarBaseShadow RestoreCarBaseShadow::instance;

class ReflectionResolution : public Hook
{
	inline static std::array<int, 6> ReflectionResolution_Addrs = 
	{
		// Envmap_Init
		0x13B50 + 1,
		0x13BA1 + 1,
		0x13BA6 + 1,
		// D3D_CreateTemporaries
		0x17A69 + 1,
		0x17A88 + 1,
		0x17A8D + 1,
	};

public:
	std::string_view description() override
	{
		return "ReflectionResolution";
	}

	bool validate() override
	{
		return Settings::ReflectionResolution >= 2;
	}

	bool apply() override
	{
		for (const int& addr : ReflectionResolution_Addrs)
		{
			Memory::VP::Patch(Module::exe_ptr<int>(addr), Settings::ReflectionResolution);
		}
		return true;
	}

	static ReflectionResolution instance;
};
ReflectionResolution ReflectionResolution::instance;

class DisableDPIScaling : public Hook
{
public:
	std::string_view description() override
	{
		return "DisableDPIScaling";
	}

	bool validate() override
	{
		return Settings::DisableDPIScaling;
	}

	bool apply() override
	{
		SetProcessDPIAware();
		return true;
	}

	static DisableDPIScaling instance;
};
DisableDPIScaling DisableDPIScaling::instance;

class ScreenEdgeCullFix : public Hook
{
	const static int CalcBall3D2D_Addr = 0x49E70;

	// Hook CalcBall3D2D to rescale screen-ratio positions back to 4:3 positions that game code expects
	// (fixes objects like cones being culled out before they reach edge of the screen)
	inline static SafetyHookInline dest_orig = {};
	static float __cdecl destination(float a1, Sphere* a2, Sphere* a3)
	{
		float ret = dest_orig.ccall<float>(a1, a2, a3);

		constexpr float ratio_4_3 = 4.f / 3.f;

		float ratio_screen = Game::screen_resolution->x / Game::screen_resolution->y;

		a3->f0 = (a3->f0 / ratio_screen) * ratio_4_3;
		a3->f1 = (a3->f1 * ratio_screen) / ratio_4_3;
		return ret;
	}

public:
	std::string_view description() override
	{
		return "ScreenEdgeCullFix";
	}

	bool validate() override
	{
		return Settings::ScreenEdgeCullFix;
	}

	bool apply() override
	{
		dest_orig = safetyhook::create_inline(Module::exe_ptr(CalcBall3D2D_Addr), destination);
		return !!dest_orig;
	}

	static ScreenEdgeCullFix instance;
};
ScreenEdgeCullFix ScreenEdgeCullFix::instance;

class DisableStageCulling : public Hook
{
	const static int CalcCulling_PatchAddr = 0x501F;

public:
	std::string_view description() override
	{
		return "DisableStageCulling";
	}

	bool validate() override
	{
		return Settings::DisableStageCulling;
	}

	bool apply() override
	{
		// Patch "if (CheckCulling(...))" -> no-op
		Memory::VP::Patch(Module::exe_ptr(CalcCulling_PatchAddr), { 0x90, 0x90 });
		return true;
	}

	static DisableStageCulling instance;
};
DisableStageCulling DisableStageCulling::instance;

class DisableVehicleLODs : public Hook
{
	const static int DispOthcar_PatchAddr = 0xAE4E9;
public:
	std::string_view description() override
	{
		return "DisableVehicleLODs";
	}

	bool validate() override
	{
		return Settings::DisableVehicleLODs;
	}

	bool apply() override
	{
		// Patch "eax = car.LodNumber" -> "eax = 0"
		Memory::VP::Patch(Module::exe_ptr(DispOthcar_PatchAddr), { 0x90, 0x31, 0xC0 });

		return true;
	}

	static DisableVehicleLODs instance;
};
DisableVehicleLODs DisableVehicleLODs::instance;

class FixZBufferPrecision : public Hook
{
	const static int CalcCameraMatrix_Addr = 0x84BD0;
	const static int Clr_SceneEffect_Addr = 0xBE70;

	inline static SafetyHookInline CalcCameraMatrix = {};

	static inline bool allow_znear_override = true;
	static void CalcCameraMatrix_dest(EvWorkCamera* camera)
	{
		// improve z-buffer precision by increasing znear
		// game default is 0.1 which reduces precision of far objects massively, causing z-fighting and objects not drawing properly

		if (allow_znear_override)
		{
			// only set znear to 1 if...
			if ((camera->camera_mode_34A == 2 || camera->camera_mode_34A == 0) // ... in third-person or FPV
				&& camera->camera_mode_timer_364 == 0 // ... not switching cameras
				&& *Game::current_mode == STATE_GAME) // ... we're in main game state (not in STATE_START cutscene etc)
			{
				camera->perspective_znear_BC = 1.0f;
			}
			else
			{
				if (camera->camera_mode_timer_364 != 0 || *Game::current_mode != STATE_GAME)
					camera->perspective_znear_BC = 0.1f; // set znear to 0.1 during camera switch / cutscene
				else
					camera->perspective_znear_BC = 0.3f; // 0.3 seems fine for in-car view, doesn't improve as much as 1.0f but still better than 0.1f
			}
		}
		CalcCameraMatrix.call(camera);
	}

	// hook Clr_SceneEffect so we can reset camera z-near before screen effects are draw
	inline static SafetyHookInline Clr_SceneEffect = {};
	static void Clr_SceneEffect_dest(int a1)
	{
		FixZBufferPrecision::allow_znear_override = false;

		EvWorkCamera* camera = Module::exe_ptr<EvWorkCamera>(0x39FE10);

		float prev = camera->perspective_znear_BC;

		// apply vanilla znear
		camera->perspective_znear_BC = 0.05f; // game default = 0.1, but that causes lens flare to slightly clip, 0.05 allows it to fade properly
		CalcCameraMatrix_dest(camera);

		Clr_SceneEffect.call(a1);

		// restore orig znear
		camera->perspective_znear_BC = prev;
		CalcCameraMatrix_dest(camera);

		FixZBufferPrecision::allow_znear_override = true;
	}

public:
	std::string_view description() override
	{
		return "FixZBufferPrecision";
	}

	bool validate() override
	{
		return Settings::FixZBufferPrecision;
	}

	bool apply() override
	{
		CalcCameraMatrix = safetyhook::create_inline(Module::exe_ptr(CalcCameraMatrix_Addr), CalcCameraMatrix_dest);
		if (!CalcCameraMatrix)
			return false;

		Clr_SceneEffect = safetyhook::create_inline(Module::exe_ptr(Clr_SceneEffect_Addr), Clr_SceneEffect_dest);
		return !!Clr_SceneEffect;
	}

	static FixZBufferPrecision instance;
};
FixZBufferPrecision FixZBufferPrecision::instance;

class TransparencySupersampling : public Hook
{
	const static int DeviceInitHookAddr = 0xEC2F;
	const static int DeviceResetHookAddr = 0x17A20;

	inline static SafetyHookMid dest_hook = {};
	inline static SafetyHookMid deviceReset_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		auto* device = Game::D3DDevice();
		device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
		device->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, TRUE);
		device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		device->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);

		// NVIDIA transparency supersampling
		device->SetRenderState(D3DRS_ADAPTIVETESS_Y, D3DFORMAT(MAKEFOURCC('A', 'T', 'O', 'C')));
		device->SetRenderState(D3DRS_ADAPTIVETESS_Y, D3DFORMAT(MAKEFOURCC('S', 'S', 'A', 'A')));
	}

public:
	std::string_view description() override
	{
		return "TransparencySupersampling";
	}

	bool validate() override
	{
		return Settings::TransparencySupersampling;
	}

	bool apply() override
	{
		dest_hook = safetyhook::create_mid(Module::exe_ptr(DeviceInitHookAddr), destination);
		deviceReset_hook = safetyhook::create_mid(Module::exe_ptr(DeviceResetHookAddr), destination);
		return !!dest_hook;
	}

	static TransparencySupersampling instance;
};
TransparencySupersampling TransparencySupersampling::instance;

class WindowedBorderless : public Hook
{
	const static int WinMain_BorderlessWindow_WndStyleExAddr = 0x18175;
	const static int WinMain_BorderlessWindow_WndStyleAddr = 0x1817A;
	const static int Win32_Init_DisableWindowResize_Addr1 = 0xE9E3;
	const static int Win32_Init_DisableWindowResize_Addr2 = 0xEA30;
	const static int Win32_Init_SetWindowPos_Addr = 0xEAA7;

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		SetWindowPos(HWND(ctx.ebp), 0, 
			Settings::WindowPositionX, Settings::WindowPositionY, 
			Game::screen_resolution->x, Game::screen_resolution->y,
			0x40);
	}

public:
	std::string_view description() override
	{
		return "WindowedBorderless";
	}

	bool validate() override
	{
		return Settings::WindowedBorderless;
	}

	bool apply() override
	{
		auto* patch_addr = Module::exe_ptr(WinMain_BorderlessWindow_WndStyleExAddr);
		Memory::VP::Patch(patch_addr, uint32_t(0));

		patch_addr = Module::exe_ptr(WinMain_BorderlessWindow_WndStyleAddr);
		Memory::VP::Patch(patch_addr, uint32_t(WS_POPUP));

		patch_addr = Module::exe_ptr(Win32_Init_DisableWindowResize_Addr1);
		Memory::VP::Nop(patch_addr, 6);

		patch_addr = Module::exe_ptr(Win32_Init_DisableWindowResize_Addr2);
		Memory::VP::Nop(patch_addr, 6);

		// replace original SetWindowPos call
		Memory::VP::Nop(Module::exe_ptr(Win32_Init_SetWindowPos_Addr), 0x23);
		dest_hook = safetyhook::create_mid(Module::exe_ptr(Win32_Init_SetWindowPos_Addr), destination);

		return true;
	}

	static WindowedBorderless instance;
};
WindowedBorderless WindowedBorderless::instance;

class AnisotropicFiltering : public Hook
{
	const static int ChangeTexAttribute_HookAddr1 = 0x9AD8;
	const static int ChangeTexAttribute_HookAddr2 = 0x8960;

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		int Sampler = ctx.ebp;

		Game::D3DDevice()->SetSamplerState(Sampler, D3DSAMP_MAXANISOTROPY, Settings::AnisotropicFiltering);
	}

	inline static SafetyHookMid dest_hook2 = {};
	static void destination2(safetyhook::Context& ctx)
	{
		int Sampler = *(int*)(ctx.esp + 0xC);

		if (ctx.edi == D3DSAMP_MINFILTER || ctx.edi == D3DSAMP_MAGFILTER)
		{
			ctx.esi = D3DTEXF_ANISOTROPIC;

			Game::D3DDevice()->SetSamplerState(Sampler, D3DSAMP_MAXANISOTROPY, Settings::AnisotropicFiltering);
		}
	}

public:
	std::string_view description() override
	{
		return "AnisotropicFiltering";
	}

	bool validate() override
	{
		return Settings::AnisotropicFiltering > 0;
	}

	bool apply() override
	{
		dest_hook = safetyhook::create_mid(Module::exe_ptr(ChangeTexAttribute_HookAddr1), destination);
		dest_hook2 = safetyhook::create_mid(Module::exe_ptr(ChangeTexAttribute_HookAddr2), destination2);

		return true;
	}

	static AnisotropicFiltering instance;
};
AnisotropicFiltering AnisotropicFiltering::instance;

class VSyncOverride : public Hook
{
	const static int D3DInit_HookAddr = 0xEB66;

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		Game::D3DPresentParams->PresentationInterval = Settings::VSync;
		if (!Settings::VSync)
			Game::D3DPresentParams->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

		// TODO: add MultiSampleType / MultiSampleQuality overrides here?
		//  (doesn't seem any of them are improvement over vanilla "DX/ANTIALIASING = 2" though...)
	}

public:
	std::string_view description() override
	{
		return "VSync";
	}

	bool validate() override
	{
		return Settings::VSync != 1;
	}

	bool apply() override
	{
		dest_hook = safetyhook::create_mid(Module::exe_ptr(D3DInit_HookAddr), destination);

		return true;
	}

	static VSyncOverride instance;
};
VSyncOverride VSyncOverride::instance;
