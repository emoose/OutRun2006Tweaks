#pragma once

#include "plugin.hpp"
#include <d3d9.h>
#include <dinput.h>
#include "game.hpp"

typedef void(__stdcall* D3DXVec4Transform_fn)(D3DXVECTOR4*, D3DXVECTOR4*, D3DMATRIX*);

typedef void(__cdecl* mxPushLoadMatrix_fn)(D3DMATRIX*);
typedef void(__cdecl* mxTranslate_fn)(float, float, float);
typedef void(__cdecl* DrawObject_Internal_fn)(int, int, uint16_t*, int, int, int);
typedef void(__cdecl* DrawObjectAlpha_Internal_fn)(int, float, void*, int);
typedef void(__cdecl* mxPopMatrix_fn)();

namespace Game
{
	inline D3DXVECTOR2 original_resolution{ 640, 480 };

	inline int* game_mode = nullptr;
	inline GameState* current_mode = nullptr;
	inline int* game_start_progress_code = nullptr;
	inline int* file_load_progress_code = nullptr;

	static_assert(sizeof(bool) == sizeof(uint8_t)); // the following bools take 1 byte each
	inline bool* Sumo_CountdownTimerEnable = nullptr;
	inline bool* Sumo_IntroLogosEnable = nullptr;

	inline D3DPRESENT_PARAMETERS* D3DPresentParams = nullptr;
	inline IDirect3DDevice9** D3DDevice_ptr = nullptr;

	inline IDirectInput8A** DirectInput8_ptr = nullptr;

	inline HWND* hWnd_ptr = nullptr;

	inline IDirect3DDevice9* D3DDevice() {
		return *D3DDevice_ptr;
	}
	inline IDirectInput8A* DirectInput8() {
		return *DirectInput8_ptr;
	}
	inline HWND GameHwnd() {
		return *hWnd_ptr;
	}

	inline uint32_t* navipub_disp_flg = nullptr;

	inline int* sel_bgm_kind_buf = nullptr;

	inline s_chrset_info* chrset_info = nullptr;

	inline int* app_time = nullptr; // used by SetTweeningTable etc
	inline int* sprani_num_ticks = nullptr; // number of game ticks being ran in the current frame (can be 0 if above 60FPS)

	inline GameStage* stg_stage_num = nullptr;

	inline D3DXVECTOR2* screen_scale = nullptr;

	inline DrawBuffer* s_ImmDrawBuffer = nullptr;
	inline DrawBuffer* s_AftDrawBuffer = nullptr;

	// ini cfg
	inline D3DXVECTOR2* screen_resolution = nullptr;
	inline int* D3DFogEnabled = nullptr;
	inline int* D3DAdapterNum = nullptr;
	inline int* D3DAntialiasing = nullptr;
	inline uint8_t* D3DWindowed = nullptr;
	inline int* CfgLanguage = nullptr;

	// player data
	inline float* Sumo_NumOutRunMiles = nullptr;

	// game functions
	inline fn_0args SetFrameStartCpuTime = nullptr;
	inline fn_1arg_int CalcNumUpdatesToRun = nullptr;

	inline LPDIENUMDEVICESCALLBACKA DInput_EnumJoysticksCallback = nullptr;

	inline fn_0args Sumo_D3DResourcesRelease = nullptr;
	inline fn_0args Sumo_D3DResourcesCreate = nullptr;

	inline fn_1arg fn43FA10 = nullptr;

	inline fn_0args ReadIO = nullptr;
	inline fn_0args SoundControl_mb = nullptr;
	inline fn_0args LinkControlReceive = nullptr;
	inline fn_0args ModeControl = nullptr;
	inline fn_0args EventControl = nullptr;
	inline fn_0args GhostCarExecServer = nullptr;
	inline fn_0args fn4666A0 = nullptr;
	inline fn_0args FileLoad_Ctrl = nullptr;

	inline fn_1arg PrjSndRequest = nullptr;
	inline fn_1arg SetSndQueue = nullptr;

