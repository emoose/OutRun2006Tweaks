#pragma once
#include <d3d9types.h>
#include <Xinput.h>
#include <unordered_map>

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

enum GameStage : int
{
	STAGE_PALM_BEACH,
	STAGE_DEEP_LAKE,
	STAGE_INDUSTRIAL_COMPLEX,
	STAGE_ALPINE,
	STAGE_SNOW_MOUNTAIN,
	STAGE_CLOUDY_HIGHLAND,
	STAGE_CASTLE_WALL,
	STAGE_GHOST_FOREST,
	STAGE_CONIFEROUS_FOREST,
	STAGE_DESERT,
	STAGE_TULIP_GARDEN,
	STAGE_METROPOLIS,
	STAGE_ANCIENT_RUINS,
	STAGE_CAPE_WAY,
	STAGE_IMPERIAL_AVENUE,
	STAGE_BEACH,
	STAGE_SEQUOIA,
	STAGE_NIAGARA,
	STAGE_LAS_VEGAS,
	STAGE_ALASKA,
	STAGE_GRAND_CANYON,
	STAGE_SAN_FRANCISCO,
	STAGE_AMAZON,
	STAGE_MACHU_PICCHU,
	STAGE_YOSEMITE,
	STAGE_MAYA,
	STAGE_NEW_YORK,
	STAGE_PRINCE_EDWARD,
	STAGE_FLORIDA,
	STAGE_EASTER_ISLAND,
	STAGE_PALM_BEACH_R,
	STAGE_DEEP_LAKE_R,
	STAGE_INDUSTRIAL_COMPLEX_R,
	STAGE_ALPINE_R,
	STAGE_SNOW_MOUNTAIN_R,
	STAGE_CLOUDY_HIGHLAND_R,
	STAGE_CASTLE_WALL_R,
	STAGE_GHOST_FOREST_R,
	STAGE_CONIFEROUS_FOREST_R,
	STAGE_DESERT_R,
	STAGE_TULIP_GARDEN_R,
	STAGE_METROPOLIS_R,
	STAGE_ANCIENT_RUINS_R,
	STAGE_CAPE_WAY_R,
	STAGE_IMPERIAL_AVENUE_R,
	STAGE_BEACH_R,
	STAGE_SEQUOIA_R,
	STAGE_NIAGARA_R,
	STAGE_LAS_VEGAS_R,
	STAGE_ALASKA_R,
	STAGE_GRAND_CANYON_R,
	STAGE_SAN_FRANCISCO_R,
	STAGE_AMAZON_R,
	STAGE_MACHU_PICCHU_R,
	STAGE_YOSEMITE_R,
	STAGE_MAYA_R,
	STAGE_NEW_YORK_R,
	STAGE_PRINCE_EDWARD_R,
	STAGE_FLORIDA_R,
	STAGE_EASTER_ISLAND_R,
	STAGE_PALM_BEACH_T,
	STAGE_BEACH_T,
	STAGE_PALM_BEACH_BT,
	STAGE_BEACH_BT,
	STAGE_PALM_BEACH_BR,
	STAGE_BEACH_BR,

	STAGE_COUNT
};
static_assert(sizeof(GameStage) == 4);

