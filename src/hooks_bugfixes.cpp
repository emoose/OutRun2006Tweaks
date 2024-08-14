#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

class FixBinkLargeMovies : public Hook
{
	// Most cards don't support creating textures that aren't exact powers of 2, so game tries to find closest pow2 that would fit the movie
	// Sadly the function they use only goes up to 1024 max, anything larger will just use 1024 and cause a crash later
	// We'll just replace their AlignToPow2 func so we can work it out ourselves

	inline static SafetyHookMid Sumo_BinkGetPow2 = {};
	static void destination(SafetyHookContext& ctx)
	{
		int n = ctx.eax;
		// Handle the case where n is already a power of 2
		if (n && !(n & (n - 1))) {
			return;
		}

		// Set all bits after the most significant bit
		n--;
		n |= n >> 1;
		n |= n >> 2;
		n |= n >> 4;
		n |= n >> 8;
		n |= n >> 16;

		// Increment n to get the next power of 2
		ctx.eax = n;
	}

public:
	std::string_view description() override
	{
		return "FixBinkLargeMovies";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		constexpr int Sumo_BinkGetPow2_Addr = 0x14730;

		// patch start of func to 5 nops + ret
		Memory::VP::Patch(Module::exe_ptr(Sumo_BinkGetPow2_Addr), { 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3 });

		// midhook func
		Sumo_BinkGetPow2 = safetyhook::create_mid(Module::exe_ptr(Sumo_BinkGetPow2_Addr), destination);
		return !!Sumo_BinkGetPow2;
	}

	static FixBinkLargeMovies instance;
};
FixBinkLargeMovies FixBinkLargeMovies::instance;

class FixPegasusClopping : public Hook
{
	const static int SndOff_PEGA_Addr = 0x4BCC0;

	inline static SafetyHookInline SndOff_PEGA = {};
	static void destination()
	{
		SndOff_PEGA.call();

		constexpr int SfxClop = 0x8D;
		Game::PrjSndRequest(SND_STOP | SfxClop);
	}

public:
	std::string_view description() override
	{
		return "FixPegasusClopping";
	}

	bool validate() override
	{
		return Settings::FixPegasusClopping;
	}

	bool apply() override
	{
		SndOff_PEGA = safetyhook::create_inline(Module::exe_ptr(SndOff_PEGA_Addr), destination);
		return !!SndOff_PEGA;
	}

	static FixPegasusClopping instance;
};
FixPegasusClopping FixPegasusClopping::instance;

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
		return Settings::FixC2CRankings;
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
		return Settings::PreventDESTSaveCorruption;
	}

	bool apply() override
	{
		dest_hook = safetyhook::create_mid(Module::exe_ptr(SumoInputDeviceLoop_Addr), destination);
		return !!dest_hook;
	}

	static PreventDESTSaveCorruption instance;
};
PreventDESTSaveCorruption PreventDESTSaveCorruption::instance;

class FixLensFlarePath : public Hook
{
	// Game tries to load in lens flare data from common/, but the game files have it inside media/, causing lens flare not to be drawn.
	// We'll just patch code to load from media/ instead
	// (only patch it if file actually exists inside media/ though, some may have already moved it to common/)

	const static int LoadLensFlareOffset_StringAddr = 0x1A29F8;

public:
	std::string_view description() override
	{
		return "FixLensFlarePath";
	}

	bool validate() override
	{
		return Settings::FixLensFlarePath;
	}

	bool apply() override
	{
		std::string NewPath = "\\media\\lens_flare_offset.bin";
		if (std::filesystem::exists("." + NewPath))
		{
			auto* patch_addr = Module::exe_ptr<char>(LoadLensFlareOffset_StringAddr);

			DWORD dwProtect;
			VirtualProtect((void*)patch_addr, NewPath.length(), PAGE_EXECUTE_READWRITE, &dwProtect);
			strcpy(patch_addr, NewPath.c_str());
			VirtualProtect((void*)patch_addr, NewPath.length(), dwProtect, &dwProtect);
		}

		return true;
	}

	static FixLensFlarePath instance;
};
FixLensFlarePath FixLensFlarePath::instance;

class FixFullPedalChecks : public Hook
{
	// Game has a few checks to see if pedal is at full-press, with a value of 255
	// For some reason the dinput controller code only returns a max value of ~253 though
	// This breaks things like the backfire effect which is only allowed when pedal is fully pressed
	// 
	// Game does seem to have some other checks which just look if pedal is above 250
	// so we'll apply that logic here and make any between 250-255 register as 255
	//
	// TODO: should we also apply for negative 250 as well?
	// TODO: not really sure what the channel numbers are for, maybe should only apply to channel 1...

	const static int GetVolume_Addr = 0x53720;
	const static int GetVolumeOld_Addr = 0x53750;

