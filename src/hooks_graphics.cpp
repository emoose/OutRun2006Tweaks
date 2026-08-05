#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <iostream>
#include <array>

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
		EVWORK_CAR* car = Game::pl_car();

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

// Restores the console sky glow, which PC port mostly included but was disabled
// in the code due to various issues.
//
// The effect blooms pixels whose framebuffer alpha beats a per stage baseline,
// alpha being an HDR exposure of 0 to 4.0 rather than opacity. vsSetMaterial
// turns a material's intensity into that exposure, clamp(((intensity + 1) * 0.25)
// * glow_param[1] * 255), so an ordinary material at intensity 0 gives 63, and
// g_GlowParamTable sets the baseline to match.
//
// Four (4!) separate things had to be fixed for this to work properly, plus two
// more for quality:
class RestoreSkyGlow : public Hook
{
	// (1) The exposure reaches pixel shader constant c7 alpha, but nothing reads
	// it. Games pixel shaders are assembled by concatenating ps_1_1 snippets looked
	// up by combiner encoding, and entry for the HDR alpha op holds an empty snippet 
	// on PC, so alpha keeps whatever the texture stages left: opaque scenery writes 255 
	// where it should write 63 and the whole stage blooms.
	//
	// A key names an NV2A sum, (b3 * b2) + (b1 * b0), where 0x10 is zero, 0x30 one,
	// 0x14 v0, 0x18 to 0x1B t0 to t3, 0x1C r0, and 0x11 and 0x12 the stage's two
	// constants. SetHDRPostProcessCombiner picks by the draw's alpha reference:
	// 0x1C301010 below 0x80, which is r0 alone and so leaves alpha untouched, and
	// 0x10101230 at 0x80 and above, the second constant on its own. Only the second
	// is the exposure write, so only its entry is redirected and draws that keep
	// their own alpha are unaffected.
	//
	// Its key2 of 0x4C00 carries the one flag nibble set anywhere in either table,
	// AB_CD_MUX, making the result (r0.a >= 0.5) ? exposure : 0 rather than the
	// exposure everywhere. Multiplying by the alpha already in the register gives
	// the same two outcomes for a cutout mask in one instruction.
	static constexpr int AlphaOpTable_ps11_Entry0Snippet = 0x21F248;
	static constexpr int AlphaOpTable_ps14_Entry0Snippet = 0x21F2B8;

	// Alpha shader snippet for ps_1_1
	static inline const uint32_t Snippet_ps11[] = {
		0xFFFF0101, // not copied
		0x00000005, // mul
		0x80080000, // r0.a
		0x80FF0000, // r0.a
		0xA0FF0007, // c7.a
		0x0000FFFF, // end
	};
	// Alpha shader snippet for ps_1_4
	static inline const uint32_t Snippet_ps14[] = {
		0xFFFF0104,
		0x00000005,
		0x80080004, // r4.a
		0x80FF0004, // r4.a
		0xA0FF0007,
		0x0000FFFF,
	};

	// (2) InitRender leaves the alpha test on with D3DCMP_GREATEREQUAL and the
	// opaque class carries a reference of 128. That works while alpha holds texture
	// alpha, opaque texels being 255, but no exposure can reach it, so every opaque
	// draw would fail and vanish.
	//
	// Only the copy psSetPixelShader reasserts is lowered. The 128 in
	// SetDefaultAlphaState is also what SetHDRPostProcessCombiner tests to choose
	// the alpha op, and below 0x80 it installs the no-op, reverting the effect.
	// ApplyAlphaRenderState sets the device reference and picks the op from that
	// same value, then psSetPixelShader runs per material and leaves 1 for the draw.
	static constexpr int AlphaRefCompare_Imm = 0xB027;
	static constexpr int AlphaRefReassert_Imm = 0xB039;