enum GameEvent : __int32
{
	EVENT_G_TIME = 0x0,
	EVENT_GAMEPLAYTIME = 0x1,
	EVENT_MOVIE = 0x2,
	EVENT_ADV = 0x3,
	EVENT_SEL = 0x4,
	EVENT_ENTRY = 0x5,
	EVENT_AUTOSCENE = 0x6,
	EVENT_PETTY_AS = 0x7,
	EVENT_CAR01 = 0x8,
	EVENT_CAR02 = 0x9,
	EVENT_CAR03 = 0xA,
	EVENT_CAR04 = 0xB,
	EVENT_CAR05 = 0xC,
	EVENT_CAR06 = 0xD,
	EVENT_CAR07 = 0xE,
	EVENT_CAR08 = 0xF,
	EVENT_CAR09 = 0x10,
	EVENT_CAR10 = 0x11,
	EVENT_CAR11 = 0x12,
	EVENT_CAR12 = 0x13,
	EVENT_CAR13 = 0x14,
	EVENT_CAR14 = 0x15,
	EVENT_CAR15 = 0x16,
	EVENT_CAR16 = 0x17,
	EVENT_CAR17 = 0x18,
	EVENT_CAR18 = 0x19,
	EVENT_CAR19 = 0x1A,
	EVENT_CAR20 = 0x1B,
	EVENT_CAR21 = 0x1C,
	EVENT_CAR22 = 0x1D,
	EVENT_CAR23 = 0x1E,
	EVENT_CAR24 = 0x1F,
	EVENT_CS_OSO01 = 0x20,
	EVENT_CS_OSO02 = 0x21,
	EVENT_CS_OSO03 = 0x22,
	EVENT_CS_OSO04 = 0x23,
	EVENT_CS_OSO05 = 0x24,
	EVENT_CS_OSO06 = 0x25,
	EVENT_CS_OSO07 = 0x26,
	EVENT_CS_OSO08 = 0x27,
	EVENT_CS_OSO09 = 0x28,
	EVENT_CS_OSO10 = 0x29,
	EVENT_CS_OSO11 = 0x2A,
	EVENT_CS_OSO12 = 0x2B,
	EVENT_CS_OSO13 = 0x2C,
	EVENT_CS_OSO14 = 0x2D,
	EVENT_CS_OSO15 = 0x2E,
	EVENT_CS_OSO16 = 0x2F,
	EVENT_CS_OSO17 = 0x30,
	EVENT_CS_OSO18 = 0x31,
	EVENT_CS_OSO19 = 0x32,
	EVENT_CS_OSO20 = 0x33,
	EVENT_CS_OSO21 = 0x34,
	EVENT_CS_OSO22 = 0x35,
	EVENT_CS_OSO23 = 0x36,
	EVENT_CS_OSO24 = 0x37,
	EVENT_CS_OSO25 = 0x38,
	EVENT_CS_OSO26 = 0x39,
	EVENT_CS_OSO27 = 0x3A,
	EVENT_CS_OSO28 = 0x3B,
	EVENT_CS_OSO29 = 0x3C,
	EVENT_CS_OSO30 = 0x3D,
	EVENT_CS_OSO31 = 0x3E,
	EVENT_CS_OSO32 = 0x3F,
	EVENT_CS_OSO33 = 0x40,
	EVENT_CS_OSO34 = 0x41,
	EVENT_CS_OSO35 = 0x42,
	EVENT_CS_OSO36 = 0x43,
	EVENT_CS_OSO37 = 0x44,
	EVENT_CS_OSO38 = 0x45,
	EVENT_CS_OSO39 = 0x46,
	EVENT_CS_OSO40 = 0x47,
	EVENT_CS_OSO41 = 0x48,
	EVENT_CS_OSO42 = 0x49,
	EVENT_CS_OSO43 = 0x4A,
	EVENT_CS_OSO44 = 0x4B,
	EVENT_CS_OSO45 = 0x4C,
	EVENT_CS_OSO46 = 0x4D,
	EVENT_CS_OSO47 = 0x4E,
	EVENT_CS_OSO48 = 0x4F,
	EVENT_CS_OSO49 = 0x50,
	EVENT_CS_OSO50 = 0x51,
	EVENT_CS_OSO51 = 0x52,
	EVENT_CS_OSO52 = 0x53,
	EVENT_CS_OSO53 = 0x54,
	EVENT_CS_OSO54 = 0x55,
	EVENT_CS_OSO55 = 0x56,
	EVENT_CS_OSO56 = 0x57,
	EVENT_CS_OSO57 = 0x58,
	EVENT_CS_OSO58 = 0x59,
	EVENT_CS_OSO59 = 0x5A,
	EVENT_CS_OSO60 = 0x5B,
	EVENT_CS_OSO61 = 0x5C,
	EVENT_CS_OSO62 = 0x5D,
	EVENT_CS_OSO63 = 0x5E,
	EVENT_CS_OSO64 = 0x5F,
	EVENT_CS_OSO65 = 0x60,
	EVENT_CS_OSO66 = 0x61,
	EVENT_CS_OSO67 = 0x62,
	EVENT_CS_OSO68 = 0x63,
	EVENT_CS_OSO69 = 0x64,
	EVENT_CS_OSO70 = 0x65,
	EVENT_CS_OSO71 = 0x66,
	EVENT_CS_OSO72 = 0x67,
	EVENT_CS_OSO73 = 0x68,
	EVENT_CS_OSO74 = 0x69,
	EVENT_CS_OSO75 = 0x6A,
	EVENT_CS_OSO76 = 0x6B,
	EVENT_CS_OSO77 = 0x6C,
	EVENT_CS_OSO78 = 0x6D,
	EVENT_CS_OSO79 = 0x6E,
	EVENT_CS_OSO80 = 0x6F,
	EVENT_CS_OSO81 = 0x70,
	EVENT_CS_OSO82 = 0x71,
	EVENT_CS_OSO83 = 0x72,
	EVENT_CS_OSO84 = 0x73,
	EVENT_CS_OSO85 = 0x74,
	EVENT_CS_OSO86 = 0x75,
	EVENT_CS_OSO87 = 0x76,
	EVENT_CS_OSO88 = 0x77,
	EVENT_CS_OSO89 = 0x78,
	EVENT_CS_OSO90 = 0x79,
	EVENT_CS_OSO91 = 0x7A,
	EVENT_CS_OSO92 = 0x7B,
	EVENT_CS_OSO93 = 0x7C,
	EVENT_CS_OSO94 = 0x7D,
	EVENT_CS_OSO95 = 0x7E,
	EVENT_CS_OSO96 = 0x7F,
	EVENT_CS_OSO97 = 0x80,
	EVENT_CS_OSO98 = 0x81,
	EVENT_CS_OSO99 = 0x82,
	EVENT_CS_OSO100 = 0x83,
	EVENT_CS_OSO101 = 0x84,
	EVENT_CS_OSO102 = 0x85,
	EVENT_CS_OSO103 = 0x86,
	EVENT_CS_OSO104 = 0x87,
	EVENT_CS_OSO105 = 0x88,
	EVENT_CS_OSO106 = 0x89,
	EVENT_CS_OSO107 = 0x8A,
	EVENT_CS_OSO108 = 0x8B,
	EVENT_CS_OSO109 = 0x8C,
	EVENT_CS_OSO110 = 0x8D,
	EVENT_CS_OSO111 = 0x8E,
	EVENT_CS_OSO112 = 0x8F,
	EVENT_CS_OSO113 = 0x90,
	EVENT_CS_OSO114 = 0x91,
	EVENT_CS_OSO115 = 0x92,
	EVENT_CS_OSO116 = 0x93,
	EVENT_CS_OSO117 = 0x94,
	EVENT_CS_OSO118 = 0x95,
	EVENT_CS_OSO119 = 0x96,
	EVENT_CS_OSO120 = 0x97,
	EVENT_CS_OSO121 = 0x98,
	EVENT_CS_OSO122 = 0x99,
	EVENT_CS_OSO123 = 0x9A,
	EVENT_CS_OSO124 = 0x9B,
	EVENT_CS_OSO125 = 0x9C,
	EVENT_CS_OSO126 = 0x9D,
	EVENT_CS_OSO127 = 0x9E,
	EVENT_CS_OSO128 = 0x9F,
	EVENT_CS_OSO129 = 0xA0,
	EVENT_CS_OSO130 = 0xA1,
	EVENT_CS_OSO131 = 0xA2,
	EVENT_CS_OSO132 = 0xA3,
	EVENT_CS_OSO133 = 0xA4,
	EVENT_CS_OSO134 = 0xA5,
	EVENT_CS_OSO135 = 0xA6,
	EVENT_CS_OSO136 = 0xA7,
	EVENT_CS_OSO137 = 0xA8,
	EVENT_CS_OSO138 = 0xA9,
	EVENT_CS_OSO139 = 0xAA,
	EVENT_CS_OSO140 = 0xAB,
	EVENT_CS_OSO141 = 0xAC,
	EVENT_CS_OSO142 = 0xAD,
	EVENT_CS_OSO143 = 0xAE,
	EVENT_CS_OSO144 = 0xAF,
	EVENT_CS_OSO145 = 0xB0,
	EVENT_CS_OSO146 = 0xB1,
	EVENT_CS_OSO147 = 0xB2,
	EVENT_CS_OSO148 = 0xB3,
	EVENT_CS_OSO149 = 0xB4,
	EVENT_CS_OSO150 = 0xB5,
	EVENT_CS_OSO151 = 0xB6,
	EVENT_CS_OSO152 = 0xB7,
	EVENT_CS_OSO153 = 0xB8,
	EVENT_CS_OSO154 = 0xB9,
	EVENT_CS_OSO155 = 0xBA,
	EVENT_CS_OSO156 = 0xBB,
	EVENT_CS_OSO157 = 0xBC,
	EVENT_CS_OSO158 = 0xBD,
	EVENT_CS_OSO159 = 0xBE,
	EVENT_CS_OSO160 = 0xBF,
	EVENT_CS_OSO161 = 0xC0,
	EVENT_CS_OSO162 = 0xC1,
	EVENT_CS_OSO163 = 0xC2,
	EVENT_CS_OSO164 = 0xC3,
	EVENT_CS_OSO165 = 0xC4,
	EVENT_CS_OSO166 = 0xC5,
	EVENT_CS_OSO167 = 0xC6,
	EVENT_CS_OSO168 = 0xC7,
	EVENT_CS_OSO169 = 0xC8,
	EVENT_CS_OSO170 = 0xC9,
	EVENT_CS_OSO171 = 0xCA,
	EVENT_CS_OSO172 = 0xCB,
	EVENT_CS_OSO173 = 0xCC,
	EVENT_CS_OSO174 = 0xCD,
	EVENT_CS_OSO175 = 0xCE,
	EVENT_CS_OSO176 = 0xCF,
	EVENT_CS_OSO177 = 0xD0,
	EVENT_CS_OSO178 = 0xD1,
	EVENT_CS_OSO179 = 0xD2,
	EVENT_CS_OSO180 = 0xD3,
	EVENT_CS_OSO181 = 0xD4,
	EVENT_CS_OSO182 = 0xD5,
	EVENT_CS_OSO183 = 0xD6,
	EVENT_CS_OSO184 = 0xD7,
	EVENT_CS_OSO185 = 0xD8,
	EVENT_CS_OSO186 = 0xD9,
	EVENT_CS_OSO187 = 0xDA,
	EVENT_CS_OSO188 = 0xDB,
	EVENT_CS_OSO189 = 0xDC,
	EVENT_CS_OSO190 = 0xDD,
	EVENT_CS_OSO191 = 0xDE,
	EVENT_CS_OSO192 = 0xDF,
	EVENT_CS_OSO193 = 0xE0,
	EVENT_CS_OSO194 = 0xE1,
	EVENT_CS_OSO195 = 0xE2,
	EVENT_CS_OSO196 = 0xE3,
	EVENT_CS_OSO197 = 0xE4,
	EVENT_CS_OSO198 = 0xE5,
	EVENT_CS_OSO199 = 0xE6,
	EVENT_CS_OSO200 = 0xE7,
	EVENT_CS_OSO201 = 0xE8,
	EVENT_CS_OSO202 = 0xE9,
	EVENT_CS_OSO203 = 0xEA,
	EVENT_CS_OSO204 = 0xEB,
	EVENT_CS_OSO205 = 0xEC,
	EVENT_CS_OSO206 = 0xED,
	EVENT_CS_OSO207 = 0xEE,
	EVENT_CS_OSO208 = 0xEF,
	EVENT_CS_OSO209 = 0xF0,
	EVENT_CS_OSO210 = 0xF1,
	EVENT_CS_OSO211 = 0xF2,
	EVENT_CS_OSO212 = 0xF3,
	EVENT_CS_OSO213 = 0xF4,
	EVENT_CS_OSO214 = 0xF5,
	EVENT_CS_OSO215 = 0xF6,
	EVENT_CS_OSO216 = 0xF7,
	EVENT_CS_OSO217 = 0xF8,
	EVENT_CS_OSO218 = 0xF9,
	EVENT_CS_OSO219 = 0xFA,
	EVENT_CS_OSO220 = 0xFB,
	EVENT_CS_OSO221 = 0xFC,
	EVENT_CS_OSO222 = 0xFD,
	EVENT_CS_OSO223 = 0xFE,
	EVENT_CS_OSO224 = 0xFF,
	EVENT_CS_OSO225 = 0x100,
	EVENT_CS_OSO226 = 0x101,
	EVENT_CS_OSO227 = 0x102,
	EVENT_CS_OSO228 = 0x103,
	EVENT_CS_OSO229 = 0x104,
	EVENT_CS_OSO230 = 0x105,
	EVENT_CS_OSO231 = 0x106,
	EVENT_CS_OSO232 = 0x107,
	EVENT_CS_OSO233 = 0x108,
	EVENT_CS_OSO234 = 0x109,
	EVENT_CS_OSO235 = 0x10A,
	EVENT_CS_OSO236 = 0x10B,
	EVENT_CS_OSO237 = 0x10C,
	EVENT_CS_OSO238 = 0x10D,
	EVENT_CS_OSO239 = 0x10E,
	EVENT_CS_OSO240 = 0x10F,
	EVENT_CS_OSO241 = 0x110,
	EVENT_CS_OSO242 = 0x111,
	EVENT_CS_OSO243 = 0x112,
	EVENT_CS_OSO244 = 0x113,
	EVENT_CS_OSO245 = 0x114,
	EVENT_CS_OSO246 = 0x115,
	EVENT_CS_OSO247 = 0x116,
	EVENT_CS_OSO248 = 0x117,
	EVENT_CS_OSO249 = 0x118,
	EVENT_CS_OSO250 = 0x119,
	EVENT_BR_OSO01 = 0x11A,
	EVENT_BR_OSO02 = 0x11B,
	EVENT_BR_OSO03 = 0x11C,
	EVENT_BR_OSO04 = 0x11D,
	EVENT_BR_OSO05 = 0x11E,
	EVENT_BR_OSO06 = 0x11F,
	EVENT_BR_OSO07 = 0x120,
	EVENT_BR_OSO08 = 0x121,
	EVENT_BR_OSO09 = 0x122,
	EVENT_BR_OSO10 = 0x123,
	EVENT_BR_OSO11 = 0x124,
	EVENT_BR_OSO12 = 0x125,
	EVENT_BR_OSO13 = 0x126,
	EVENT_BR_OSO14 = 0x127,
	EVENT_BR_OSO15 = 0x128,
	EVENT_BR_OSO16 = 0x129,
	EVENT_BR_OSO17 = 0x12A,
	EVENT_BR_OSO18 = 0x12B,
	EVENT_BR_OSO19 = 0x12C,
	EVENT_BR_OSO20 = 0x12D,
	EVENT_BR_OSO21 = 0x12E,
	EVENT_BR_OSO22 = 0x12F,
	EVENT_BR_OSO23 = 0x130,
	EVENT_BR_OSO24 = 0x131,
	EVENT_BR_OSO25 = 0x132,
	EVENT_BR_OSO26 = 0x133,
	EVENT_HAM_OSO01 = 0x134,
	EVENT_HAM_OSO02 = 0x135,
	EVENT_HAM_OSO03 = 0x136,
	EVENT_HAM_OSO04 = 0x137,
	EVENT_HAM_OSO05 = 0x138,
	EVENT_HAM_OSO06 = 0x139,
	EVENT_HAM_OSO07 = 0x13A,
	EVENT_HAM_OSO08 = 0x13B,
	EVENT_HAM_OSO09 = 0x13C,
	EVENT_HAM_OSO10 = 0x13D,
	EVENT_HAM_OSO11 = 0x13E,
	EVENT_HAM_OSO12 = 0x13F,
	EVENT_HAM_OSO13 = 0x140,
	EVENT_HAM_OSO14 = 0x141,
	EVENT_HAM_OSO15 = 0x142,
	EVENT_HAM_OSO16 = 0x143,
	EVENT_HAM_OSO17 = 0x144,
	EVENT_HAM_OSO18 = 0x145,
	EVENT_HAM_OSO19 = 0x146,
	EVENT_HAM_OSO20 = 0x147,
	EVENT_HAM_OSO21 = 0x148,
	EVENT_HAM_OSO22 = 0x149,
	EVENT_HAM_OSO23 = 0x14A,
	EVENT_HAM_OSO24 = 0x14B,
	EVENT_HAM_OSO25 = 0x14C,
	EVENT_HAM_OSO26 = 0x14D,
	EVENT_HAM_OSO27 = 0x14E,
	EVENT_HAM_OSO28 = 0x14F,
	EVENT_HAM_OSO29 = 0x150,
	EVENT_HAM_OSO30 = 0x151,
	EVENT_HAM_OSO31 = 0x152,
	EVENT_HAM_OSO32 = 0x153,
	EVENT_HAM_OSO33 = 0x154,
	EVENT_HAM_OSO34 = 0x155,
	EVENT_HAM_OSO35 = 0x156,
	EVENT_HAM_OSO36 = 0x157,
	EVENT_HAM_OSO37 = 0x158,
	EVENT_HAM_OSO38 = 0x159,
	EVENT_HAM_OSO39 = 0x15A,
	EVENT_HAM_OSO40 = 0x15B,
	EVENT_REND_OSO01 = 0x15C,
	EVENT_REND_OSO02 = 0x15D,
	EVENT_REND_OSO03 = 0x15E,
	EVENT_REND_OSO04 = 0x15F,
	EVENT_REND_OSO05 = 0x160,
	EVENT_REND_OSO06 = 0x161,
	EVENT_REND_OSO07 = 0x162,
	EVENT_REND_OSO08 = 0x163,
	EVENT_REND_OSO09 = 0x164,
	EVENT_REND_OSO10 = 0x165,
	EVENT_EFCT_MOV = 0x166,
	EVENT_OBSERVER = 0x167,
	EVENT_COMM_TRANS = 0x168,
	EVENT_HEADLIGHTS = 0x169,
	EVENT_ROB01 = 0x16A,
	EVENT_ROB02 = 0x16B,
	EVENT_ROB03 = 0x16C,
	EVENT_ROB04 = 0x16D,
	EVENT_ROB05 = 0x16E,
	EVENT_ROB06 = 0x16F,
	EVENT_ROB07 = 0x170,
	EVENT_ROB08 = 0x171,
	EVENT_ROB09 = 0x172,
	EVENT_ROB10 = 0x173,
	EVENT_ROB11 = 0x174,
	EVENT_ROB12 = 0x175,
	EVENT_ROB13 = 0x176,
	EVENT_ROB14 = 0x177,
	EVENT_ROB15 = 0x178,
	EVENT_ROB16 = 0x179,
	EVENT_ROB17 = 0x17A,
	EVENT_ROB18 = 0x17B,
	EVENT_ROB19 = 0x17C,
	EVENT_ROB20 = 0x17D,
	EVENT_ROB21 = 0x17E,
	EVENT_SOUND = 0x17F,
	EVENT_OC_EDIT = 0x180,
	EVENT_CAMERA = 0x181,
	EVENT_SCN_ENV = 0x182,
	EVENT_SCN_EFC = 0x183,
	EVENT_NAVI_PUB = 0x184,
	EVENT_GAD_PUB = 0x185,
	EVENT_AREA = 0x186,
	EVENT_SKY = 0x187,
	EVENT_ROUTE = 0x188,
	EVENT_RANKING = 0x189,
	EVENT_VOICE_FIGHT = 0x18A,
	EVENT_G_NAMEENTRY = 0x18B,
	EVENT_EXEC_DRAW = 0x18C,
	EVENT_PART_EFC = 0x18D,
	EVENT_SPRANI = 0x18E,
	EVENT_F_LOAD = 0x18F,
	EVENT_TOUR_MISSIONS = 0x190,
	EVENT_RACE_MODE = 0x191,
	EVENT_RACE_ATTACK = 0x192,
	EVENT_TIME_ATTACK = 0x193,
	EVENT_DRIFT_ATTACK = 0x194,
	EVENT_SUMOFE = 0x195,
	EVENT_FE_BG_MOVIE = 0x196,
	EVENT_CUSTOM_SOUNDTRACK = 0x197,
	EVENT_DISC_ERRORS = 0x198,
	EVENT_SUMOREWARD = 0x199,
};

