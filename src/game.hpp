#pragma once
#include <d3d9types.h>
#include <Xinput.h>

#define D3DX_DEFAULT ULONG_MAX

typedef void (*fn_0args)();
typedef void* (*fn_0args_void)();
typedef void (*fn_1arg)(int);
typedef void (*fn_2args)(int, int);
typedef void (*fn_3args)(int, int, int);
typedef int (*fn_1arg_int)(int);
typedef int (__stdcall *fn_stdcall_1arg_int)(int);
typedef void* (*fn_1arg_void)(int);
typedef char* (*fn_1arg_char)(int);
typedef void (*fn_2floats)(float, float);
typedef void (*fn_printf)(const char*, ...);
typedef void (__fastcall *fn_0args_class)(void* thisptr, void* unused);
typedef void (__fastcall *fn_1args_class)(void* thisptr, void* unused, void* a1);

#define XINPUT_DIGITAL_LEFT_TRIGGER   0x10000
#define XINPUT_DIGITAL_RIGHT_TRIGGER  0x20000
#define XINPUT_DIGITAL_LS_UP         0x100000
#define XINPUT_DIGITAL_LS_DOWN       0x200000
#define XINPUT_DIGITAL_LS_LEFT       0x400000
#define XINPUT_DIGITAL_LS_RIGHT      0x800000
#define XINPUT_DIGITAL_RS_UP        0x1000000
#define XINPUT_DIGITAL_RS_DOWN      0x2000000
#define XINPUT_DIGITAL_RS_LEFT      0x4000000
#define XINPUT_DIGITAL_RS_RIGHT     0x8000000

inline static const std::unordered_map<std::string, uint32_t> XInputButtonMap = {
	{"up", XINPUT_GAMEPAD_DPAD_UP},
	{"down", XINPUT_GAMEPAD_DPAD_DOWN},
	{"left", XINPUT_GAMEPAD_DPAD_LEFT},
	{"right", XINPUT_GAMEPAD_DPAD_RIGHT},

	{"start", XINPUT_GAMEPAD_START},
	{"back", XINPUT_GAMEPAD_BACK},

	{"ls", XINPUT_GAMEPAD_LEFT_THUMB},
	{"rs", XINPUT_GAMEPAD_RIGHT_THUMB},

	{"lb", XINPUT_GAMEPAD_LEFT_SHOULDER},
	{"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},

	{"a", XINPUT_GAMEPAD_A},
	{"b", XINPUT_GAMEPAD_B},
	{"x", XINPUT_GAMEPAD_X},
	{"y", XINPUT_GAMEPAD_Y},

	{"lt", XINPUT_DIGITAL_LEFT_TRIGGER},
	{"rt", XINPUT_DIGITAL_RIGHT_TRIGGER},

	{"ls-up", XINPUT_DIGITAL_LS_UP},
	{"ls-down", XINPUT_DIGITAL_LS_DOWN},
	{"ls-left", XINPUT_DIGITAL_LS_LEFT},
	{"ls-right", XINPUT_DIGITAL_LS_RIGHT},

	{"rs-up", XINPUT_DIGITAL_RS_UP},
	{"rs-down", XINPUT_DIGITAL_RS_DOWN},
	{"rs-left", XINPUT_DIGITAL_RS_LEFT},
	{"rs-right", XINPUT_DIGITAL_RS_RIGHT},
};

namespace Input
{
	inline XINPUT_STATE PadStatePrev{ 0 };
	inline uint32_t PadDigitalPrev{ 0 };
	inline XINPUT_STATE PadStateCur{ 0 };
	inline uint32_t PadDigitalCur{ 0 };

	inline bool PadReleased(uint32_t buttons)
	{
		return (PadDigitalPrev & buttons) == buttons &&
			(PadDigitalCur & buttons) != buttons;
	}

	// hooks_input.cpp
	void Update();
};

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

enum ChrSet
{
	CHR_AUT01 = 0,
	CHR_AUT02 = 1,
	CHR_AUT03 = 2,
	CHR_AUT04 = 3,
	CHR_AUT04_CVT = 4,
	CHR_DR_G00 = 5,
	CHR_DR_G00_USA = 6,
	CHR_DR_GH00 = 7,
	CHR_DR_GH00_USA = 8,
	CHR_DR_L00 = 9,
	CHR_DR_LH00 = 10,
	CHR_DR_M00 = 11,
	CHR_DR_MH00 = 12,
	CHR_FAL = 13,
	CHR_GAL = 14,
	CHR_GAL_USA = 15,
	CHR_MAL = 16,
	CHR_DR_W00 = 17,
	CHR_DR_S00 = 18,
	CHR_DR_H00 = 19
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
	uint8_t unk_6C[4];
	D3DMATRIX matrix_70;
	D3DMATRIX matrix_B0;
	D3DMATRIX matrix_F0;
	uint8_t unk_130[0x10F0 - 0x130];
} EVWORK_CAR;
static_assert(sizeof(EVWORK_CAR) == 0x10F0);
// car0 = 0x7804B0

struct EvFunc
{
	void(__stdcall* Init)(void*);
	void(__stdcall* Ctrl)(void*);
	void(__stdcall* Disp)(void*);
	void(__stdcall* Shadow)(void*);
	void(__stdcall* Dest)(void*);
};

struct sEventWork
{
	uint32_t field_0;
	uint32_t field_4;
	uint32_t event_data_8;
	uint32_t field_C;
	EvFunc EventFunc;
	uint8_t unk_24[24];

	template <typename T>
	inline T* data()
	{
		return (T*)event_data_8;
	}
};
static_assert(sizeof(sEventWork) == 0x3C);

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

typedef struct D3DXVECTOR2 {
	FLOAT x;
	FLOAT y;
} D3DXVECTOR2, * LPD3DXVECTOR2;

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
	D3DVECTOR TopLeft_54;
	D3DVECTOR BottomLeft_60;
	D3DVECTOR TopRight_6C;
	D3DVECTOR BottomRight_78;
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
	tagSPRARGS2* child_B4;
} SPRARGS2;
static_assert(sizeof(SPRARGS2) == 0xB8);

struct tagEvWorkRobot
{
	uint32_t workId_0;
	uint32_t dword4;
	ChrSet chrset_8;
	__declspec(align(8)) _D3DMATRIX d3dmatrix10;
	D3DVECTOR field_50;
	D3DVECTOR field_5C;
	int dword68;
	uint32_t dword6C;
	int dword70;
	uint32_t dword74;
	uint32_t dword78;
	uint32_t dword7C;
	int dword80;
	char byte84;
	char byte85;
	uint8_t unk_86[2];
	uint8_t unk_88[8];
};
static_assert(sizeof(tagEvWorkRobot) == 0x90);

inline void WaitForDebugger()
{
#ifdef _DEBUG
	while (!IsDebuggerPresent())
	{
	}
#endif
}