	// (3) D3D9 blends alpha with the colour factors, so the sky's exposure lands as
	// srcA*srcA + dstA*(1 - srcA) instead of srcA.
	//
	// Seeding the frame clear colour with that exposure, E = 0.25 * glow_param[0], 
	// makes that E*E + E*(1 - E), which is E, the blend's fixed point, so it comes 
	// out with right value without needing to override blend modes.
	//
	// Consoles seem to reach E by never clearing alpha and letting it settle across
	// frames, which the DISCARD swap chain used on PC can't do. E also suits pixels 
	// no sky covers: Sky_Disp draws with depth off and transparent scenery writes RGB 
	// only, so on Alaska, which has no skyId0 and shows black, a 0 clear would previously
	// leave a hard edge on surfaces that intersect empty sky parts.
	static constexpr int ClearBuffer_Addr = 0xEC70;
	static constexpr int BackColor_Addr = 0x49BD5C;
	static constexpr int GlowParam0_Addr = 0x4A8C18;

	// (4) Flag for enabling the glow effect - always set to 0 on PC.
	// (the launcher exe did include a setting for the glow that was hidden, devs
	// likely intended to add an option for it, but guess they ran out of time
	// to implement it fully - to be fair it took us over 2 years to fix too :P)
	static constexpr int GlowEnabled_Addr = 0x55AF09;

	// Addresses for glow texture sizes
	static constexpr int GlowInit_Addr = 0x14C00;
	static constexpr int GlowInit_WidthImm[] = { 0x14C22, 0x14C45, 0x14C69 };
	static constexpr int GlowInit_HeightImm[] = { 0x14C1D, 0x14C40, 0x14C64 };

	// Extract and blur passes both draw a quad spanning 0,0 to the target size,
	// so also have to be updated with the proper texture size.
	static constexpr int ExtractQuad_XY[] = { 0x2236CC, 0x2236EC, 0x223704, 0x223708 };
	static constexpr int BlurQuad_XY[] = { 0x223754, 0x22378C, 0x2237BC, 0x2237C0 };

	// (5) MakeReduceBuff copies the back buffer into the glow buffer with a single
	// StretchRect. Bilinear samples 2x2, so reducing by 4x reads only four of every
	// sixteen pixels, and building edges alias and crawl. Two 2x steps sample all of
	// them, bilinear being an exact box filter at exactly half, and the extra
	// averaging adds the softness the small buffer is meant to provide. Only worth
	// doing when the reduction is more than 2x.
	static constexpr int MakeReduceBuff_Addr = 0x14E50;
	static constexpr int GlowRelease_Addr = 0x14C80;
	static constexpr int RenderCfgGlow_Addr = 0x39FCD5; // tested as (x & 3) == 2
	static constexpr int Step1Tex_Addr = 0x4A8C14;
	static constexpr int LatestTex_Addr = 0x4A8C08;
	static constexpr int GlowAmount_Addr = 0x4A8C40; // glow_param.amount_28

	inline static IDirect3DTexture9* ReduceHalf = nullptr;

