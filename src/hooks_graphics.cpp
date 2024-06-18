#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <fstream>
#include "d3d9.h"
#include "ddraw.h"
#include <xxhash.h>

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

		float ratio_screen = *Game::screen_width / *Game::screen_height;

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
			*Game::screen_width, *Game::screen_height,
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

#define DDS_MAGIC 0x20534444  // "DDS "
struct DDS_FILE
{
	DWORD magic;
	DDSURFACEDESC2 data;
};


class SupaTextureDumper9000 : public Hook
{
	const static int D3DXCreateTextureFromFileInMemory_Addr = 0x39412;

	inline static SafetyHookInline D3DXCreateTextureFromFileInMemory = {};

	inline static std::filesystem::path CurrentSpriteXmt;

	inline static std::filesystem::path XmtDumpPath;
	inline static std::filesystem::path XmtLoadPath;

	inline static uint8_t* xstset = 0;
	inline static int xstset_index = 0;

	inline static std::unordered_map<int, std::pair<float, float>> sprite_scales;

	static void RescaleSpriteClipArgs(const DDS_FILE* ddshead, DDS_FILE* newhead, float& scaleX, float& scaleY)
	{
		float ratio_width = float(newhead->data.dwWidth) / float(ddshead->data.dwWidth);
		float ratio_height = float(newhead->data.dwHeight) / float(ddshead->data.dwHeight);
		scaleX = ratio_width;
		scaleY = ratio_height;

		struct ClipArgsMb
		{
			uint32_t unk_0;
			float field_4;
			float field_8;
			float field_C;
			float field_10;
			uint16_t field_14;
			uint16_t field_16;
			uint16_t field_18;
			uint16_t field_1A;
		};

		uint8_t* clipargs = *(uint8_t**)(xstset + 4);
		int numclips = *(int*)(clipargs + 0x18);
		ClipArgsMb* clips = *(ClipArgsMb**)(clipargs + 0x1C);

		int clipnum = *(int*)0x95B25C;
		sprite_scales[(xstset_index << 16) | clipnum] = { ratio_width, ratio_height };
	};

	struct SPRARGS
	{
		uint32_t xstnum_0;
		uint32_t top_4;
		uint32_t left_8;
		uint32_t bottom_C;
		uint32_t right_10;
		float scaleX;
		float scaleY;
		float float1C;
		float float20;
		float float24;
		float float28;
		uint32_t dword2C;
		float float30;
		uint32_t dword34;
		uint32_t* pdword38;
		uint8_t unk_3C[12];
	};

	inline static SafetyHookInline put_sprite_ex = {};
	static int __cdecl put_sprite_ex_dest(SPRARGS* a1, float a2)
	{
		int xstnum = a1->xstnum_0;
		if (sprite_scales.contains(xstnum))
		{
			auto scaleX = std::get<0>(sprite_scales[xstnum]);
			auto scaleY = std::get<1>(sprite_scales[xstnum]);
			a1->top_4 = a1->top_4 * scaleY;
			a1->left_8 = a1->left_8 * scaleX;
			a1->bottom_C = a1->bottom_C * scaleY;
			a1->right_10 = a1->right_10 * scaleX;
			a1->scaleX = a1->scaleX / scaleX;
			a1->scaleY = a1->scaleY / scaleY;
		}
		return put_sprite_ex.call<int>(a1, a2);
	}

	struct SPRARGS2
	{
		uint32_t xstnum_0;
		uint8_t unk_4[8];
		uintptr_t xstset_ptr_C;
		uint8_t unk_10[0x50];
		float unk_60;
		float unk_64;
		float unk_68;
		float unk_6C;
		float unk_70;
		float unk_74;
		float unk_78;
		float unk_7C;
		float unk_80;
		float unk_84;
		float unk_88;
		float unk_8C;
		float unk_90;
		float unk_94;
		float unk_98;
		float unk_9C;
		float unk_A0;
	};
	static_assert(sizeof(SPRARGS2) == 0xA4, "");

	// TODO:
	// Right now we're using a map with the IDirect3DTexture9 pointer as index, since SPRARGS2 fortunately includes pointer to it
	// (SPRARGS2 does contain a xstnum_0 field, but it doesn't always seem used...)
	//
	// It's likely that as gameplay progresses these pointers will become invalid, and may end up pointing to entirely different texture...
	// 
	// Seems game uses get_texture(textureId) to fetch the pointer first, and the textureId param should be able to index into our sprite_scales map
	// So maybe better to just do something like
	// get_texture hook -> if textureId exists inside sprite_scales, stash the IDirect3DTexture9* pointer somewhere
	// put_sprite_ex2 hook -> check IDirect3DTexture9*, if it matches up apply the scale, then invalidate pointer
	// 
	inline static std::unordered_map<uintptr_t, std::pair<float, float>> sprite_offset_scales;