	inline static SafetyHookInline GetVolume = {};
	static int GetVolume_dest(int channel)
	{
		int result = GetVolume.call<int>(channel);
		if (channel != 1 && channel != 2) // accelerate / brake pedals only
			return result;
		if (result < 255 && result >= 250)
			result = 255;
		return result;
	}

	inline static SafetyHookInline GetVolumeOld = {};
	static int GetVolumeOld_dest(int channel)
	{
		int result = GetVolumeOld.call<int>(channel);
		if (channel != 1 && channel != 2) // accelerate / brake pedals only
			return result;
		if (result < 255 && result >= 250)
			result = 255;
		return result;
	}

public:
	std::string_view description() override
	{
		return "FixFullPedalChecks";
	}

	bool validate() override
	{
		return Settings::FixFullPedalChecks;
	}

	bool apply() override
	{
		GetVolume = safetyhook::create_inline(Module::exe_ptr(GetVolume_Addr), GetVolume_dest);
		GetVolumeOld = safetyhook::create_inline(Module::exe_ptr(GetVolumeOld_Addr), GetVolumeOld_dest);
		return !!GetVolume && !!GetVolumeOld;
	}

	static FixFullPedalChecks instance;
};
FixFullPedalChecks FixFullPedalChecks::instance;

class HideOnlineSigninText : public Hook
{
	// Online mode is no longer active, let's try to clean up the remnants of it

	inline static SafetyHookInline Sumo_DrawActionButtonName = {};
	static bool __fastcall Sumo_DrawActionButtonName_dest(uint8_t* thisptr, int unused, int buttonId)
	{
		int num = buttonId + (8 * *(DWORD*)(thisptr + 0x408));
		int buttonStringId = *(DWORD*)(thisptr + (4 * num) + 0x18);
		if (buttonStringId == 664)
			return true;

		return Sumo_DrawActionButtonName.thiscall<bool>(thisptr, buttonId);
	}

	inline static SafetyHookInline Sumo_DrawActionButtonIcon1 = {};
	static bool __fastcall Sumo_DrawActionButtonIcon1_dest(uint8_t* thisptr, int unused, int buttonId)
	{
		int num = buttonId + (8 * *(DWORD*)(thisptr + 0x408));
		int buttonStringId = *(DWORD*)(thisptr + (4 * num) + 0x18);
		if (buttonStringId == 664)
			return true;

		return Sumo_DrawActionButtonIcon1.thiscall<bool>(thisptr, buttonId);
	}

	inline static SafetyHookInline Sumo_DrawActionButtonIcon2 = {};
	static bool __fastcall Sumo_DrawActionButtonIcon2_dest(uint8_t* thisptr, int unused, int buttonId)
	{
		int num = buttonId + (8 * *(DWORD*)(thisptr + 0x408));
		int buttonStringId = *(DWORD*)(thisptr + (4 * num) + 0x18);
		if (buttonStringId == 664)
			return true;

		return Sumo_DrawActionButtonIcon2.thiscall<bool>(thisptr, buttonId);
	}

public:
	std::string_view description() override
	{
		return "HideOnlineSigninText";
	}

	bool validate() override
	{
		return Settings::HideOnlineSigninText;
	}

	bool apply() override
	{
		constexpr int Sumo_PrintSignedInStatus_Addr = 0x41370;
		constexpr int Sumo_DrawSignedInStatusBox_PatchAddr = 0x43256;
		constexpr int Sumo_DrawActionButtonName_Addr = 0x46510;
		constexpr int Sumo_DrawActionButtonIcon1_Addr = 0x46A80;
		constexpr int Sumo_DrawActionButtonIcon2_Addr = 0x46B20;

		// Hide "Not Signed In" text
		Memory::VP::Patch(Module::exe_ptr(Sumo_PrintSignedInStatus_Addr), { 0xC3 });

		// Hide box that contains the message above
		Memory::VP::Patch(Module::exe_ptr(Sumo_DrawSignedInStatusBox_PatchAddr), { 0xEB });

		// Don't allow "sign in" action button text/icon to show
		Sumo_DrawActionButtonName = safetyhook::create_inline(Module::exe_ptr(Sumo_DrawActionButtonName_Addr), Sumo_DrawActionButtonName_dest);
		Sumo_DrawActionButtonIcon1 = safetyhook::create_inline(Module::exe_ptr(Sumo_DrawActionButtonIcon1_Addr), Sumo_DrawActionButtonIcon1_dest);
		Sumo_DrawActionButtonIcon2 = safetyhook::create_inline(Module::exe_ptr(Sumo_DrawActionButtonIcon2_Addr), Sumo_DrawActionButtonIcon2_dest);

		return !!Sumo_DrawActionButtonName;
	}

	static HideOnlineSigninText instance;
};
HideOnlineSigninText HideOnlineSigninText::instance;