	inline fn_1arg_int SwitchNow = nullptr;
	inline int(*Sumo_CalcSteerSensitivity)(int cur, int prev) = nullptr;

	inline fn_1arg_int GetNowStageNum = nullptr;
	inline fn_1arg_int GetStageUniqueNum = nullptr;
	inline fn_1arg_int GetMaxCsLen = nullptr;
	inline fn_1arg_char GetStageUniqueName = nullptr;

	inline void(*QuickSort)(void*, int, int) = nullptr;
	inline void(*DrawStoredModel_Internal)(DrawBuffer*) = nullptr;

	inline fn_stdcall_1arg_int Sumo_CheckRacerUnlocked = nullptr;

	inline const char* SumoNet_OnlineUserName = nullptr;
	inline sSumoNet_LobbyInfo* SumoNet_LobbyInfo = nullptr;
	inline SumoNet_NetDriver** SumoNet_CurNetDriver = nullptr;

	// 2d sprite drawing
	inline fn_1arg sprSetFontPriority = nullptr;
	inline fn_1arg sprSetPrintFont = nullptr;
	inline fn_1arg sprSetFontColor = nullptr;
	inline fn_2floats sprSetFontScale = nullptr;
	inline fn_2args sprLocateP = nullptr;
	inline fn_printf sprPrintf = nullptr;

	inline fn_1arg_char Sumo_GetStringFromId = nullptr;
	inline fn_printf Sumo_Printf = nullptr;

	inline fn_0args_void SumoFrontEnd_GetSingleton_4035F0 = nullptr;
	inline fn_0args_class SumoFrontEnd_animate_443110 = nullptr;

	// 3d drawing
	inline DrawObject_Internal_fn DrawObject_Internal = nullptr;
	inline DrawObjectAlpha_Internal_fn DrawObjectAlpha_Internal = nullptr;
	inline int* power_on_timer = nullptr;

	// math
	inline mxPushLoadMatrix_fn mxPushLoadMatrix = (mxPushLoadMatrix_fn)0x409F90;
	inline mxTranslate_fn mxTranslate = (mxTranslate_fn)0x40A290;
	inline mxPopMatrix_fn mxPopMatrix = (mxPopMatrix_fn)0x40A010;

	// audio
	inline fn_3args adxPlay = nullptr;
	inline fn_0args adxStopAll = nullptr;

	// D3DX
	inline D3DXVec4Transform_fn D3DXVec4Transform = nullptr;

	inline sEventWork* event(int event_id)
	{
		sEventWork* s_EventWork = Module::exe_ptr<sEventWork>(0x399B30);
		return &s_EventWork[event_id];
	}

	inline EVWORK_CAR* pl_car()
	{
		return event(8)->data<EVWORK_CAR>();
	}

	inline bool is_in_game()
	{
		return
			*Game::current_mode == GameState::STATE_GAME ||
			*Game::current_mode == GameState::STATE_GOAL ||
			*Game::current_mode == GameState::STATE_TIMEUP ||
			*Game::current_mode == GameState::STATE_TRYAGAIN ||
			*Game::current_mode == GameState::STATE_OUTRUNMILES ||
			*Game::current_mode == GameState::STATE_SMPAUSEMENU ||
			(*Game::current_mode == GameState::STATE_START && *Game::game_start_progress_code == 65);
	}