struct TAG_model_info
{
	uint8_t todo[0x98];
};
static_assert(sizeof(TAG_model_info) == 0x98);

struct StageTable_mb
{
	GameStage StageUniqueName_0;
	int StageTableIdx_4;
	uint32_t field_8;
	uint32_t field_C;
	int ExtendedTime_10;
	TAG_model_info* CsInfoPtr_14;
	TAG_model_info* BrInfoPtr_18;
	int CsInfoId_1C;
	int BrInfoId_20;
	int ExitTableIdx_24[2];
	int ExitIDs_2C[2];
	uint8_t gap34[48];
	struct tagOthcarPercentTable* OthcarPercentTablePtr_64;
	void* OthcarPercentTablePtr_68;
	uint32_t field_6C;
	uint32_t field_70;
	uint32_t field_74;
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
	uint32_t dword0;
	uint32_t dword4;
	uint32_t dword8;
	uint8_t gapC[12];
	uint32_t dword18;
	uint32_t dword1C;
	uint32_t dword20;
	uint8_t gap24[28];
	_D3DMATRIX d3dmatrix40;
	float dword80;
	float dword84;
	float dword88;
	uint32_t dword8C;
	uint32_t dword90;
	uint32_t dword94;
	uint32_t dword98;
	uint32_t dword9C;
	float perspective_a1_A0;
	float dwordA4;
	float dwordA8;
	float CamFov_AC;
	float field_B0;
	float field_B4;
	float perspective_a2_B8;
	float perspective_znear_BC;
	float perspective_zfar_C0;
	float perspective_a5_C4;
	float perspective_a6_C8;
	uint32_t dwordCC;
	uint32_t dwordD0;
	D3DVECTOR field_D4;
	D3DVECTOR cam_pos_orig_E0;
	D3DVECTOR look_pos_orig_EC;
	D3DVECTOR cam_pos_F8;
	D3DVECTOR look_pos_104;
	uint32_t dword110;
	uint32_t dword114;
	uint32_t dword118;
	uint32_t dword11C;
	uint32_t dword120;
	uint32_t dword124;
	D3DVECTOR cam_ang_128;
	uint8_t gap134[12];
	_D3DMATRIX d3dmatrix140;
	_D3DMATRIX d3dmatrix180;
	_D3DMATRIX cam_matrix_1C0;
	_D3DMATRIX d3dmatrix200;
	_D3DMATRIX d3dmatrix240;
	_D3DMATRIX d3dmatrix280;
	_D3DMATRIX d3dmatrix2C0;
	char char300;
	uint8_t gap301[59];
	uint16_t word33C;
	uint16_t word33E;
	uint16_t word340;
	uint16_t word342;
	uint16_t word344;
	uint16_t word346;
	uint16_t word348;
	char cam_mode_34A;
	char cam_mode_prev_34B;
	uint32_t dword34C;
	uint8_t gap350[4];
	uint32_t dword354;
	float field_358;
	float field_35C;
	uint8_t field_360;
	uint8_t gap361[3];
	float cam_mode_timer_364;
	uint16_t word368;
	D3DVECTOR dword36C;
	uint32_t dword378;
	uint32_t dword37C;
	float dword380;
	D3DVECTOR vector_384;
	D3DVECTOR vector_390;
	D3DVECTOR vector_39C;
	D3DVECTOR vector_3A8;
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

