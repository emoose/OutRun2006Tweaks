#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <array>

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

class WindowedHideMouseCursor : public Hook
{
	const static int WndProc_Addr = 0x17F90;

	inline static SafetyHookInline dest_orig = {};
	static LRESULT __stdcall destination(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_SETFOCUS || (msg == WM_ACTIVATE && lParam != WA_INACTIVE))
		{
			ShowCursor(false);
		}
		else if (msg == WM_ERASEBKGND) // erase window to white during device reset
		{
			RECT rect;
			GetClientRect(hwnd, &rect);
			HBRUSH brush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
			FillRect((HDC)wParam, &rect, brush);
			DeleteObject(brush);
			return 1;
		}
		// Other message handling
		return dest_orig.stdcall<LRESULT>(hwnd, msg, wParam, lParam);
	}

public:
	std::string_view description() override
	{
		return "WindowedHideMouseCursor";
	}

	bool validate() override
	{
		return Settings::WindowedHideMouseCursor;
	}

	bool apply() override
	{
		dest_orig = safetyhook::create_inline(Module::exe_ptr(WndProc_Addr), destination);
		return !!dest_orig;
	}

	static WindowedHideMouseCursor instance;
};
WindowedHideMouseCursor WindowedHideMouseCursor::instance;

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

enum class ScalingMode
{
	Vanilla,
	KeepCentered,
	OnlineArcade,
	Other
};

#define SCREEN_ONE_THIRD 213
#define SCREEN_TWO_THIRD 426

class UIScaling : public Hook
{
	const static int D3DXMatrixTransformation2D_Addr = 0x39400;
	const static int draw_sprite_custom_matrix_mid_Addr = 0x2A556;
	const static int Calc3D2D_Addr = 0x49940;

	const static int NaviPub_Disp_SpriteScaleEnable_Addr = 0xBEB31;
	const static int NaviPub_Disp_SpriteScaleEnable2_Addr = 0xBEDBE;
	const static int NaviPub_Disp_SpriteScaleDisable_Addr = 0xBEDE0;
	const static int NaviPub_Disp_SpriteScaleDisable2_Addr = 0xBEE21;
	const static int drawFootage_caller_Addr = 0x293F3;
	const static int draw_sprite_custom_matrix_multi__case2_Addr = 0x2B1E2;
	const static int draw_sprite_custom_matrix_multi__case3_Addr = 0x2B53E;
	const static int draw_sprite_custom_matrix_multi__case4_Addr = 0x2BB2F;