	inline const char* StageNames[] = {
		"Palm Beach",
		"Deep Lake", "Industrial Complex",
		"Alpine", "Snowy Mountain", "Cloudy Highland",
		"Castle Wall", "Ghost Forest", "Coniferous Forest", "Desert",
		"Tulip Garden", "Metropolis", "Ancient Ruins", "Cape Way", "Imperial Avenue",

		"Sunny Beach",
		"Big Forest", "Waterfalls",
		"Casino Town", "Ice Scape", "Canyon",
		"Bay Area", "Jungle", "Lost City", "National Park",
		"Legend", "Skyscrapers", "Floral Village", "Milky Way", "Giant Statues",

		"(R) Palm Beach",
		"(R) Deep Lake", "(R) Industrial Complex",
		"(R) Alpine", "(R) Snowy Mountain", "(R) Cloudy Highland",
		"(R) Castle Wall", "(R) Ghost Forest", "(R) Coniferous Forest", "(R) Desert",
		"(R) Tulip Garden", "(R) Metropolis", "(R) Ancient Ruins", "(R) Cape Way", "(R) Imperial Avenue",

		"(R) Sunny Beach",
		"(R) Big Forest", "(R) Waterfalls",
		"(R) Casino Town", "(R) Ice Scape", "(R) Canyon",
		"(R) Bay Area", "(R) Jungle", "(R) Lost City", "(R) National Park",
		"(R) Legend", "(R) Skyscrapers", "(R) Floral Village", "(R) Milky Way", "(R) Giant Statues",

		"(T) Palm Beach", "(T) Sunny Beach",
		"(Night) Palm Beach", "(Night) Sunny Beach",
		"(R-Night) Palm Beach", "(R-Night) Sunny Beach"
	};

	inline const char* GetStageFriendlyName(GameStage stage)
	{
		if (int(stage) < 0x42)
			return StageNames[int(stage)];
		return StageNames[0];
	}

