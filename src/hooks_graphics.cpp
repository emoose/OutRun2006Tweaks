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

class UIScaling : public Hook
{
	const static int D3DXMatrixTransformation2D_Addr = 0x39400;
	const static int draw_sprite_custom_matrix_mid_Addr = 0x2A556;
	const static int Calc3D2D_Addr = 0x49940;

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
			pTranslation->x = (pTranslation->x / Game::screen_scale->x) * scale;
			pTranslation->y = (pTranslation->y / Game::screen_scale->y) * scale;

			if (mode == ScalingMode::KeepCentered)
			{
				// Amount to add for centering
				float add = (Game::screen_resolution->x - (Game::original_resolution.x * scale)) / 2;
				pTranslation->x += add;
			}
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

		// Copies we can modify if needed
		D3DVECTOR TopLeft = a1->TopLeft_54;
		D3DVECTOR BottomLeft = a1->BottomLeft_60;
		D3DVECTOR TopRight = a1->TopRight_6C;
		D3DVECTOR BottomRight = a1->BottomRight_78;

		D3DXVECTOR4 topLeft{ 0 };
		D3DXVECTOR4 bottomLeft{ 0 };
		D3DXVECTOR4 topRight{ 0 };
		D3DXVECTOR4 bottomRight{ 0 };

		float scaleY = Game::screen_scale->y;

		// Multiply by the smallest scale factor
		if (mode == ScalingMode::KeepCentered)
			scaleY = min(Game::screen_scale->x, Game::screen_scale->y);

		// TopLeft
		vec.x = TopLeft.x;
		vec.y = TopLeft.y;
		vec.z = TopLeft.z;
		vec.w = 1.0;
		Game::D3DXVec4Transform(&topLeft, &vec, &pM);
		
		g_spriteVertexStream[1] = scaleY * topLeft.y + 0.5;
		g_spriteVertexStream[2] = topLeft.z;
		g_spriteVertexStream[3] = 1.0;
		g_spriteVertexStream[4] = a1->color_4;
		g_spriteVertexStream[5] = v21 + a1->top_9C;
		g_spriteVertexStream[6] = -v22 + a1->left_A0;

		// BottomLeft
		vec.x = BottomLeft.x;
		vec.y = BottomLeft.y;
		vec.z = BottomLeft.z;
		vec.w = 1.0;
		Game::D3DXVec4Transform(&bottomLeft, &vec, &pM);
		
		g_spriteVertexStream[8] = scaleY * bottomLeft.y + 0.5;
		g_spriteVertexStream[9] = bottomLeft.z;
		g_spriteVertexStream[0xA] = 1.0;
		g_spriteVertexStream[0xB] = a1->color_4;
		g_spriteVertexStream[0xC] = v21 + a1->top_94;
		g_spriteVertexStream[0xD] = v22 + a1->right_98;

		// TopRight
		vec.x = TopRight.x;
		vec.y = TopRight.y;
		vec.z = TopRight.z;
		vec.w = 1.0;
		Game::D3DXVec4Transform(&topRight, &vec, &pM);

		g_spriteVertexStream[0xF] = scaleY * topRight.y + 0.5;
		g_spriteVertexStream[0x10] = topRight.z;
		g_spriteVertexStream[0x11] = 1.0;
		g_spriteVertexStream[0x12] = a1->color_4;
		g_spriteVertexStream[0x13] = -v21 + a1->bottom_84;
		g_spriteVertexStream[0x14] = -v22 + a1->left_88;

		// BottomRight
		vec.x = BottomRight.x;
		vec.y = BottomRight.y;
		vec.z = BottomRight.z;
		vec.w = 1.0;
		Game::D3DXVec4Transform(&bottomRight, &vec, &pM);

		g_spriteVertexStream[0x16] = scaleY * bottomRight.y + 0.5;
		g_spriteVertexStream[0x17] = bottomRight.z;
		g_spriteVertexStream[0x18] = 1.0;
		g_spriteVertexStream[0x19] = a1->color_4;
		g_spriteVertexStream[0x1A] = -v21 + a1->bottom_8C;
		g_spriteVertexStream[0x1B] = v22 + a1->right_90;

		if (mode == ScalingMode::KeepCentered)
		{
			float add = (Game::screen_resolution->x - (Game::original_resolution.x * scaleY)) / 2;

			g_spriteVertexStream[0] = (scaleY * topLeft.x) + add;
			g_spriteVertexStream[7] = (scaleY * bottomLeft.x) + add;
			g_spriteVertexStream[0xE] = (scaleY * topRight.x) + add;
			g_spriteVertexStream[0x15] = (scaleY * bottomRight.x) + add;
		}
		else if (mode == ScalingMode::OnlineArcade)
		{
			g_spriteVertexStream[0] = (scaleY * topLeft.x);
			g_spriteVertexStream[7] = (scaleY * bottomLeft.x);
			g_spriteVertexStream[0xE] = (scaleY * topRight.x);
			g_spriteVertexStream[0x15] = (scaleY * bottomRight.x);
		}
		else // if (mode == Mode::Vanilla)
		{
			g_spriteVertexStream[0] = (Game::screen_scale->x * topLeft.x);
			g_spriteVertexStream[7] = (Game::screen_scale->x * bottomLeft.x);
			g_spriteVertexStream[0xE] = (Game::screen_scale->x * topRight.x);
			g_spriteVertexStream[0x15] = (Game::screen_scale->x * bottomRight.x);
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

		// TODO: there is another func draw_sprite_custom_matrix_multi which some sprites may use
		// it's a pretty long func though, luckily haven't seen anything that uses it so far

		return true;
	}
	static UIScaling instance;
};
UIScaling UIScaling::instance;