	inline static SafetyHookInline MakeReduceBuff_hook = {};
	static void __stdcall MakeReduceBuff_dest()
	{
		// Checked here rather than in apply so the setting can be changed while the
		// game runs.
		if (!Settings::SkyGlowTwoStep || !ReduceHalf)
		{
			MakeReduceBuff_hook.stdcall();
			return;
		}

		// The conditions the original reduces under.
		if ((*Module::exe_ptr<uint8_t>(RenderCfgGlow_Addr) & 3) != 2)
			return;
		if (*Module::exe_ptr<int>(GlowAmount_Addr) == 0)
			return;

		IDirect3DDevice9* device = Game::D3DDevice();
		IDirect3DTexture9* step1 = *Module::exe_ptr<IDirect3DTexture9*>(Step1Tex_Addr);
		if (!device || !step1)
			return;

		IDirect3DSurface9* back = nullptr;
		IDirect3DSurface9* half = nullptr;
		IDirect3DSurface9* target = nullptr;

		if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))
			&& SUCCEEDED(ReduceHalf->GetSurfaceLevel(0, &half))
			&& SUCCEEDED(step1->GetSurfaceLevel(0, &target)))
		{
			device->StretchRect(back, nullptr, half, nullptr, D3DTEXF_LINEAR);
			device->StretchRect(half, nullptr, target, nullptr, D3DTEXF_LINEAR);

			*Module::exe_ptr<IDirect3DTexture9*>(LatestTex_Addr) = step1;
		}

		if (target)
			target->Release();
		if (half)
			half->Release();
		if (back)
			back->Release();
	}

	// The intermediate is D3DPOOL_DEFAULT, so it has to be gone before a device
	// reset. D3D_GlowRelease_mb is where the game drops its own glow targets.
	inline static SafetyHookInline GlowRelease_hook = {};
	static void __stdcall GlowRelease_dest()
	{
		if (ReduceHalf)
		{
			ReduceHalf->Release();
			ReduceHalf = nullptr;
		}

		GlowRelease_hook.stdcall();
	}

	// (6) BlurGlowImage offsets its taps by `glow_param[11] / screen width` in texture
	// coordinates, growing by glow_param[12] each pass. Dividing by the live width
	// pins the blur to a fixed count of screen pixels, which covered most of the
	// picture at 640 across and is almost nothing at 3840: the offset stays 0.1
	// buffer texels either way, but the buffer is six times wider so the blur spans
	// six times less of the image, and four taps inside one texel leave bilinear
	// nothing to average.
	// Scaling both by width / 640 holds the texture coordinate offset constant, the 
	// proportion the console blurred at.
	static constexpr int BlurGlowImage_Addr = 0x15630;
	static constexpr int BlurOffset_Addr = 0x4A8C44;     // glow_param[11]
	static constexpr int BlurOffsetStep_Addr = 0x4A8C48; // glow_param[12]

	inline static SafetyHookInline BlurGlowImage_hook = {};
	static void __stdcall BlurGlowImage_dest()
	{
		float* offset = Module::exe_ptr<float>(BlurOffset_Addr);
		float* step = Module::exe_ptr<float>(BlurOffsetStep_Addr);

		const float scale = Game::screen_resolution->x / Game::original_resolution.x;
		const float prevOffset = *offset;
		const float prevStep = *step;

		*offset = prevOffset * scale;
		*step = prevStep * scale;

		BlurGlowImage_hook.stdcall();

		*offset = prevOffset;
		*step = prevStep;
	}

	inline static SafetyHookInline GlowInit_hook = {};
	static void __stdcall GlowInit_dest()
	{
		// Sized here rather than in apply because the render resolution is only
		// known once the game has read its config.
		int w = Game::screen_resolution->x / Settings::SkyGlowFactor;
		int h = Game::screen_resolution->y / Settings::SkyGlowFactor;

		// Floor at the size the console ran, so a small window still leaves the blur
		// room to work in.
		if (w < 160)
			w = 160;
		if (h < 120)
			h = 120;

		for (int addr : GlowInit_WidthImm)
			Memory::VP::Patch(Module::exe_ptr<uint32_t>(addr), uint32_t(w));
		for (int addr : GlowInit_HeightImm)
			Memory::VP::Patch(Module::exe_ptr<uint32_t>(addr), uint32_t(h));

		for (const int* quad : { ExtractQuad_XY, BlurQuad_XY })
		{
			Memory::VP::Patch(Module::exe_ptr<float>(quad[0]), float(w));
			Memory::VP::Patch(Module::exe_ptr<float>(quad[1]), float(h));
			Memory::VP::Patch(Module::exe_ptr<float>(quad[2]), float(w));
			Memory::VP::Patch(Module::exe_ptr<float>(quad[3]), float(h));
		}

		spdlog::info("RestoreSkyGlow: glow buffers sized {}x{}", w, h);

		GlowInit_hook.stdcall();

		// Halfway stage for the reduce, so neither StretchRect does more than a 2x.
		// This runs again after a device reset, so drop any previous one first.
		if (ReduceHalf)
		{
			ReduceHalf->Release();
			ReduceHalf = nullptr;
		}

		if (Settings::SkyGlowFactor > 2)
		{
			if (IDirect3DDevice9* device = Game::D3DDevice())
			{
				uint32_t halfW = uint32_t(Game::screen_resolution->x) / 2;
				uint32_t halfH = uint32_t(Game::screen_resolution->y) / 2;

				if (FAILED(device->CreateTexture(halfW, halfH, 1, D3DUSAGE_RENDERTARGET,
					D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ReduceHalf, nullptr)))
				{
					ReduceHalf = nullptr;
					spdlog::warn("RestoreSkyGlow: reduce intermediate failed, glow will alias at edges");
				}
			}
		}
	}

	inline static SafetyHookInline ClearBuffer_hook = {};
	static int __cdecl ClearBuffer_dest(int a1)
	{
		uint32_t* backColor = Module::exe_ptr<uint32_t>(BackColor_Addr);
		const uint32_t prevColor = *backColor;

		if (Game::is_in_game())
		{
			float exposure = 0.25f * *Module::exe_ptr<float>(GlowParam0_Addr);

			int alpha = int(exposure * 255.0f);
			if (alpha < 0)
				alpha = 0;
			if (alpha > 255)
				alpha = 255;

			*backColor = (prevColor & 0x00FFFFFF) | (uint32_t(alpha) << 24);
		}

		int result = ClearBuffer_hook.ccall<int>(a1);

		// Only the clear needs the exposure in there; leaving it set changes what
		// every later clear starts from, including the menus after a race.
		*backColor = prevColor;

		return result;
	}