	inline void init()
	{
		game_mode = Module::exe_ptr<int>(0x380258);
		current_mode = Module::exe_ptr<GameState>(0x38026C);
		game_start_progress_code = Module::exe_ptr<int>(0x4367A8);
		file_load_progress_code = Module::exe_ptr<int>(0x436718);
		Sumo_CountdownTimerEnable = Module::exe_ptr<bool>(0x237911);
		Sumo_IntroLogosEnable = Module::exe_ptr<bool>(0x2319A1);

		D3DPresentParams = Module::exe_ptr<D3DPRESENT_PARAMETERS>(0x49BD64);
		D3DDevice_ptr = Module::exe_ptr<IDirect3DDevice9*>(0x49BD60);
		DirectInput8_ptr = Module::exe_ptr<IDirectInput8A*>(0x4606E8);
		hWnd_ptr = Module::exe_ptr<HWND>(0x4A8C88);

		navipub_disp_flg = Module::exe_ptr<uint32_t>(0x4447F8);

		sel_bgm_kind_buf = Module::exe_ptr<int>(0x430364);

		chrset_info = Module::exe_ptr<s_chrset_info>(0x254860);

		app_time = Module::exe_ptr<int>(0x49EDB8);
		sprani_num_ticks = Module::exe_ptr<int>(0x380278);

		stg_stage_num = Module::exe_ptr<GameStage>(0x3D2E8C);

		screen_scale = Module::exe_ptr<D3DXVECTOR2>(0x340C94);

		s_ImmDrawBuffer = Module::exe_ptr<DrawBuffer>(0x00464EF8);
		s_AftDrawBuffer = Module::exe_ptr<DrawBuffer>(0x004612D8);

		screen_resolution = Module::exe_ptr<D3DXVECTOR2>(0x340C8C);

		D3DFogEnabled = Module::exe_ptr<int>(0x340C88);
		D3DAdapterNum = Module::exe_ptr<int>(0x55AF00);
		D3DAntialiasing = Module::exe_ptr<int>(0x55AF04);
		D3DWindowed = Module::exe_ptr<uint8_t>(0x55AF08);
		CfgLanguage = Module::exe_ptr<int>(0x340CA0);

		Sumo_NumOutRunMiles = Module::exe_ptr<float>(0x3C2404);

		SetFrameStartCpuTime = Module::fn_ptr<fn_0args>(0x49430);
		CalcNumUpdatesToRun = Module::fn_ptr<fn_1arg_int>(0x17890);

		DInput_EnumJoysticksCallback = Module::fn_ptr<LPDIENUMDEVICESCALLBACKA>(0x3EF0);

		Sumo_D3DResourcesRelease = Module::fn_ptr<fn_0args>(0x17970);
		Sumo_D3DResourcesCreate = Module::fn_ptr<fn_0args>(0x17A20);

		fn43FA10 = Module::fn_ptr<fn_1arg>(0x3FA10);

		ReadIO = Module::fn_ptr<fn_0args>(0x53BB0); // ReadIO
		SoundControl_mb = Module::fn_ptr<fn_0args>(0x2F330); // SoundControl_mb
		LinkControlReceive = Module::fn_ptr<fn_0args>(0x55130); // LinkControlReceive
		ModeControl = Module::fn_ptr<fn_0args>(0x3FA20); // ModeControl
		EventControl = Module::fn_ptr<fn_0args>(0x3FAB0); // EventControl
		GhostCarExecServer = Module::fn_ptr<fn_0args>(0x80F80); // GhostCarExecServer
		fn4666A0 = Module::fn_ptr<fn_0args>(0x666A0);
		FileLoad_Ctrl = Module::fn_ptr<fn_0args>(0x4FBA0);

		PrjSndRequest = Module::fn_ptr<fn_1arg>(0x249F0);
		SetSndQueue = Module::fn_ptr<fn_1arg>(0x24940);

		SwitchNow = Module::fn_ptr<fn_1arg_int>(0x536C0);
		Sumo_CalcSteerSensitivity = Module::fn_ptr(0x537C0, Sumo_CalcSteerSensitivity);

		GetNowStageNum = Module::fn_ptr<fn_1arg_int>(0x50380);
		GetStageUniqueNum = Module::fn_ptr<fn_1arg_int>(0x4DC50);
		GetMaxCsLen = Module::fn_ptr<fn_1arg_int>(0x3D470);
		GetStageUniqueName = Module::fn_ptr<fn_1arg_char>(0x4BE80);

		QuickSort = Module::fn_ptr(0x499E0, QuickSort);
		DrawStoredModel_Internal = Module::fn_ptr(0x5890, DrawStoredModel_Internal);

		Sumo_CheckRacerUnlocked = Module::fn_ptr<fn_stdcall_1arg_int>(0xE8410);

		SumoNet_OnlineUserName = Module::exe_ptr<const char>(0x430C20);
		SumoNet_LobbyInfo = Module::exe_ptr<sSumoNet_LobbyInfo>(0x25A7A4);
		SumoNet_CurNetDriver = Module::exe_ptr<SumoNet_NetDriver*>(0x3D68AC);

		sprSetFontPriority = Module::fn_ptr<fn_1arg>(0x2CCB0);
		sprSetPrintFont = Module::fn_ptr<fn_1arg>(0x2CA60);
		sprSetFontColor = Module::fn_ptr<fn_1arg>(0x2CCA0);
		sprSetFontScale = Module::fn_ptr<fn_2floats>(0x2CC60);
		sprLocateP = Module::fn_ptr<fn_2args>(0x2CC00);
		sprPrintf = Module::fn_ptr<fn_printf>(0x2CCE0);

		Sumo_GetStringFromId = Module::fn_ptr<fn_1arg_char>(0x65EB0);
		Sumo_Printf = Module::fn_ptr<fn_printf>(0x2CDD0);

		SumoFrontEnd_GetSingleton_4035F0 = Module::fn_ptr<fn_0args_void>(0x35F0);
		SumoFrontEnd_animate_443110 = Module::fn_ptr<fn_0args_class>(0x43110);

		DrawObject_Internal = Module::fn_ptr<DrawObject_Internal_fn>(0x5360);
		DrawObjectAlpha_Internal = Module::fn_ptr<DrawObjectAlpha_Internal_fn>(0x56D0);
		power_on_timer = Module::exe_ptr<int>(0x55AF0C);

		mxPushLoadMatrix = Module::fn_ptr<mxPushLoadMatrix_fn>(0x9F90);
		mxTranslate = Module::fn_ptr<mxTranslate_fn>(0xA290);
		mxPopMatrix = Module::fn_ptr<mxPopMatrix_fn>(0xA010);

		adxPlay = Module::fn_ptr<fn_3args>(0x1000);
		adxStopAll = Module::fn_ptr<fn_0args>(0x1050);

		D3DXVec4Transform = Module::fn_ptr<D3DXVec4Transform_fn>(0x393B2);
	}
};
