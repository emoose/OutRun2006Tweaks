#pragma once

#include "plugin.hpp"
#include <d3d9.h>
#include <dinput.h>

namespace Game
{
	inline GameState* current_mode = nullptr;
	inline int* game_start_progress_code = nullptr;
	inline int* file_load_progress_code = nullptr;

	inline uint8_t* Sumo_CountdownTimerEnable = nullptr;
	inline uint8_t* Sumo_IntroLogosEnable = nullptr;

	inline D3DPRESENT_PARAMETERS* D3DPresentParams = nullptr;
	inline IDirect3DDevice9** D3DDevice_ptr = nullptr;

	inline IDirectInput8A** DirectInput8_ptr = nullptr;

	inline IDirect3DDevice9* D3DDevice() {
		return *D3DDevice_ptr;
	}
	inline IDirectInput8A* DirectInput8() {
		return *DirectInput8_ptr;
	}

	inline int* sel_bgm_kind_buf = nullptr;

	// ini cfg
	inline float* screen_width = nullptr;
	inline float* screen_height = nullptr;
	inline int* D3DFogEnabled = nullptr;
	inline int* D3DAdapterNum = nullptr;
	inline int* D3DAntialiasing = nullptr;
	inline uint8_t* D3DWindowed = nullptr;
	inline int* CfgLanguage = nullptr;

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

	inline fn_1arg PrjSndRequest = nullptr;

	inline fn_1arg_int GetNowStageNum = nullptr;
	inline fn_1arg_int GetStageUniqueNum = nullptr;

	// 2d sprite drawing
	inline fn_1arg sprSetFontPriority = nullptr;
	inline fn_1arg sprSetPrintFont = nullptr;
	inline fn_1arg sprSetFontColor = nullptr;
	inline fn_2floats sprSetFontScale = nullptr;
	inline fn_2args sprLocateP = nullptr;
	inline fn_printf sprPrintf = nullptr;

	// audio
	inline fn_2args adxPlay = nullptr;

	inline void init()
	{
		current_mode = Module::exe_ptr<GameState>(0x38026C);
		game_start_progress_code = Module::exe_ptr<int>(0x4367A8);
		file_load_progress_code = Module::exe_ptr<int>(0x436718);
		Sumo_CountdownTimerEnable = Module::exe_ptr<uint8_t>(0x237911);
		Sumo_IntroLogosEnable = Module::exe_ptr<uint8_t>(0x2319A1);

		D3DPresentParams = Module::exe_ptr<D3DPRESENT_PARAMETERS>(0x49BD64);
		D3DDevice_ptr = Module::exe_ptr<IDirect3DDevice9*>(0x49BD60);
		DirectInput8_ptr = Module::exe_ptr<IDirectInput8A*>(0x4606E8);

		sel_bgm_kind_buf = Module::exe_ptr<int>(0x430364);

		screen_width = Module::exe_ptr<float>(0x340C8C);
		screen_height = Module::exe_ptr<float>(0x340C90);

		D3DFogEnabled = Module::exe_ptr<int>(0x340C88);
		D3DAdapterNum = Module::exe_ptr<int>(0x55AF00);
		D3DAntialiasing = Module::exe_ptr<int>(0x55AF04);
		D3DWindowed = Module::exe_ptr<uint8_t>(0x55AF08);
		CfgLanguage = Module::exe_ptr<int>(0x340CA0);

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

		PrjSndRequest = Module::fn_ptr<fn_1arg>(0x249F0);

		GetNowStageNum = Module::fn_ptr<fn_1arg_int>(0x50380);
		GetStageUniqueNum = Module::fn_ptr<fn_1arg_int>(0x4DC50);

		sprSetFontPriority = Module::fn_ptr<fn_1arg>(0x2CCB0);
		sprSetPrintFont = Module::fn_ptr<fn_1arg>(0x2CA60);
		sprSetFontColor = Module::fn_ptr<fn_1arg>(0x2CCA0);
		sprSetFontScale = Module::fn_ptr<fn_2floats>(0x2CC60);
		sprLocateP = Module::fn_ptr<fn_2args>(0x2CC00);
		sprPrintf = Module::fn_ptr<fn_printf>(0x2CCE0);

		adxPlay = Module::fn_ptr<fn_2args>(0x1000);
	}
};