public:
	std::string_view description() override
	{
		return "RestoreSkyGlow";
	}

	bool validate() override
	{
		return Settings::SkyGlowFactor > 0;
	}

	bool apply() override
	{
		Memory::VP::Patch(Module::exe_ptr<uint8_t>(GlowEnabled_Addr), uint8_t(1));

		Memory::VP::Patch(Module::exe_ptr(AlphaOpTable_ps11_Entry0Snippet), uintptr_t(Snippet_ps11));
		Memory::VP::Patch(Module::exe_ptr(AlphaOpTable_ps14_Entry0Snippet), uintptr_t(Snippet_ps14));

		for (int addr : { AlphaRefCompare_Imm, AlphaRefReassert_Imm })
			Memory::VP::Patch(Module::exe_ptr<uint32_t>(addr), uint32_t(1));

		GlowInit_hook = safetyhook::create_inline(Module::exe_ptr(GlowInit_Addr), GlowInit_dest);
		ClearBuffer_hook = safetyhook::create_inline(Module::exe_ptr(ClearBuffer_Addr), ClearBuffer_dest);
		MakeReduceBuff_hook = safetyhook::create_inline(Module::exe_ptr(MakeReduceBuff_Addr), MakeReduceBuff_dest);
		GlowRelease_hook = safetyhook::create_inline(Module::exe_ptr(GlowRelease_Addr), GlowRelease_dest);
		BlurGlowImage_hook = safetyhook::create_inline(Module::exe_ptr(BlurGlowImage_Addr), BlurGlowImage_dest);

		return !!GlowInit_hook && !!ClearBuffer_hook && !!MakeReduceBuff_hook
			&& !!GlowRelease_hook && !!BlurGlowImage_hook;
	}

	static RestoreSkyGlow instance;
};
RestoreSkyGlow RestoreSkyGlow::instance;

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
			if ((camera->cam_mode_34A == 2 || camera->cam_mode_34A == 0) // ... in third-person or FPV
				&& camera->cam_mode_timer_364 == 0 // ... not switching cameras
				&& (*Game::current_mode == STATE_GAME || *Game::current_mode == STATE_GOAL)) // ... we're in main game state (not in STATE_START cutscene etc)
			{
				camera->perspective_znear_BC = 1.0f;
			}
			else
			{
				if (camera->cam_mode_timer_364 != 0 || *Game::current_mode != STATE_GAME)
					camera->perspective_znear_BC = 0.1f; // set znear to 0.1 during camera switch / cutscene
				else
				{
					float in_car_view_znear = 0.25f; // 0.25 seems fine for in-car view, doesn't improve as much as 1.0f but still better than 0.1f
					auto* pl_car = Game::pl_car();
					if (pl_car)
					{
						if (pl_car->car_kind_11 == 7) // 360SP still shows gap with 0.25
							in_car_view_znear = 0.2f;
					}

					camera->perspective_znear_BC = in_car_view_znear;
				} 
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
		HWND window = HWND(ctx.ebp);
		SetWindowPos(window, 0,
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