	inline static SafetyHookInline put_sprite_ex2 = {};
	static int __cdecl put_sprite_ex2_dest(SPRARGS2* a1, float a2)
	{
		int xstnum = a1->xstnum_0;
		if (sprite_offset_scales.contains(a1->xstset_ptr_C))
		{
			auto scaleX = std::get<0>(sprite_offset_scales[a1->xstset_ptr_C]);
			auto scaleY = std::get<1>(sprite_offset_scales[a1->xstset_ptr_C]);

			float origLeft = 1.0f - a1->unk_88;
			origLeft = origLeft * scaleX;
			a1->unk_88 = 1.0f - origLeft;
			a1->unk_A0 = a1->unk_88;

			float origRight = 1.0f - a1->unk_90;
			origRight = origRight * scaleX;
			a1->unk_90 = 1.0f - origRight;
			a1->unk_98 = a1->unk_90;

			// bottom?
			a1->unk_84 = a1->unk_84 * scaleY;
			a1->unk_8C = a1->unk_8C * scaleY;

			// top?
			a1->unk_94 = a1->unk_94 * scaleY;
			a1->unk_9C = a1->unk_9C * scaleY;
		}
		return put_sprite_ex2.call<int>(a1, a2);
	}

	inline static SafetyHookMid LoadSpriteTextures_hook = {};
	static void LoadSpriteTextures_dest(safetyhook::Context& ctx)
	{
		xstset = (uint8_t*)(ctx.esi);
	}

	inline static SafetyHookMid LoadXstsetSprite_hook = {};
	static void LoadXstsetSprite_dest(safetyhook::Context& ctx)
	{
		CurrentSpriteXmt = (char*)ctx.ecx;
		xstset_index = (int)(ctx.eax);
	};

	static HRESULT __stdcall D3DXCreateTextureFromFileInMemory_dest(LPDIRECT3DDEVICE9 pDevice, void* pSrcData, UINT SrcDataSize, LPDIRECT3DTEXTURE9* ppTexture)
	{
		std::unique_ptr<uint8_t[]> newdata;

		bool scaled = false;
		float scaleX = 1;
		float scaleY = 1;

		if (pSrcData && SrcDataSize)
		{
			const DDS_FILE* header = (const DDS_FILE*)pSrcData;
			if (header->magic == DDS_MAGIC)
			{
				int width = header->data.dwWidth;
				int height = header->data.dwHeight;

				auto hash = XXH32(pSrcData, SrcDataSize, 0);

				std::string ddsName = std::format("{:X}_{}x{}.dds", hash, width, height);
				auto path_dump = XmtDumpPath / CurrentSpriteXmt.filename().stem();
				if (!std::filesystem::exists(path_dump))
					std::filesystem::create_directories(path_dump);
				path_dump = path_dump / ddsName;

				auto path_load = XmtLoadPath / CurrentSpriteXmt.filename().stem() / ddsName;
				if (!std::filesystem::exists(path_load))
					path_load = XmtLoadPath / ddsName;

				if (std::filesystem::exists(path_load))
				{
					FILE* file = fopen(path_load.string().c_str(), "rb");
					fseek(file, 0, SEEK_END);
					long size = ftell(file);
					fseek(file, 0, SEEK_SET);
					newdata = std::make_unique<uint8_t[]>(size);
					fread(newdata.get(), 1, size, file);
					fclose(file);

					scaled = true;
					RescaleSpriteClipArgs(header, (DDS_FILE*)newdata.get(), scaleX, scaleY);
					memcpy(pSrcData, newdata.get(), sizeof(DDS_FILE));

					pSrcData = newdata.get();
					SrcDataSize = size;
				}
				else
				{
					if (!std::filesystem::exists(path_dump))
					{
						FILE* file = fopen(path_dump.string().c_str(), "wb");
						fwrite(pSrcData, 1, SrcDataSize, file);
						fclose(file);
					}
				}
			}
		}

		auto ret = D3DXCreateTextureFromFileInMemory.stdcall<HRESULT>(pDevice, pSrcData, SrcDataSize, ppTexture);
		if (scaled && *ppTexture)
			sprite_offset_scales[(uintptr_t)*ppTexture] = { scaleX, scaleY };
		return ret;
	}

public:
	std::string_view description() override
	{
		return "SupaTextureDumper9000";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		XmtDumpPath = "textures\\dump";
		XmtLoadPath = "textures\\load";

		D3DXCreateTextureFromFileInMemory = safetyhook::create_inline(Module::exe_ptr(D3DXCreateTextureFromFileInMemory_Addr), D3DXCreateTextureFromFileInMemory_dest);

		LoadSpriteTextures_hook = safetyhook::create_mid(Module::exe_ptr(0x30120), LoadSpriteTextures_dest);
		LoadXstsetSprite_hook = safetyhook::create_mid(Module::exe_ptr(0x2FE20), LoadXstsetSprite_dest);
		
		put_sprite_ex = safetyhook::create_inline(Module::exe_ptr(0x2CFE0), put_sprite_ex_dest);
		put_sprite_ex2 = safetyhook::create_inline(Module::exe_ptr(0x2D0C0), put_sprite_ex2_dest);

		return true;
	}

	static SupaTextureDumper9000 instance;
};
SupaTextureDumper9000 SupaTextureDumper9000::instance;
