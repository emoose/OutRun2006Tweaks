#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

class FixC2CRankings : public Hook
{
	// A lot of the C2C ranking code has a strange check that tries to OpenEventA an existing named event based on current process ID
	// However no code is included in the game to actually create this event first, so the OpenEventA call fails, and game skips the ranking code body
	// 
	// The only hit for that 0x19EA3FD3 magic number on google is a semi-decompiled Razor1911 crack, which contains code that creates this event
	// (searching github shows this might be "SecuROM_Tripwire")
	// 
	// Guess it's probably something that gets setup by the SecuROM stub code, and then game devs can add some kind of "if(SECUROM_CHECK) { do stuff }" which inserts the OpenEventA stuff
	// For the Steam release it seems they repacked the original pre-securom-wrapper 2006 game EXE without any changes, guess they forgot these checks were included?
public:
	std::string_view description() override
	{
		return "FixC2CRankings";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		char Buffer[52];

		DWORD CurrentProcessId = GetProcessId(GetCurrentProcess());
		sprintf(Buffer, "v7_%04d", CurrentProcessId ^ 0x19EA3FD3);
		CreateEventA(0, 1, 1, Buffer);

		return true;
	}

	static FixC2CRankings instance;
};
FixC2CRankings FixC2CRankings::instance;

class PreventDESTSaveCorruption : public Hook
{
	// Games input remapping code loops over every dinput device detected
	// for each one it updates some 0xB0 block of device-data from 0x7C211C onward
	// however, player save data begins at 0x7C23E0, which leaves only 0x2C4 bytes for device-data
	// 0x2C4 / 0xB0 = ~4 blocks of device-data, but as mentioned it'll still loop over every device
	// So past 4 dinput devices it'll trample over anything after the 0x2C4 bytes :(
	const static int SumoInputDeviceLoop_Addr = 0xD85C0;

	// workaround it by lying to the function when it checks how many dinput devices are available
	// probably means you won't be able to remap devices past device #4, ah well
	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		if (ctx.eax > 4)
			ctx.eax = 4;
	}

public:
	std::string_view description() override
	{
		return "PreventDESTSaveCorruption";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		dest_hook = safetyhook::create_mid(Module::exe_ptr(SumoInputDeviceLoop_Addr), destination);
		return !!dest_hook;
	}

	static PreventDESTSaveCorruption instance;
};
PreventDESTSaveCorruption PreventDESTSaveCorruption::instance;
