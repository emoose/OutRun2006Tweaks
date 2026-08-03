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

// Frame interpolation: display-matrix builders, safe to re-run per rendered
// frame. Both read their blend factor from sub_4493E0 -> g_InterpAlpha.
typedef void(__cdecl* CalcDispMatrix_fn)(EVWORK_CAR*);
typedef void(__cdecl* CalcCameraMatrix_fn)(EvWorkCamera*);
// sub_4493E0: returns the effective interpolation alpha - g_InterpAlpha normally,
// but a hard 1.0 during autoscenes (GetPettyAutoSceneEnable / GetAutosceneStatus==3).
typedef float(__cdecl* GetInterpAlpha_fn)();
typedef void(__cdecl* mxCalcPoint_fn)(D3DVECTOR* out, const D3DVECTOR* in);

// Queues 2D draws into sprite_prio_root
// 
// Note: not const! hooks_textures hooks this and rewrites the SPRARGS in place to
// remap UVs onto a higher-resolution replacement texture.
typedef void(__cdecl* put_sprite_ex_fn)(SPRARGS* sprargs, float priority);

typedef int(__cdecl* sprani_play_ae_auth_alpha_fn)(uint32_t spriteId, float x, float y, int a4, int a5, float alpha);
typedef int(__cdecl* put_clip_sprite_fn)(int xstnum, int x, int y, uint32_t flags, float priority, uint32_t color);

