#pragma once
#include <d3d9types.h>

typedef void (*fn_0args)();
typedef void (*fn_1arg)(int);
typedef int (*fn_1arg_int)(int);

enum GameState
{
	STATE_SYSTEM = 0x0,
	STATE_WARNING = 0x1,
	STATE_INFO = 0x2,
	STATE_ADVERTISE = 0x3,
	STATE_TITLE = 0x4,
	STATE_RATING = 0x5,
	STATE_ENTRY = 0x6,
	STATE_ADVEXIT = 0x7,
	STATE_ERROR = 0x8,
	STATE_MENU = 0x9,
	STATE_SELECTOR = 0xA,
	STATE_SELEXIT = 0xB,
	STATE_SELRESET = 0xC,
	STATE_START = 0xD,
	STATE_WARP = 0xE,
	STATE_RESTART = 0xF,
	STATE_GAME = 0x10,
	STATE_GIVEUP = 0x11,
	STATE_SMPAUSEMENU = 0x12,
	STATE_GOAL = 0x13,
	STATE_TIMEUP = 0x14,
	STATE_LINK_TIMEUP = 0x15,
	STATE_RESULT = 0x16,
	STATE_CONTINUE = 0x17,
	STATE_ENDING = 0x18,
	STATE_ROUTEMAP = 0x19,
	STATE_T_RANKING = 0x1A,
	STATE_GAMEOVER = 0x1B,
	STATE_GAMEEXIT = 0x1C,
	STATE_GAMERESET = 0x1D,
	STATE_SUMOGAMERESET = 0x1E,
	STATE_G_NAMEENTRY = 0x1F,
	STATE_SUMO_FE = 0x20,
	STATE_LIVEUPDATE = 0x21,
	STATE_OUTRUNMILES = 0x22,
	STATE_TRYAGAIN = 0x23,
	STATE_SUMOREWARD = 0x24,
};

enum SOUND_CMD
{
	/* 0x0800 */ SND_LOOP = (1 << 11),
	/* 0x1000 */ SND_PAN_LEFT = (1 << 12), 
	/* 0x2000 */ SND_PAN_RIGHT = (1 << 13),
	/* 0x4000 */ SND_PAN_LEFTRIGHT = (1 << 14),
	/* 0x8000 */ SND_STOP = (1 << 15),
};

typedef struct tagSphere
{
	float f0;
	float f1;
	float f2;
	float f3;
} Sphere;

typedef struct tgaEvWorkCamera // [sic]
{
	uint8_t unk_0[0xBC];
	float perspective_znear_BC;
	float perspective_zfar_C0;
	uint8_t unk_C4[0xC];
	uint8_t unk_D0[0x34A - 0xD0];
	char camera_mode_34A;
	uint8_t unk_34B[0x5];
	uint8_t unk_350[0x14];
	float camera_mode_timer_364;
	uint8_t unk_368[0x4C];
} EvWorkCamera;
static_assert(sizeof(EvWorkCamera) == 0x3B4);

struct OnRoadPlace
{
	DWORD loadColiType_0;
	uint32_t field_4;
	__int16 roadSectionNum_8;
	uint8_t field_A;
	uint8_t unk_B;
	DWORD curStageIdx_C;
};
static_assert(sizeof(OnRoadPlace) == 0x10);

typedef struct tagEVWORK_CAR
{
	uint8_t unk_0[0x5C];
	OnRoadPlace OnRoadPlace_5C;
	uint8_t unk_6C[0x10F0 - 0x6C];
} EVWORK_CAR;
static_assert(sizeof(EVWORK_CAR) == 0x10F0);
// car0 = 0x7804B0

typedef struct tagSPRARGS
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
} SPRARGS;
static_assert(sizeof(SPRARGS) == 0x48);

typedef struct D3DXVECTOR4 {
	FLOAT x;
	FLOAT y;
	FLOAT z;
	FLOAT w;
} D3DXVECTOR4, * LPD3DXVECTOR4;

typedef struct tagSPRARGS2
{
	uint32_t xstnum_0;
	float color_4;
	uint8_t unk_8[4];
	struct IDirect3DTexture9* d3dtexture_ptr_C; // IDirect3DTexture9* ?
	uint32_t unk_10;
	D3DMATRIX mtx_14;
	D3DVECTOR field_54;
	D3DVECTOR field_60;
	D3DVECTOR field_6C;
	D3DVECTOR field_78;
	float bottom_84;
	float left_88;
	float bottom_8C;
	float right_90;
	float top_94;
	float right_98;
	float top_9C;
	float left_A0;
	float unk_A4;
	float unk_A8;
	uint32_t unk_AC;
	uint32_t unk_B0;
	uint32_t ptr_B4;
} SPRARGS2;
static_assert(sizeof(SPRARGS2) == 0xB8);

inline void WaitForDebugger()
{
#ifdef _DEBUG
	while (!IsDebuggerPresent())
	{
	}
#endif
}