  inline bool is_in_bunki() { return OnRoadPlace_5C.loadColiType_0 != 0; }
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
	int field_8;
	int MaxBuffers_C;
	DrawEntry** BufferPtrs_10;
	DrawEntry* Buffer_14;
	void* UnkBuffer_18;
} DrawBuffer;
static_assert(sizeof(DrawBuffer) == 0x1C);

// Info sent between game and master server
struct SumoNet_MatchMakingInfo
{
	uint32_t SlotCount_0;
	uint32_t SlotCount_4;
	int SlotCount_8;
	int SlotCount_C;
	char unk_field_24_10;
	uint8_t unk_11[3];
	uint32_t MatchType_14;
	uint32_t Nationality_18;
	uint32_t unk_field_54_1C;
	uint32_t unk_field_58_20;
	uint32_t CourseType_24;
	uint32_t Course_28;
	uint32_t unk_field_64_2C;
	uint32_t CarClass_30;
	uint32_t CarType_34;
	uint32_t CatchUp_38;
	uint32_t unk_field_74_3C;
	uint32_t Collision_40;
	uint32_t unk_field_7C_44;
	char LobbyName_48[16];
};
static_assert(sizeof(SumoNet_MatchMakingInfo) == 0x58);

// Info sent between servers & clients
struct sSumoNet_LobbyInfo
{
	uint8_t MatchType_0;
	uint8_t Nationality_1;
	uint8_t unk_field_54_2;
	uint8_t unk_field_58_3;
	uint8_t CourseType_4;
	uint8_t Course_5;
	uint8_t unk_field_64_6;
	uint8_t CarClass_7;
	uint8_t CarType_8;
	uint8_t CatchUp_9;
	uint8_t unk_field_74_A;
	uint8_t Collision_B;
	uint8_t unk_field_7C_C;
	uint8_t SlotCount0_D;
	uint8_t SlotCount4_E;
	uint8_t unk_F;
	uint8_t field_10;
	uint8_t unk_11;
	uint8_t field_12;
	uint8_t field_13;
	char LobbyName[16];
};
static_assert(sizeof(sSumoNet_LobbyInfo) == 0x24);

struct SumoNet_NetDriver
{
	uint32_t vftable;
	uint8_t unk_4;
	uint8_t is_in_lobby_5;
	uint8_t unk_6;
	uint8_t is_hosting_7;
	uint8_t unk_8[0xC];
	uint8_t is_hosting_14;
	uint8_t unk_15[3];

	inline bool is_driver_online()
	{
		// some other drivers seem to be for lan/online/etc
		// TODO: should probably use Module::exe_ptr here, but it's not included atm..
		return vftable == 0x627EB8;
	}
	inline bool is_driver_lan()
	{
		return vftable == 0x627BC8;
	}

	inline bool is_driver_valid()
	{
		return is_driver_online() || is_driver_lan();
	}

	inline bool is_in_lobby()
	{
		return is_driver_valid() && is_in_lobby_5;
	}

	inline bool is_hosting()
	{
		return is_in_lobby() && is_hosting_7;
	}

	inline bool is_hosting_online()
	{
		return is_hosting() && is_driver_online();
	}
};
static_assert(sizeof(SumoNet_NetDriver) == 0x18);