	// D3DXMatrixTransformation2D hook allows us to change draw_sprite_custom
	static inline SafetyHookInline D3DXMatrixTransformation2D = {};
	static int __stdcall D3DXMatrixTransformation2D_dest(D3DMATRIX* pOut, D3DXVECTOR2* pScalingCenter, float pScalingRotation,
		D3DXVECTOR2* pScaling, D3DXVECTOR2* pRotationCenter, float Rotation, D3DXVECTOR2* pTranslation)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);

		if (mode == ScalingMode::KeepCentered || mode == ScalingMode::OnlineArcade)
		{
			// Multiply by the smallest scale factor
			float scale = min(Game::screen_scale->x, Game::screen_scale->y);

			pScaling->x = (pScaling->x / Game::screen_scale->x) * scale;
			pScaling->y = (pScaling->y / Game::screen_scale->y) * scale;

			float origX = (pTranslation->x / Game::screen_scale->x);

			// TODO: this allows fixing the race position HUD element to be actually positioned properly
			// but right now it'd end up breaking some other calls to draw_sprite_custom, like the text for button inputs on menus
			// would need some way to know that this is actually the position HUD being drawn for us to only apply to that
			// (alternately: find the code responsible for the position HUD and reposition it when that sets it up?)
			// 
			// ...but it turns out online arcade seems to have left the 1st/2nd/3rd HUD semi-broken?
			// eg. https://youtu.be/-UqxjFgGvhk?t=39 shows it positioned slightly left of the actual "Position" text
			// which doesn't match how it displays at 4:3 at all...
			// guess they also ran into the same issue with it applying to other sprites and left it as is?
#if 0
			if (mode == ScalingMode::OnlineArcade && SpriteScalingEnabled)
			{
				float spacing = -((Game::screen_scale->y * Game::original_resolution.x) - Game::screen_resolution->x) / 2;

				// Space out the UI elements if they're past a certain X position
				// Seems this is pretty much what online arcade does
				// TODO: add checks to skip processing non-HUD elements

				float add = 0;
				if (origX < SCREEN_ONE_THIRD)
					add = -(spacing / Game::screen_scale->x);
				if (origX >= SCREEN_TWO_THIRD)
					add = (spacing / Game::screen_scale->x);
				origX += add;
			}
#endif
			// Reposition sprite to be centered
			float centering = (Game::screen_resolution->x - (Game::original_resolution.x * scale)) / 2;
			pTranslation->x = (origX * scale) + centering;
			pTranslation->y = (pTranslation->y / Game::screen_scale->y) * scale;
		}

		return D3DXMatrixTransformation2D.stdcall<int>(pOut, pScalingCenter, pScalingRotation, pScaling, pRotationCenter, Rotation, pTranslation);
	}

	static inline SafetyHookMid draw_sprite_custom_matrix_hk = {};
	static void __cdecl draw_sprite_custom_matrix_mid(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);

		float* g_spriteVertexStream = Module::exe_ptr<float>(0x58B868);
		SPRARGS2* a1 = (SPRARGS2*)ctx.ebx;

		D3DSURFACE_DESC v25;
		a1->d3dtexture_ptr_C->GetLevelDesc(0, &v25);
		float v21 = 0.50999999 / (double)v25.Width;
		float v22 = 0.50999999 / (double)v25.Height;

		D3DMATRIX pM;
		memcpy(&pM, &a1->mtx_14, sizeof(pM));

		D3DXVECTOR4 vec;

		D3DXVECTOR4 topLeft{ 0 };
		D3DXVECTOR4 bottomLeft{ 0 };
		D3DXVECTOR4 topRight{ 0 };
		D3DXVECTOR4 bottomRight{ 0 };

		float scaleY = Game::screen_scale->y;

		// Multiply by the smallest scale factor
		if (mode == ScalingMode::KeepCentered || mode == ScalingMode::OnlineArcade)
			scaleY = min(Game::screen_scale->x, Game::screen_scale->y);

		// TopLeft
		vec.x = a1->TopLeft_54.x;
		vec.y = a1->TopLeft_54.y;
		vec.z = a1->TopLeft_54.z;
		vec.w = 1.0;
		Game::D3DXVec4Transform(&topLeft, &vec, &pM);

		g_spriteVertexStream[1] = scaleY * topLeft.y + 0.5;
		g_spriteVertexStream[2] = topLeft.z;
		g_spriteVertexStream[3] = 1.0;
		g_spriteVertexStream[4] = a1->color_4;
		g_spriteVertexStream[5] = v21 + a1->top_9C;
		g_spriteVertexStream[6] = -v22 + a1->left_A0;

		// BottomLeft
		vec.x = a1->BottomLeft_60.x;
		vec.y = a1->BottomLeft_60.y;
		vec.z = a1->BottomLeft_60.z;
		vec.w = 1.0;
		Game::D3DXVec4Transform(&bottomLeft, &vec, &pM);

		g_spriteVertexStream[8] = scaleY * bottomLeft.y + 0.5;
		g_spriteVertexStream[9] = bottomLeft.z;
		g_spriteVertexStream[0xA] = 1.0;
		g_spriteVertexStream[0xB] = a1->color_4;
		g_spriteVertexStream[0xC] = v21 + a1->top_94;
		g_spriteVertexStream[0xD] = v22 + a1->right_98;

		// TopRight
		vec.x = a1->TopRight_6C.x;
		vec.y = a1->TopRight_6C.y;
		vec.z = a1->TopRight_6C.z;
		vec.w = 1.0;
		Game::D3DXVec4Transform(&topRight, &vec, &pM);

		g_spriteVertexStream[0xF] = scaleY * topRight.y + 0.5;
		g_spriteVertexStream[0x10] = topRight.z;
		g_spriteVertexStream[0x11] = 1.0;
		g_spriteVertexStream[0x12] = a1->color_4;
		g_spriteVertexStream[0x13] = -v21 + a1->bottom_84;
		g_spriteVertexStream[0x14] = -v22 + a1->left_88;

		// BottomRight
		vec.x = a1->BottomRight_78.x;
		vec.y = a1->BottomRight_78.y;
		vec.z = a1->BottomRight_78.z;
		vec.w = 1.0;
		Game::D3DXVec4Transform(&bottomRight, &vec, &pM);

		g_spriteVertexStream[0x16] = scaleY * bottomRight.y + 0.5;
		g_spriteVertexStream[0x17] = bottomRight.z;
		g_spriteVertexStream[0x18] = 1.0;
		g_spriteVertexStream[0x19] = a1->color_4;
		g_spriteVertexStream[0x1A] = -v21 + a1->bottom_8C;
		g_spriteVertexStream[0x1B] = v22 + a1->right_90;

		if (mode == ScalingMode::KeepCentered || mode == ScalingMode::OnlineArcade)
		{
			float add = (Game::screen_resolution->x - (Game::original_resolution.x * scaleY)) / 2;

			g_spriteVertexStream[0] = (scaleY * topLeft.x) + add;
			g_spriteVertexStream[7] = (scaleY * bottomLeft.x) + add;
			g_spriteVertexStream[0xE] = (scaleY * topRight.x) + add;
			g_spriteVertexStream[0x15] = (scaleY * bottomRight.x) + add;
		}
		else // if (mode == Mode::Vanilla)
		{
			g_spriteVertexStream[0] = (Game::screen_scale->y * topLeft.x);
			g_spriteVertexStream[7] = (Game::screen_scale->y * bottomLeft.x);
			g_spriteVertexStream[0xE] = (Game::screen_scale->y * topRight.x);
			g_spriteVertexStream[0x15] = (Game::screen_scale->y * bottomRight.x);
		}

		// Game seems to add these, half-pixel offset?
		g_spriteVertexStream[0] += 0.5f;
		g_spriteVertexStream[7] += 0.5f;
		g_spriteVertexStream[0xE] += 0.5f;
		g_spriteVertexStream[0x15] += 0.5f;

		Game::D3DDevice()->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2u, g_spriteVertexStream, 0x1Cu);
	}

	// Adjust positions of sprites in 3d space (eg 1st/2nd/etc markers)
	static inline SafetyHookInline Calc3D2D_hk = {};
	static void Calc3D2D_dest(float a1, float a2, D3DVECTOR* in, D3DVECTOR* out)
	{
		Calc3D2D_hk.call(a1, a2, in, out);

		// TODO: OnlineArcade mode needs to add position here

		ScalingMode mode = ScalingMode(Settings::UIScalingMode);

		if (mode == ScalingMode::KeepCentered || mode == ScalingMode::OnlineArcade)
			out->x = (out->x / Game::screen_scale->y) * Game::screen_scale->x;
	};

	enum SpriteScaleType
	{
		Disabled = 0,
		DetectEdges = 1,
		ForceLeft = 2,
		ForceRight = 3
	};

	static inline SpriteScaleType ScalingType = SpriteScaleType::Disabled;

	static inline SafetyHookMid drawFootage{};
	static void drawFootage_dest(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode == ScalingMode::OnlineArcade && ScalingType != SpriteScaleType::Disabled)
		{
			float spacing = -((Game::screen_scale->y * Game::original_resolution.x) - Game::screen_resolution->x) / 2;

			// Space out the UI elements if they're past a certain X position
			// Seems this is pretty much what online arcade does
			// TODO: add checks to skip processing non-HUD elements

			float* m = *Module::exe_ptr<float*>(0x49B564);
			float x = m[12];

			float add = 0;
			if (ScalingType == SpriteScaleType::ForceLeft || ScalingType == SpriteScaleType::DetectEdges && x < SCREEN_ONE_THIRD)
				add = -(spacing / Game::screen_scale->x);
			if (ScalingType == SpriteScaleType::ForceRight || ScalingType == SpriteScaleType::DetectEdges && x >= SCREEN_TWO_THIRD)
				add = (spacing / Game::screen_scale->x);

			m[12] = x + add;
		}
	}
	static inline SafetyHookMid draw_sprite_custom_matrix_multi_CenterSprite_hk{};
	static void draw_sprite_custom_matrix_multi_CenterSprite(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode != ScalingMode::KeepCentered && mode != ScalingMode::OnlineArcade)
			return;

		float* vtxStream = (float*)(ctx.ecx);

		float scale = min(Game::screen_scale->x, Game::screen_scale->y);
		float centering = (Game::screen_resolution->x - (Game::original_resolution.x * scale)) / 2;

		float x1 = vtxStream[0];
		float x2 = vtxStream[9];
		float x3 = vtxStream[18];
		float x4 = vtxStream[27];

		vtxStream[0] = ((vtxStream[0] / Game::screen_scale->x) * scale) + centering;
		vtxStream[9] = ((vtxStream[9] / Game::screen_scale->x) * scale) + centering;
		vtxStream[18] = ((vtxStream[18] / Game::screen_scale->x) * scale) + centering;
		vtxStream[27] = ((vtxStream[27] / Game::screen_scale->x) * scale) + centering;
	}

	static inline SafetyHookMid draw_sprite_custom_matrix_multi_CenterSprite2_hk{};
	static void draw_sprite_custom_matrix_multi_CenterSprite2(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode != ScalingMode::KeepCentered && mode != ScalingMode::OnlineArcade)
			return;

		float* vtxStream = (float*)(ctx.esp + 0x80);

		float scale = min(Game::screen_scale->x, Game::screen_scale->y);
		float centering = (Game::screen_resolution->x - (Game::original_resolution.x * scale)) / 2;

		float x1 = vtxStream[0];
		float x2 = vtxStream[11];
		float x3 = vtxStream[22];
		float x4 = vtxStream[33];

		vtxStream[0] = ((vtxStream[0] / Game::screen_scale->x) * scale) + centering;
		vtxStream[11] = ((vtxStream[11] / Game::screen_scale->x) * scale) + centering;
		vtxStream[22] = ((vtxStream[22] / Game::screen_scale->x) * scale) + centering;
		vtxStream[33] = ((vtxStream[33] / Game::screen_scale->x) * scale) + centering;
	}

	static inline SafetyHookMid draw_sprite_custom_matrix_multi_CenterSprite3_hk{};
	static void draw_sprite_custom_matrix_multi_CenterSprite3(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode != ScalingMode::KeepCentered && mode != ScalingMode::OnlineArcade)
			return;

		float* vtxStream = (float*)(ctx.edx);

		float scale = min(Game::screen_scale->x, Game::screen_scale->y);
		float centering = (Game::screen_resolution->x - (Game::original_resolution.x * scale)) / 2;

		float x1 = vtxStream[0];
		float x2 = vtxStream[13];
		float x3 = vtxStream[26];
		float x4 = vtxStream[39];

		vtxStream[0] = ((vtxStream[0] / Game::screen_scale->x) * scale) + centering;
		vtxStream[13] = ((vtxStream[13] / Game::screen_scale->x) * scale) + centering;
		vtxStream[26] = ((vtxStream[26] / Game::screen_scale->x) * scale) + centering;
		vtxStream[39] = ((vtxStream[39] / Game::screen_scale->x) * scale) + centering;
	}

	static void SpriteSpacingEnable(safetyhook::Context& ctx)
	{
		ScalingType = SpriteScaleType::DetectEdges;
	}
	static void SpriteSpacingDisable(safetyhook::Context& ctx)
	{
		ScalingType = SpriteScaleType::Disabled;
	}
	static void SpriteSpacingForceLeft(safetyhook::Context& ctx)
	{
		ScalingType = SpriteScaleType::ForceLeft;
	}
	static void SpriteSpacingForceRight(safetyhook::Context& ctx)
	{
		ScalingType = SpriteScaleType::ForceRight;
	}

	// NaviPub_Disp
	static inline SafetyHookMid NaviPub_Disp_SpriteSpacingEnable_hk{};
	static inline SafetyHookMid NaviPub_Disp_SpriteSpacingEnable2_hk{};

	static inline SafetyHookMid NaviPub_Disp_SpriteSpacingDisable_hk{};
	static inline SafetyHookMid NaviPub_Disp_SpriteSpacingDisable2_hk{};

	// dispMarkerCheck
	static inline SafetyHookMid dispMarkerCheck_SpriteScalingDisable_hk{};

	// DispTimeAttack2D
	static inline SafetyHookMid DispTimeAttack2D_SpriteScalingForceRight_hk{};
	static inline SafetyHookMid DispTimeAttack2D_SpriteScalingForceLeft_hk{};
	static inline SafetyHookMid DispTimeAttack2D_SpriteScalingDisable_hk{};
	static inline SafetyHookMid DispTimeAttack2D_SpriteScalingForceEnable_hk{};

	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk2{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk3{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk4{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk5{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk6{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk7{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk8{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk9{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk10{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk11{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk12{};

	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk1{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk2{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk3{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk4{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk5{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk6{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk7{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk8{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk9{};

	static void put_scroll_AdjustPosition(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode != ScalingMode::OnlineArcade)
			return;

		float spacing = -((Game::screen_scale->y * Game::original_resolution.x) - Game::screen_resolution->x) / 2;
		spacing = spacing / Game::screen_scale->x;

		int* positionX = (int*)(ctx.esp + 4);
		*positionX += (int)spacing;
	}

	// PutGhostGapInfo
	static inline SafetyHookMid PutGhostGapInfo_AdjustPosition_hk{};
	static void PutGhostGapInfo_AdjustPosition(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode != ScalingMode::OnlineArcade)
			return;

		float spacing = -((Game::screen_scale->y * Game::original_resolution.x) - Game::screen_resolution->x) / 2;
		spacing = spacing / Game::screen_scale->x;

		int* val = (int*)&ctx.ebp;
		if (*val == 102)
			spacing = -spacing;

		*val += (int)spacing;
	};

	// Fix position of the "Ghost/You/Diff" sprites shown with ghost car info
	// Online arcade doesn't seem to adjust this, maybe was left broken in that? (it's needed for 21:9 at least...)
	static inline SafetyHookMid PutGhostGapInfo_sub_AdjustPosition_hk{};
	static void PutGhostGapInfo_sub_AdjustPosition(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode != ScalingMode::OnlineArcade)
			return;

		float spacing = -((Game::screen_scale->y * Game::original_resolution.x) - Game::screen_resolution->x) / 2;
		spacing = spacing / Game::screen_scale->x;

		float val = ctx.xmm0.f32[0];
		if (val == 4.0f)
			spacing = -spacing;
		ctx.xmm0.f32[0] = val + spacing;
	}

	// DispGhostGap
	static inline SafetyHookMid DispGhostGap_ForceLeft_hk{};
	static inline SafetyHookMid DispGhostGap_ForceLeft2_hk{};
	static inline SafetyHookMid DispGhostGap_ForceRight_hk{};

	// NaviPub_DispTimeAttackGoal
	static inline SafetyHookMid NaviPub_DispTimeAttackGoal_DisableScaling_hk{};

	// Heart totals
	static inline SafetyHookMid NaviPub_Disp_HeartDisableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_HeartEnableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_C2CHeartDisableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_C2CHeartEnableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_C2CHeartEnableScaling2_hk{};

	static inline SafetyHookMid ctrl_icon_work_AdjustPosition_hk{};
	static void ctrl_icon_work_AdjustPosition(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode != ScalingMode::OnlineArcade)
			return;

		float spacing = -((Game::screen_scale->y * Game::original_resolution.x) - Game::screen_resolution->x) / 2;
		spacing = spacing / Game::screen_scale->x;
		ctx.xmm0.f32[0] = ctx.xmm0.f32[0] + spacing;
	}

	static inline SafetyHookMid ctrl_icon_work_AdjustPosition2_hk{};
	static inline SafetyHookMid set_icon_work_AdjustPosition_hk{};
	static void ctrl_icon_work_AdjustPosition2(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode != ScalingMode::OnlineArcade)
			return;

		float spacing = -((Game::screen_scale->y * Game::original_resolution.x) - Game::screen_resolution->x) / 2;
		spacing = spacing / Game::screen_scale->x;
		ctx.xmm0.f32[0] = ctx.xmm0.f32[0] + spacing;

		*(float*)(ctx.esp) = ctx.xmm0.f32[0];
	}

	static inline SafetyHookMid DispTempHeartNum_AdjustPosition_hk{};
	static void DispTempHeartNum_AdjustPosition(safetyhook::Context& ctx)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode != ScalingMode::OnlineArcade)
			return;

		float spacing = -((Game::screen_scale->y * Game::original_resolution.x) - Game::screen_resolution->x) / 2;
		spacing = spacing / Game::screen_scale->x;

		int* val = (int*)(ctx.esp);
		*val = int(float(*val) + spacing);
	}

public:
	std::string_view description() override
	{
		return "UIScaling";
	}

	bool validate() override
	{
		return Settings::UIScalingMode > 0;
	}

	bool apply() override
	{
		Memory::VP::Nop(Module::exe_ptr(draw_sprite_custom_matrix_mid_Addr), 0x293);

		draw_sprite_custom_matrix_hk = safetyhook::create_mid(Module::exe_ptr(draw_sprite_custom_matrix_mid_Addr), draw_sprite_custom_matrix_mid);

		// D3DXMatrixTransformation2D hook allows us to change draw_sprite_custom
		D3DXMatrixTransformation2D = safetyhook::create_inline(Module::exe_ptr(D3DXMatrixTransformation2D_Addr), D3DXMatrixTransformation2D_dest);

		Calc3D2D_hk = safetyhook::create_inline(Module::exe_ptr(Calc3D2D_Addr), Calc3D2D_dest);

		NaviPub_Disp_SpriteSpacingEnable_hk = safetyhook::create_mid(Module::exe_ptr(NaviPub_Disp_SpriteScaleEnable_Addr), SpriteSpacingEnable);
		NaviPub_Disp_SpriteSpacingEnable2_hk = safetyhook::create_mid(Module::exe_ptr(NaviPub_Disp_SpriteScaleEnable2_Addr), SpriteSpacingEnable);
		NaviPub_Disp_SpriteSpacingDisable_hk = safetyhook::create_mid(Module::exe_ptr(NaviPub_Disp_SpriteScaleDisable_Addr), SpriteSpacingDisable);
		NaviPub_Disp_SpriteSpacingDisable2_hk = safetyhook::create_mid(Module::exe_ptr(NaviPub_Disp_SpriteScaleDisable2_Addr), SpriteSpacingDisable);
		NaviPub_Disp_HeartDisableScaling_hk = safetyhook::create_mid((void*)0x4BEBE1, SpriteSpacingDisable);
		NaviPub_Disp_HeartEnableScaling_hk = safetyhook::create_mid((void*)0x4BEBE6, SpriteSpacingEnable);

		NaviPub_Disp_C2CHeartDisableScaling_hk = safetyhook::create_mid((void*)0x481B76, SpriteSpacingDisable);
		NaviPub_Disp_C2CHeartEnableScaling_hk = safetyhook::create_mid((void*)0x4BECBA, SpriteSpacingEnable);
		NaviPub_Disp_C2CHeartEnableScaling2_hk = safetyhook::create_mid((void*)0x4BECE0, SpriteSpacingEnable);

		// dispMarkerCheck is called by all three rival-marker functions, hopefully can fix them all
		dispMarkerCheck_SpriteScalingDisable_hk = safetyhook::create_mid((void*)0x4BA0E0, SpriteSpacingDisable);

		drawFootage = safetyhook::create_mid(Module::exe_ptr(drawFootage_caller_Addr), drawFootage_dest);

		// Hook the DrawPrimitiveUP call inside the three custom_matrix_multi cases so we can fix up X position
		// (except case3 doesn't point to the scaled-position-array for some reason, so we have to hook slightly earlier)
		draw_sprite_custom_matrix_multi_CenterSprite_hk = safetyhook::create_mid(Module::exe_ptr(draw_sprite_custom_matrix_multi__case2_Addr), draw_sprite_custom_matrix_multi_CenterSprite);
		draw_sprite_custom_matrix_multi_CenterSprite2_hk = safetyhook::create_mid(Module::exe_ptr(draw_sprite_custom_matrix_multi__case3_Addr), draw_sprite_custom_matrix_multi_CenterSprite2);
		draw_sprite_custom_matrix_multi_CenterSprite3_hk = safetyhook::create_mid(Module::exe_ptr(draw_sprite_custom_matrix_multi__case4_Addr), draw_sprite_custom_matrix_multi_CenterSprite3);

		// Fixes for the time displays in time attack mode
		DispTimeAttack2D_SpriteScalingForceRight_hk = safetyhook::create_mid((void*)0x4BE4BC, SpriteSpacingForceRight);
		DispTimeAttack2D_SpriteScalingDisable_hk = safetyhook::create_mid((void*)0x4BE4C1, SpriteSpacingDisable);
		DispTimeAttack2D_SpriteScalingForceLeft_hk = safetyhook::create_mid((void*)0x4BE4E7, SpriteSpacingForceLeft);
		DispTimeAttack2D_SpriteScalingForceEnable_hk = safetyhook::create_mid((void*)0x4BE575, SpriteSpacingEnable);

		DispTimeAttack2D_put_scroll_AdjustPosition_hk = safetyhook::create_mid((void*)0x4BE5CD, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk2 = safetyhook::create_mid((void*)0x4BE603, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk3 = safetyhook::create_mid((void*)0x4BE633, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk4 = safetyhook::create_mid((void*)0x4BE66D, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk5 = safetyhook::create_mid((void*)0x4BE690, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk6 = safetyhook::create_mid((void*)0x4BE6B5, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk7 = safetyhook::create_mid((void*)0x4BE6D5, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk8 = safetyhook::create_mid((void*)0x4BE8D8, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk9 = safetyhook::create_mid((void*)0x4BE915, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk10 = safetyhook::create_mid((void*)0x4BE94A, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk11 = safetyhook::create_mid((void*)0x4BE97A, put_scroll_AdjustPosition);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk12 = safetyhook::create_mid((void*)0x4BE9A3, put_scroll_AdjustPosition);

		DispRank_put_scroll_AdjustPosition_hk1 = safetyhook::create_mid((void*)0x4B9F3A, put_scroll_AdjustPosition);
		DispRank_put_scroll_AdjustPosition_hk2 = safetyhook::create_mid((void*)0x4B9F5E, put_scroll_AdjustPosition);
		DispRank_put_scroll_AdjustPosition_hk3 = safetyhook::create_mid((void*)0x4B9F81, put_scroll_AdjustPosition);
		DispRank_put_scroll_AdjustPosition_hk4 = safetyhook::create_mid((void*)0x4B9FD0, put_scroll_AdjustPosition);
		DispRank_put_scroll_AdjustPosition_hk5 = safetyhook::create_mid((void*)0x4B9FFC, put_scroll_AdjustPosition);
		DispRank_put_scroll_AdjustPosition_hk6 = safetyhook::create_mid((void*)0x4BA01E, put_scroll_AdjustPosition);
		DispRank_put_scroll_AdjustPosition_hk7 = safetyhook::create_mid((void*)0x4BA035, put_scroll_AdjustPosition);
		DispRank_put_scroll_AdjustPosition_hk8 = safetyhook::create_mid((void*)0x4BA052, put_scroll_AdjustPosition);

		// Fix ghost car info text positions
		PutGhostGapInfo_AdjustPosition_hk = safetyhook::create_mid((void*)0x4BDE3A, PutGhostGapInfo_AdjustPosition);
		DispGhostGap_ForceLeft_hk = safetyhook::create_mid((void*)0x4BE045, SpriteSpacingForceLeft);
		DispGhostGap_ForceLeft2_hk = safetyhook::create_mid((void*)0x4BE083, SpriteSpacingForceLeft);
		DispGhostGap_ForceRight_hk = safetyhook::create_mid((void*)0x4BE0A5, SpriteSpacingForceRight);

		PutGhostGapInfo_sub_AdjustPosition_hk = safetyhook::create_mid((void*)0x4BDAE8, PutGhostGapInfo_sub_AdjustPosition);

		NaviPub_DispTimeAttackGoal_DisableScaling_hk = safetyhook::create_mid((void*)0x4BEA64, SpriteSpacingDisable);

		// adjusts the girlfriend request speech bubble
		ctrl_icon_work_AdjustPosition_hk = safetyhook::create_mid((void*)0x460D40, ctrl_icon_work_AdjustPosition);
		ctrl_icon_work_AdjustPosition2_hk = safetyhook::create_mid((void*)0x460FBC, ctrl_icon_work_AdjustPosition2);
		set_icon_work_AdjustPosition_hk = safetyhook::create_mid((void*)0x460A21, ctrl_icon_work_AdjustPosition2); // set_icon_work can use same logic as ctrl_icon_work_AdjustPosition2

		// "-" text when negative heart score
		DispTempHeartNum_AdjustPosition_hk = safetyhook::create_mid((void*)0x4BBA89, DispTempHeartNum_AdjustPosition);

		return true;
	}
	static UIScaling instance;
};
UIScaling UIScaling::instance;
