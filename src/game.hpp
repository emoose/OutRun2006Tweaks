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

struct s_chrset_info // not actual name
{
	const char* bin_ptr;
	int field_4; // always 0?
	int xmtset_no_8;
	int field_C; // always 0?
};
static_assert(sizeof(s_chrset_info) == 0x10);

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
	uint32_t loadColiType_0;
	uint32_t field_4;
	__int16 roadSectionNum_8;
	uint8_t field_A;
	uint8_t unk_B;
	uint32_t curStageIdx_C;
};
static_assert(sizeof(OnRoadPlace) == 0x10);

typedef struct tagOthCarCtrlInfo
{
	uint8_t field_0[4];
	D3DVECTOR field_4;
	uint8_t field_10[8];
	float field_18;
	uint8_t unk_1C[8];
	int field_24;
	uint32_t field_28;
	uint8_t unk_2C[52];
	uint16_t field_60;
	uint8_t field_62[2];
	float field_64;
} OthCarCtrlInfo;
static_assert(sizeof(OthCarCtrlInfo) == 0x68);

typedef struct tagEVWORK_CAR
{
  uint32_t dword0;
  uint32_t flags_4;
  uint32_t field_8;
  uint32_t field_C;
  uint8_t car_id_10;
  char car_kind_11;
  char car_color_12;
  uint8_t manu_transmission_enable_13;
  D3DVECTOR position_14;
  D3DVECTOR spd_mb_20;
  int16_t field_2C[3];
  int16_t field_32;
  int pedal_amount_34;
  int field_38;
  int field_3C;
  int16_t field_40;
  uint16_t field_42;
  int16_t field_44;
  uint16_t field_46;
  uint32_t dword48;
  uint16_t field_4C;
  int16_t field_4E;
  uint8_t unk_50[8];
  float lightRate_58;
  OnRoadPlace OnRoadPlace_5C;
  uint8_t gap6C[4];
  D3DMATRIX matrix_70;
  D3DMATRIX matrix_B0;
  D3DMATRIX matrix_F0;
  D3DVECTOR vector_130[4];
  int16_t field_160;
  int16_t field_162;
  uint8_t unk_164[6];
  uint16_t field_16A;
  D3DVECTOR field_16C;
  float field_178;
  int16_t field_17C[3];
  uint8_t unk_182[2];
  OnRoadPlace OnRoadPlace_prev_184;
  uint8_t unk_194[44];
  int field_1C0;
  float field_1C4;
  float field_1C8;
  float field_1CC;
  float field_1D0;
  float field_1D4;
  uint32_t dword1D8;
  float field_1DC;
  float field_1E0;
  uint32_t dword1E4;
  D3DVECTOR spd_20_bk_1E8;
  uint32_t field_1F4;
  float field_1F8;
  uint16_t field_1FE;
  uint8_t unk_200[2];
  uint16_t field_202;
  uint16_t field_204;
  uint8_t unk_206[2];
  uint32_t cur_gear_208;
  uint32_t dword20C;
  uint32_t dword210;
  uint8_t gap214[8];
  float float21C;
  uint8_t gap220[36];
  uint32_t flags_244;
  uint8_t unk_248[4];
  uint32_t water_flag_24C[4];
  uint16_t flags_25C;
  uint16_t field_25E;
  uint16_t field_260;
  uint16_t field_262;
  float field_264;
  float field_268;
  float field_26C;
  uint8_t unk_270[12];
  int field_27C;
  uint8_t field_280;
  uint8_t field_coli_281;
  uint8_t field_282;
  uint8_t field_283;
  uint8_t unk_284[2];
  uint16_t field_286;
  uint8_t unk_288[8];
  uint32_t field_290;
  uint8_t unk_294[2];
  uint8_t field_296;
  uint8_t unk_297[1];
  uint8_t unk_298[16];
  uint32_t field_2A8;
  uint8_t unk_2AC[4];
  int field_2B0;
  int ptr_2B4;
  uint32_t field_2B8;
  uint32_t field_2BC;
  uint32_t field_2C0;
  struct SHADOWWORK *ShadowWork_2C4;
  float field_2C8;
  float field_2CC;
  float field_2D0;
  float field_2D4;
  D3DVECTOR distance_mb_2D8;
  D3DVECTOR field_2E4;
  uint32_t flags_2F0;
  uint32_t dword2F4;
  float crush_stiff_time_2F8;
  float field_2FC;
  float field_300;
  uint8_t gap304[8];
  float float30C;
  uint8_t gap310[4];
  uint32_t dword314;
  void *ptr_318;
  uint32_t dword31C;
  uint32_t dword320;
  uint32_t dword324;
  char lodLevel_328;
  uint8_t field_329;
  uint8_t byte32A;
  uint8_t byte32B;
  uint8_t gap32C[1980];
  uint32_t dwordAE8;
  uint8_t gapAEC[4];
  OthCarCtrlInfo OthCarCtrlInfo_AF0;
  int field_B58;
  int field_B5C;
  int16_t field_B60;
  int16_t field_B62;
  uint16_t field_B64;
  uint8_t unk_B66[2];
  float ghostcar_transparency_B68;
  uint8_t field_B6C;
  uint8_t field_B6D;
  uint16_t field_B6E;
  int16_t field_B70;
  int16_t field_B72;
  uint8_t field_B74;
  uint8_t unk_B75[3];
  float field_B78;
  float field_B7C;
  float field_B80;
  float field_B84;
  float field_B88;
  float field_B8C;
  float field_B90;
  float field_B94;
  uint8_t unk_B98[28];
  float field_BB4;
  float field_BB8;
  uint8_t unk_BBC[4];
  float field_BC0;
  uint8_t unk_BC4[8];
  float field_BCC;
  float field_BD0;
  float field_BD4;
  float field_BD8;
  uint8_t unk_BDC[4];
  float field_BE0;
  float field_BE4;
  float field_BE8;
  uint8_t unk_BEC[64];
  int16_t field_C2C;
  uint16_t field_C2E;
  uint8_t unk_C30[6];
  uint8_t field_C36;
  uint8_t unk_C37[1];
  char field_C38;
  uint8_t unk_C39[3];
  uint32_t field_C3C;
  uint8_t unk_C40[28];
  uint32_t flags_C5C;
  uint32_t field_C60;
  uint8_t unk_C64[12];
  D3DVECTOR field_C70;
  D3DVECTOR field_C7C;
  uint8_t unk_C88[8];
  D3DMATRIX mtx_C90;
  D3DMATRIX mtx_CD0;
  int field_D10;
  int field_D14;
  int field_D18;
  uint8_t unk_D1C[4];
  uint8_t field_D20;
  uint8_t field_D21;
  char field_D22;
  uint8_t byteD23;
  float field_D24;
  D3DVECTOR field_D28;
  uint8_t gapD34[2];
  char field_D36;
  uint8_t unk_D37[1];
  uint8_t unk_D38[12];
  uint16_t field_D44;
  int16_t field_D46;
  uint8_t unk_D48[4];
  int16_t field_D4C;
  int16_t field_D4E;
  int16_t field_D50;
  uint8_t unk_D52[2];
  uint8_t unk_D54[20];
  int16_t field_D68;
  int16_t field_D6A;
  D3DVECTOR field_D6C;
  int16_t field_D78;
  uint8_t unk_D7A[2];
  OnRoadPlace OnRoadPlace_D7C;
  uint16_t field_D8C;
  uint8_t unk_D8E[2];
  uint32_t dwordD90;
  int field_D94;
  int field_D98;
  int field_D9C;
  uint32_t field_DA0;
  char field_DA4;
  uint8_t unk_DA5[3];
  uint32_t field_DA8;
  uint32_t field_DAC;
  uint8_t commrace_getrank_DB0;
  uint8_t handicap_field_DB1;
  uint8_t unk_DB2[2];
  float handicap_field_DB4;
  uint16_t handicap_field_DB8;
  uint16_t handicap_field_DBA;
  float actionforce_DBC;
  float field_DC0;
  float field_DC4;
  uint8_t flags_DC8;
  uint8_t field_DC9;
  uint8_t unk_DCA[2];
  uint32_t field_DCC;
  uint16_t field_DD0;
  uint8_t unk_DD2[2];
  uint32_t dwordDD4;
  float field_DD8;
  float field_DDC;
  D3DVECTOR field_DE0;
  uint8_t unk_DEC[12];
  uint32_t dwordDF8;
  uint32_t dwordDFC;
  uint32_t dwordE00;
  uint32_t dwordE04;
  uint32_t dwordE08;
  float dwordE0C;
  float field_E10[7];
  float field_E2C[7];
  float field_E48[7];
  uint8_t unk_E64[4];
  float field_E68;
  float field_E6C;
  uint8_t field_E70;
  uint8_t unk_E71[3];
  int field_E74;
  int field_E78;
  uint32_t dwordE7C;
  uint32_t dwordE80;
  uint32_t auto_transmission_enable_E84;
  uint32_t dwordE88;
  uint32_t dwordE8C;
  uint32_t dwordE90;
  uint32_t dwordE94;
  uint32_t dwordE98;
  float dwordE9C;
  uint16_t wordEA0;
  uint8_t gapEA2[2];
  uint8_t unk_EA4[360];
  uint8_t BraRendStat_100C;
  uint8_t unk_1010[14];
  uint8_t PrevBraRendStat_101B;
  uint8_t unk_101C[16];
  float field_102C;
  uint16_t word1030;
  uint8_t unk_1032[2];
  D3DVECTOR field_1034;
  D3DVECTOR field_1040;
  uint8_t unk_104C[4];
  float field_1050;
  int field_1054;
  uint8_t unk_1058[120];
  int field_10D0;
  void *ptr_10D4;
  uint8_t field_10D8;
  uint8_t field_10D9;
  uint8_t unk_10DA[2];
  float field_10DC;
  float field_10E0;
  uint8_t unk_10E4[12];
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

typedef struct tagEvWorkRobot
{
	uint32_t workId_0;
	uint32_t dword4;
	ChrSet chrset_8;
	uint8_t unk_C[4];
	_D3DMATRIX d3dmatrix10;
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
} EvWorkRobot;
static_assert(sizeof(EvWorkRobot) == 0x90);

typedef struct TDrawEntry
{
	uint8_t todo[0x3C];
} DrawEntry;
static_assert(sizeof(DrawEntry) == 0x3C);

typedef struct TDrawBuffer
{
	int NumBuffers_0;
	int MaxBuffers_4;
	int unk_8[1];
	int MaxBuffers_C;
	DrawEntry** BufferPtrs_10;
	DrawEntry* Buffer_14;
	void* UnkBuffer_18;
} DrawBuffer;
static_assert(sizeof(DrawBuffer) == 0x1C);

inline void WaitForDebugger()
{
#ifdef _DEBUG
	while (!IsDebuggerPresent())
	{
	}
#endif
}
