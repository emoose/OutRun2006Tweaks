#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

enum class ScalingMode
{
	Vanilla,
	OnlineArcade,
	KeepCentered,
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

			// Space out the UI elements if they're on the sides of the screen
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
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk13{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk14{};
	static inline SafetyHookMid DispTimeAttack2D_put_scroll_AdjustPosition_hk15{};

	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk1{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk2{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk3{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk4{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk5{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk6{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk7{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk8{};
	static inline SafetyHookMid DispRank_put_scroll_AdjustPosition_hk9{};

	static inline SafetyHookMid DispGearPosition_put_scroll_AdjustPosition_hk1{};
	static inline SafetyHookMid DispGearPosition_put_scroll_AdjustPosition_hk2{};
	static inline SafetyHookMid DispGearPosition_put_scroll_AdjustPosition_hk3{};

	template <typename T>
	static void AddSpriteSpacing(T* value, bool left)
	{
		ScalingMode mode = ScalingMode(Settings::UIScalingMode);
		if (mode != ScalingMode::OnlineArcade)
			return;

		float spacing = -((Game::screen_scale->y * Game::original_resolution.x) - Game::screen_resolution->x) / 2;
		spacing = spacing / Game::screen_scale->x;

		if(left)
			*value = T(float(*value) - spacing);
		else
			*value = T(float(*value) + spacing);
	}

	static void put_scroll_AdjustPositionRight(safetyhook::Context& ctx)
	{
		AddSpriteSpacing((int*)(ctx.esp + 4), false);
	}
	static void put_scroll_AdjustPositionLeft(safetyhook::Context& ctx)
	{
		AddSpriteSpacing((int*)(ctx.esp + 4), true);
	}

	// PutGhostGapInfo
	static inline SafetyHookMid PutGhostGapInfo_AdjustPosition_hk{};
	static void PutGhostGapInfo_AdjustPosition(safetyhook::Context& ctx)
	{
		bool left = false;
		int* val = (int*)&ctx.ebp;
		if (*val == 102)
			left = true;

		AddSpriteSpacing(val, left);
	};

	// Fix position of the "Ghost/You/Diff" sprites shown with ghost car info
	// Online arcade doesn't seem to adjust this, maybe was left broken in that? (it's needed for 21:9 at least...)
	static inline SafetyHookMid PutGhostGapInfo_sub_AdjustPosition_hk{};
	static void PutGhostGapInfo_sub_AdjustPosition(safetyhook::Context& ctx)
	{
		bool left = false;
		float* val = &ctx.xmm0.f32[0];
		if (*val == 4.0f)
			left = true;

		AddSpriteSpacing(val, left);
	}

	// DispGhostGap
	static inline SafetyHookMid DispGhostGap_ForceLeft_hk{};
	static inline SafetyHookMid DispGhostGap_ForceLeft2_hk{};
	static inline SafetyHookMid DispGhostGap_ForceRight_hk{};
	static inline SafetyHookMid DispGhostGap_ForceRight2_hk{};

	// NaviPub_DispTimeAttackGoal
	static inline SafetyHookMid NaviPub_DispTimeAttackGoal_DisableScaling_hk{};

	// Heart totals
	static inline SafetyHookMid NaviPub_Disp_HeartDisableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_HeartEnableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_C2CHeartDisableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_C2CHeartEnableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_C2CHeartEnableScaling2_hk{};

	static inline SafetyHookMid NaviPub_Disp_C2CFruitDisableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_C2CFruitEnableScaling_hk{};

	static inline SafetyHookMid NaviPub_Disp_RivalDisableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_RivalEnableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_RivalOnlineDisableScaling_hk{};
	static inline SafetyHookMid NaviPub_Disp_RivalOnlineEnableScaling_hk{};

	static inline SafetyHookMid ctrl_icon_work_AdjustPosition_hk{};
	static void ctrl_icon_work_AdjustPosition(safetyhook::Context& ctx)
	{
		AddSpriteSpacing(&ctx.xmm0.f32[0], false);
	}

	static inline SafetyHookMid ctrl_icon_work_AdjustPosition2_hk{};
	static inline SafetyHookMid set_icon_work_AdjustPosition_hk{};
	static void ctrl_icon_work_AdjustPosition2(safetyhook::Context& ctx)
	{
		AddSpriteSpacing(&ctx.xmm0.f32[0], false);

		*(float*)(ctx.esp) = ctx.xmm0.f32[0];
	}

	static inline SafetyHookMid DispTempHeartNum_AdjustPosition_hk{};
	static void DispTempHeartNum_AdjustPosition(safetyhook::Context& ctx)
	{
		AddSpriteSpacing((int*)(ctx.esp), false);
	}

	static inline SafetyHookMid C2CSpeechBubble_AdjustPositionESP0_hk1{};
	static inline SafetyHookMid C2CSpeechBubble_AdjustPositionESP0_hk2{};
	static inline SafetyHookMid C2CSpeechBubble_AdjustPositionESP0_hk3{};
	static inline SafetyHookMid C2CSpeechBubble_AdjustPositionESP0_hk4{};
	static inline SafetyHookMid C2CSpeechBubble_AdjustPositionESP0_hk5{};
	static inline SafetyHookMid C2CSpeechBubble_AdjustPositionESP0_hk6{};
	static inline SafetyHookMid C2CSpeechBubble_AdjustPositionESP0_hk7{};

	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk1{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk2{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk3{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk4{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk5{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk6{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk7{};
	static inline SafetyHookMid C2CSpeechBubbleGFHeart_AdjustPositionESP0_hk1{};
	static inline SafetyHookMid C2CSpeechBubbleGFHeart_AdjustPositionESP0_hk2{};
	static inline SafetyHookMid C2CSpeechBubbleGFHeart_AdjustPositionESP0_hk3{};
	static inline SafetyHookMid C2CSpeechBubbleGFHeart_AdjustPositionESP0_hk4{};

	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk8{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk9{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk10{};

	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk11{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk12{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk13{};
	static inline SafetyHookMid C2CSpeechBubbleGF_AdjustPositionESP0_hk14{};

	static inline SafetyHookMid C2CDontLoseGF_AdjustPosition_hk1{};
	static inline SafetyHookMid C2CDontLoseGF_AdjustPosition_hk2{};
	static inline SafetyHookMid C2CDontLoseGF_AdjustPosition_hk3{};

	static void C2CSpeechBubble_AdjustPositionESP0(safetyhook::Context& ctx)
	{
		AddSpriteSpacing((float*)(ctx.esp), false);
	}

	static void C2CSpeechBubble_AdjustPositionESP4(safetyhook::Context& ctx)
	{
		AddSpriteSpacing((float*)(ctx.esp + 4), false);
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

		NaviPub_Disp_C2CFruitDisableScaling_hk = safetyhook::create_mid((void*)0x481A86, SpriteSpacingDisable);
		NaviPub_Disp_C2CFruitEnableScaling_hk = safetyhook::create_mid((void*)0x481A8B, SpriteSpacingEnable);

		NaviPub_Disp_RivalDisableScaling_hk = safetyhook::create_mid((void*)0x4BEB8E, SpriteSpacingDisable);
		NaviPub_Disp_RivalEnableScaling_hk = safetyhook::create_mid((void*)0x4BEBAF, SpriteSpacingEnable);
		NaviPub_Disp_RivalOnlineDisableScaling_hk = safetyhook::create_mid((void*)0x4BEC83, SpriteSpacingDisable);
		NaviPub_Disp_RivalOnlineEnableScaling_hk = safetyhook::create_mid((void*)0x4BEC88, SpriteSpacingEnable);

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

		DispTimeAttack2D_put_scroll_AdjustPosition_hk = safetyhook::create_mid((void*)0x4BE5CD, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk2 = safetyhook::create_mid((void*)0x4BE603, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk3 = safetyhook::create_mid((void*)0x4BE633, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk4 = safetyhook::create_mid((void*)0x4BE66D, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk5 = safetyhook::create_mid((void*)0x4BE690, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk6 = safetyhook::create_mid((void*)0x4BE6B5, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk7 = safetyhook::create_mid((void*)0x4BE6D5, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk8 = safetyhook::create_mid((void*)0x4BE8D8, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk9 = safetyhook::create_mid((void*)0x4BE915, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk10 = safetyhook::create_mid((void*)0x4BE94A, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk11 = safetyhook::create_mid((void*)0x4BE97A, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk12 = safetyhook::create_mid((void*)0x4BE9A3, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk13 = safetyhook::create_mid((void*)0x4BE7E8, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk14 = safetyhook::create_mid((void*)0x4BE802, put_scroll_AdjustPositionRight);
		DispTimeAttack2D_put_scroll_AdjustPosition_hk15 = safetyhook::create_mid((void*)0x4BE81C, put_scroll_AdjustPositionRight);

		DispRank_put_scroll_AdjustPosition_hk1 = safetyhook::create_mid((void*)0x4B9F3A, put_scroll_AdjustPositionRight);
		DispRank_put_scroll_AdjustPosition_hk2 = safetyhook::create_mid((void*)0x4B9F5E, put_scroll_AdjustPositionRight);
		DispRank_put_scroll_AdjustPosition_hk3 = safetyhook::create_mid((void*)0x4B9F81, put_scroll_AdjustPositionRight);
		DispRank_put_scroll_AdjustPosition_hk4 = safetyhook::create_mid((void*)0x4B9FD0, put_scroll_AdjustPositionRight);
		DispRank_put_scroll_AdjustPosition_hk5 = safetyhook::create_mid((void*)0x4B9FFC, put_scroll_AdjustPositionRight);
		DispRank_put_scroll_AdjustPosition_hk6 = safetyhook::create_mid((void*)0x4BA01E, put_scroll_AdjustPositionRight);
		DispRank_put_scroll_AdjustPosition_hk7 = safetyhook::create_mid((void*)0x4BA035, put_scroll_AdjustPositionRight);
		DispRank_put_scroll_AdjustPosition_hk8 = safetyhook::create_mid((void*)0x4BA052, put_scroll_AdjustPositionRight);

		// REV indicator
		DispGearPosition_put_scroll_AdjustPosition_hk1 = safetyhook::create_mid((void*)0x4B9096, put_scroll_AdjustPositionLeft);
		DispGearPosition_put_scroll_AdjustPosition_hk2 = safetyhook::create_mid((void*)0x4B90B3, put_scroll_AdjustPositionLeft);
		DispGearPosition_put_scroll_AdjustPosition_hk3 = safetyhook::create_mid((void*)0x4B90F6, put_scroll_AdjustPositionLeft);

		// Fix ghost car info text positions
		PutGhostGapInfo_AdjustPosition_hk = safetyhook::create_mid((void*)0x4BDE3A, PutGhostGapInfo_AdjustPosition);
		DispGhostGap_ForceLeft_hk = safetyhook::create_mid((void*)0x4BE045, SpriteSpacingForceLeft);
		DispGhostGap_ForceLeft2_hk = safetyhook::create_mid((void*)0x4BE083, SpriteSpacingForceLeft);
		DispGhostGap_ForceRight_hk = safetyhook::create_mid((void*)0x4BE0A5, SpriteSpacingForceRight);
		DispGhostGap_ForceRight2_hk = safetyhook::create_mid((void*)0x4BE067, SpriteSpacingForceRight);

		PutGhostGapInfo_sub_AdjustPosition_hk = safetyhook::create_mid((void*)0x4BDAE8, PutGhostGapInfo_sub_AdjustPosition);

		NaviPub_DispTimeAttackGoal_DisableScaling_hk = safetyhook::create_mid((void*)0x4BEA64, SpriteSpacingDisable);

		// adjusts the girlfriend request speech bubble
		ctrl_icon_work_AdjustPosition_hk = safetyhook::create_mid((void*)0x460D40, ctrl_icon_work_AdjustPosition);
		ctrl_icon_work_AdjustPosition2_hk = safetyhook::create_mid((void*)0x460FBC, ctrl_icon_work_AdjustPosition2);
		set_icon_work_AdjustPosition_hk = safetyhook::create_mid((void*)0x460A21, ctrl_icon_work_AdjustPosition2); // set_icon_work can use same logic as ctrl_icon_work_AdjustPosition2

		// "-" text when negative heart score
		DispTempHeartNum_AdjustPosition_hk = safetyhook::create_mid((void*)0x4BBA89, DispTempHeartNum_AdjustPosition);

		// C2C-specific speech bubbles
		C2CSpeechBubble_AdjustPositionESP0_hk1 = safetyhook::create_mid((void*)0x496AC7, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubble_AdjustPositionESP0_hk2 = safetyhook::create_mid((void*)0x496B14, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubble_AdjustPositionESP0_hk3 = safetyhook::create_mid((void*)0x496B39, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubble_AdjustPositionESP0_hk4 = safetyhook::create_mid((void*)0x496B94, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubble_AdjustPositionESP0_hk5 = safetyhook::create_mid((void*)0x496BE1, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubble_AdjustPositionESP0_hk6 = safetyhook::create_mid((void*)0x496C10, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubble_AdjustPositionESP0_hk7 = safetyhook::create_mid((void*)0x496C6A, C2CSpeechBubble_AdjustPositionESP0);

		C2CSpeechBubbleGF_AdjustPositionESP0_hk1 = safetyhook::create_mid((void*)0x4FCDC1, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGF_AdjustPositionESP0_hk2 = safetyhook::create_mid((void*)0x4FCDEA, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGF_AdjustPositionESP0_hk3 = safetyhook::create_mid((void*)0x4FCEB0, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGF_AdjustPositionESP0_hk4 = safetyhook::create_mid((void*)0x4FCED9, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGF_AdjustPositionESP0_hk5 = safetyhook::create_mid((void*)0x4FCF22, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGF_AdjustPositionESP0_hk6 = safetyhook::create_mid((void*)0x4FCF4F, C2CSpeechBubble_AdjustPositionESP0);

		// unsure if this has any effect...
		C2CSpeechBubbleGF_AdjustPositionESP0_hk7 = safetyhook::create_mid((void*)0x4FE8B1, C2CSpeechBubble_AdjustPositionESP0);

		// 4FD1A9 seemed related but no effect?
		C2CSpeechBubbleGFHeart_AdjustPositionESP0_hk1 = safetyhook::create_mid((void*)0x4FD60C, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGFHeart_AdjustPositionESP0_hk2 = safetyhook::create_mid((void*)0x4FD591, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGFHeart_AdjustPositionESP0_hk3 = safetyhook::create_mid((void*)0x4FD5CD, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGFHeart_AdjustPositionESP0_hk4 = safetyhook::create_mid((void*)0x4FD652, C2CSpeechBubble_AdjustPositionESP0);

		// ranking emoji position
		C2CSpeechBubbleGF_AdjustPositionESP0_hk8 = safetyhook::create_mid((void*)0x4FC84E, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGF_AdjustPositionESP0_hk9 = safetyhook::create_mid((void*)0x4FC882, C2CSpeechBubble_AdjustPositionESP0);

		// "rank: aaa" position
		C2CSpeechBubbleGF_AdjustPositionESP0_hk10 = safetyhook::create_mid((void*)0x4FC8B4, C2CSpeechBubble_AdjustPositionESP0);

		// speech bubble initial position
		C2CSpeechBubbleGF_AdjustPositionESP0_hk11 = safetyhook::create_mid((void*)0x4FC9EB, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGF_AdjustPositionESP0_hk12 = safetyhook::create_mid((void*)0x4FCA1E, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGF_AdjustPositionESP0_hk13 = safetyhook::create_mid((void*)0x4FCA51, C2CSpeechBubble_AdjustPositionESP0);
		C2CSpeechBubbleGF_AdjustPositionESP0_hk14 = safetyhook::create_mid((void*)0x4FCB20, C2CSpeechBubble_AdjustPositionESP4);

		// "don't lose your girlfriend" UI sprites
		C2CDontLoseGF_AdjustPosition_hk1 = safetyhook::create_mid((void*)0x4BD397, put_scroll_AdjustPositionRight);
		C2CDontLoseGF_AdjustPosition_hk2 = safetyhook::create_mid((void*)0x4BD414, put_scroll_AdjustPositionRight);
		C2CDontLoseGF_AdjustPosition_hk3 = safetyhook::create_mid((void*)0x4BD472, put_scroll_AdjustPositionRight);

		return true;
	}
	static UIScaling instance;
};
UIScaling UIScaling::instance;