// Module offsets used outside of this file.
namespace GameAddr
{
	constexpr int CalcDispMatrix = 0xA2650;
	constexpr int CalcCameraMatrix = 0x84BD0;
	constexpr int CalcCharMatrix = 0x88AF0;   // sub_488AF0, __usercall
	constexpr int OsoDynamics_Ctrl = 0xA9C80;
	constexpr int OsoDynamics_Disp = 0xA9DF0;
	constexpr int OsoCommonFunc_Disp = 0xA8A90;
	constexpr int OsoCommon_PushMatrix = 0xA8AAE; // push esi, just before mxPushLoadMatrix
	constexpr int OsoCommon_AfterPush = 0xA8AB4;  // add esp, 4, just after it
	// push ebp in HeartDisp_car_heart, where ebp holds the attached-heart
	// animation angle - used for both Sinf (bob/squash) and mxRotateY (spin).
	constexpr int HeartDisp_PulseAngle = 0x5B43A;
}

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

	// --- frame interpolation ---
	// The game has a complete render-interpolation system that is disabled by
	// a hardcoded constant. Car *_Ctrl functions copy the current transform into
	// a "previous" slot each tick (position_14 -> field_16C, field_2C ->
	// field_17C), and the display-matrix builders lerp between them using a
	// single global alpha read via sub_4493E0:
	//
	//     if (GetPettyAutoSceneEnable() || GetAutosceneStatus() == 3) return 1.0;
	//     return g_InterpAlpha;
	//
	// g_InterpAlpha is 1.0 and is never changed by any code, so both lerps
	// collapse to a no-op. Writing a fractional value re-enables the original system.
	//
	// CalcDispMatrix / CalcCameraMatrix make use of this function that returns the alpha,
	// however these are only ever called as part of the game ticks, never from the
	// render path, so they must be replayed once per rendered frame to interpolate.
	inline CalcDispMatrix_fn CalcDispMatrix = nullptr;     // 0x4A2650
	inline CalcCameraMatrix_fn CalcCameraMatrix = nullptr; // 0x484BD0

	inline float* g_InterpAlpha = nullptr; // 0x634B34

	// Always ask this for the alpha actually in force rather than reading
	// g_InterpAlpha directly - it is what CalcDispMatrix/sub_482F20 see.
	inline GetInterpAlpha_fn GetInterpAlpha = nullptr; // 0x4493E0

	// Sub-tick remainder maintained by CalcNumUpdatesToRun, in QPC ticks:
	//     ticks     = 60 * (elapsed + remainder) / freq
	//     remainder = elapsed + remainder - ticks * freq / 60
	// so alpha = remainder * 60 / qpc_freq, valid right after that call.
	inline int64_t* frameskip_remainder = nullptr; // 0x8A8CD0

	// Y-scale of the whole stage during the load-in "rise from the ground"
	// animation.
	inline float* stage_disp_scale = nullptr; // 0x7D3184

	// HAM "catch the hearts": AttachHeart bakes each heart's world position from
	// its owner car's matrix_B0 during the tick, and HeartDisp_car_heart draws
	// straight from those baked values - so they stall at tick rate once the car
	// matrices are interpolated. Entry layout (stride 0x5C, 32 entries):
	//   +0x00 event id (0x19A = unused)
	//   +0x04 / +0x10 local offsets
	//   +0x28 / +0x34 baked world positions
	//   +0x4C / +0x50 per-heart active flags
	inline AttachHeartEntry* attach_heart_table = nullptr; // 0x8037C8
	inline ConnectionEntry* connection_tbl = nullptr;      // 0x800AF8
	// Y-spin of the 3D heart drawn above each connected line.
	inline float* hart_rot_f = nullptr;                    // 0x804384
	inline uint8_t* g_EventIsOpenFlag = nullptr;  // 0x79FB48
	inline mxCalcPoint_fn mxCalcPoint = nullptr;  // 0x40A7D0

	inline fn_0args ReadIO = nullptr;
	inline SumoDInputState* dinput_state = nullptr;
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

	// The per-frame 2D draw queue itself: one list head per priority.
	inline SpriteNode** sprite_prio_root = nullptr;
	inline put_sprite_ex_fn put_sprite_ex = nullptr;
	inline sprani_play_ae_auth_alpha_fn sprani_play_ae_auth_alpha = nullptr;
	inline put_clip_sprite_fn put_clip_sprite = nullptr;

	inline constexpr int SpritePriorityCount = 0x15;
	inline constexpr int SpriteNodeMax = 0x230; // ring capacity of spr_list

	inline fn_1arg_char Sumo_GetStringFromId = nullptr;
	inline fn_printf Sumo_Printf = nullptr;

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

	// sub_488AF0(car@<esi>, rob, charIndex) rebuilds an in-car character's base
	// matrix from the car's transform.
	// Unfortunately this is __usercall, with car pointer inside esi, so needs
	// a thunk in order to call properly.
	inline uintptr_t CalcCharMatrixAddr = 0;

	static __declspec(naked) void CalcCharMatrix(EVWORK_CAR* /*car*/, void* /*rob*/, int /*charIndex*/)
	{
		__asm
		{
			push ebp
			mov  ebp, esp
			push esi
			push ebx
			mov  eax, [ebp + 16]   // charIndex
			push eax
			mov  eax, [ebp + 12]   // rob
			push eax
			mov  esi, [ebp + 8]    // car -> esi
			mov  ebx, CalcCharMatrixAddr
			call ebx
			add  esp, 8            // caller-cleaned
			pop  ebx
			pop  esi
			mov  esp, ebp
			pop  ebp
			ret
		}
	}

	inline sEventWork* event(int event_id)
	{
		sEventWork* s_EventWork = Module::exe_ptr<sEventWork>(0x399B30);
		return &s_EventWork[event_id];
	}

	inline EVWORK_CAR* pl_car()
	{
		return event(8)->data<EVWORK_CAR>();
	}

	// Camera object used by CalcCameraMatrix / sub_482F20. May be null outside
	// of gameplay (menus, loading), so callers must check.
	inline EvWorkCamera* camera()
	{
		return event(EVENT_CAMERA)->data<EvWorkCamera>();
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

		CalcDispMatrix = Module::fn_ptr<CalcDispMatrix_fn>(GameAddr::CalcDispMatrix);
		CalcCameraMatrix = Module::fn_ptr<CalcCameraMatrix_fn>(GameAddr::CalcCameraMatrix);
		g_InterpAlpha = Module::exe_ptr<float>(0x234B34);
		GetInterpAlpha = Module::fn_ptr<GetInterpAlpha_fn>(0x493E0);
		frameskip_remainder = Module::exe_ptr<int64_t>(0x4A8CD0);
		stage_disp_scale = Module::exe_ptr<float>(0x3D3184);
		attach_heart_table = Module::exe_ptr<AttachHeartEntry>(0x4037C8);
		connection_tbl = Module::exe_ptr<ConnectionEntry>(0x400AF8);
		hart_rot_f = Module::exe_ptr<float>(0x404384);
		g_EventIsOpenFlag = Module::exe_ptr<uint8_t>(0x39FB48);
		mxCalcPoint = Module::fn_ptr<mxCalcPoint_fn>(0xA7D0);
		CalcCharMatrixAddr = reinterpret_cast<uintptr_t>(Module::exe_ptr(GameAddr::CalcCharMatrix));

		ReadIO = Module::fn_ptr<fn_0args>(0x53BB0); // ReadIO
		dinput_state = Module::exe_ptr<SumoDInputState>(0x4999C0);
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

		sprite_prio_root = Module::exe_ptr<SpriteNode*>(0x556C00);
		put_sprite_ex = Module::fn_ptr<put_sprite_ex_fn>(0x2CFE0);
		sprani_play_ae_auth_alpha = Module::fn_ptr<sprani_play_ae_auth_alpha_fn>(0x29580);
		put_clip_sprite = Module::fn_ptr<put_clip_sprite_fn>(0x2D280);

		Sumo_GetStringFromId = Module::fn_ptr<fn_1arg_char>(0x65EB0);
		Sumo_Printf = Module::fn_ptr<fn_printf>(0x2CDD0);

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
