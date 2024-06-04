#include "plugin.hpp"
#include <d3d9.h>

namespace Game
{
	inline GameState* current_mode = nullptr;
	inline int* game_start_progress_code = nullptr;
	inline int* file_load_progress_code = nullptr;
	inline int* sumo_load_sprani_67F614 = nullptr;
	inline int* adv_loading_logo = nullptr;

	inline float* screen_width = nullptr;
	inline float* screen_height = nullptr;

	inline D3DPRESENT_PARAMETERS* D3DPresentParams = nullptr;
	inline IDirect3DDevice9** D3DDevice_ptr = nullptr;

	inline IDirect3DDevice9* D3DDevice() {
		return *D3DDevice_ptr;
	}

	inline fn_0args SetFrameStartCpuTime = nullptr;
	inline fn_1arg_int CalcNumUpdatesToRun = nullptr;

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

	inline void init()
	{
		current_mode = Module::exe_ptr<GameState>(0x38026C);
		game_start_progress_code = Module::exe_ptr<int>(0x4367A8);
		file_load_progress_code = Module::exe_ptr<int>(0x436718);
		sumo_load_sprani_67F614 = Module::exe_ptr<int>(0x27F614);
		adv_loading_logo = Module::exe_ptr<int>(0x287778);

		screen_width = Module::exe_ptr<float>(0x340C8C);
		screen_height = Module::exe_ptr<float>(0x340C90);

		D3DPresentParams = Module::exe_ptr<D3DPRESENT_PARAMETERS>(0x49BD64);
		D3DDevice_ptr = Module::exe_ptr<IDirect3DDevice9*>(0x49BD60);

		SetFrameStartCpuTime = Module::fn_ptr<fn_0args>(0x49430);
		CalcNumUpdatesToRun = Module::fn_ptr<fn_1arg_int>(0x17890);

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
	}
};