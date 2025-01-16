#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <random>
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <WinSock2.h>
#include <fstream>
#include <wincrypt.h>

class ProtectLoginData : public Hook
{
	// C2C saves plaintext online login details into SaveGame/Common.dat by default
	// It's not obvious this file contains login info though, so some might share it out freely
	// Now that online is restored those details could be extracted and reused
	// 
	// Instead of keeping it inside SaveGame/Common.dat, we'll store it in a seperate file next to game EXE
	// and zero-out the data inside Common.dat before it gets saved to disk
	// 
	// We'll also encrypt the data using CryptProtectData, which encrypts it against the users Windows account
	// So even if the file does get shared, it'd be difficult for others to be able to decrypt it
	const static inline std::string LoginDataFilename = "OnlineLoginData.dat";

	static void EncryptDataToFile(const uint8_t* inputData, int dataLength, const std::filesystem::path& outputFilePath)
	{
		DATA_BLOB inputBlob = { static_cast<DWORD>(dataLength), const_cast<BYTE*>(inputData) };
		DATA_BLOB outputBlob;

		if (!CryptProtectData(&inputBlob, L"OnlineLoginData", nullptr, nullptr, nullptr, 0, &outputBlob))
			throw std::runtime_error("CryptProtectData = false");
		else
		{
			std::ofstream outputFile(outputFilePath, std::ios::binary);
			if (outputFile.is_open())
			{
				outputFile.write((char*)outputBlob.pbData, outputBlob.cbData);
				outputFile.close();
			}
			LocalFree(outputBlob.pbData);
		}
	}

	static bool DecryptDataFromFile(uint8_t* outputData, int dataLength, const std::filesystem::path& inputFilePath)
	{
		if (!std::filesystem::exists(inputFilePath))
			return false;

		std::ifstream inputFile(inputFilePath, std::ios::binary | std::ios::ate);
		if (inputFile.is_open())
		{
			std::streamsize size = inputFile.tellg();
			inputFile.seekg(0, std::ios::beg);
			std::vector<char> encryptedData(size);
			if (inputFile.read(encryptedData.data(), size))
			{
				DATA_BLOB inputBlob = { static_cast<DWORD>(size), reinterpret_cast<BYTE*>(encryptedData.data()) };
				DATA_BLOB outputBlob;

				if (!CryptUnprotectData(&inputBlob, nullptr, nullptr, nullptr, nullptr, 0, &outputBlob))
					throw std::runtime_error("CryptUnprotectData = false");
				else
				{
					std::memcpy(outputData, outputBlob.pbData, min(int(outputBlob.cbData), dataLength));
					LocalFree(outputBlob.pbData);
					return true;
				}
			}
			inputFile.close();
		}
		
		return false;
	}

	inline static SafetyHookInline Sumo_CommonDat_Read_hook = {};
	static int Sumo_CommonDat_Read_dest()
	{
		auto ret = Sumo_CommonDat_Read_hook.call<int>();

		uint8_t* loginData = Module::exe_ptr<uint8_t>(0x3C205C);
		int loginDataLength = (0x10 * 8) + (8 * 8);

		try
		{
			DecryptDataFromFile(loginData, loginDataLength, Module::ExePath.parent_path() / LoginDataFilename);
		}
		catch (const std::exception& e)
		{
			memset(loginData, 0, loginDataLength);
			spdlog::error("ProtectLoginData: Failed to decrypt login data from {}: {}", LoginDataFilename, e.what());
		}

		return ret;
	}
	inline static SafetyHookInline Sumo_CommonDat_Write_hook = {};
	static int Sumo_CommonDat_Write_dest()
	{
		constexpr int loginDataLength = (0x10 * 8) + (8 * 8);
		uint8_t* loginData = Module::exe_ptr<uint8_t>(0x3C205C);

		uint8_t tempLoginData[loginDataLength];
		memcpy(tempLoginData, loginData, loginDataLength);

		try
		{
			EncryptDataToFile(loginData, loginDataLength, Module::ExePath.parent_path() / LoginDataFilename);
		}
		catch (const std::exception& e)
		{
			spdlog::error("ProtectLoginData: failed to encrypt login data to {} ({}), login details will be scrubbed from common.dat", e.what(), LoginDataFilename);
		}

		SecureZeroMemory(loginData, loginDataLength);

		auto ret = Sumo_CommonDat_Write_hook.call<int>();

		memcpy(loginData, tempLoginData, loginDataLength);

		return ret;
	}
	inline static SafetyHookInline Sumo_CommonDat_WriteRaw_hook = {};
	static int Sumo_CommonDat_WriteRaw_dest()
	{
		constexpr int loginDataLength = (0x10 * 8) + (8 * 8);
		uint8_t* loginData = Module::exe_ptr<uint8_t>(0x3C205C);

		uint8_t tempLoginData[loginDataLength];
		memcpy(tempLoginData, loginData, loginDataLength);

		try
		{
			EncryptDataToFile(loginData, loginDataLength, Module::ExePath.parent_path() / LoginDataFilename);
		}
		catch (const std::exception& e)
		{
			spdlog::error("ProtectLoginData: failed to encrypt login data to {} ({}), login details will be scrubbed from common.dat", e.what(), LoginDataFilename);
		}

		SecureZeroMemory(loginData, loginDataLength);

		auto ret = Sumo_CommonDat_WriteRaw_hook.call<int>();

		memcpy(loginData, tempLoginData, loginDataLength);

		return ret;
	}

public:
	std::string_view description() override
	{
		return "ProtectLoginData";
	}

	bool validate() override
	{
		return Settings::ProtectLoginData;
	}

	bool apply() override
	{
		constexpr int Sumo_CommonDat_Read_Addr = 0x16380;
		constexpr int Sumo_CommonDat_Write_Addr = 0x16420;
		constexpr int Sumo_CommonDat_WriteRaw_Addr = 0x164D0;

		Sumo_CommonDat_Read_hook = safetyhook::create_inline(Module::exe_ptr(Sumo_CommonDat_Read_Addr), Sumo_CommonDat_Read_dest);
		Sumo_CommonDat_Write_hook = safetyhook::create_inline(Module::exe_ptr(Sumo_CommonDat_Write_Addr), Sumo_CommonDat_Write_dest);
		Sumo_CommonDat_WriteRaw_hook = safetyhook::create_inline(Module::exe_ptr(Sumo_CommonDat_WriteRaw_Addr), Sumo_CommonDat_WriteRaw_dest);

		return true;
	}

	static ProtectLoginData instance;
};
ProtectLoginData ProtectLoginData::instance;

class PlaySegaJingle : public Hook
{
	inline static bool disableLooping = false;

	inline static SafetyHookMid begin = {};
	static void dest_begin(SafetyHookContext& ctx)
	{
		disableLooping = true;
		Game::adxPlay(0, 0x1D, 0);
	}

	inline static SafetyHookMid CSound__Play_LoopFlagOverride = {};
	static void CSound__Play_LoopFlagOverride_dest(SafetyHookContext& ctx)
	{
		if (disableLooping)
		{
			ctx.edx = 0;
			disableLooping = false;
		}
	}

public:
	std::string_view description() override
	{
		return "PlaySegaJingle";
	}

	bool validate() override
	{
		return
			(Settings::AllowFLAC && std::filesystem::exists(Module::ExePath.parent_path() / "Sound" / "sega441.flac")) ||
			std::filesystem::exists(Module::ExePath.parent_path() / "playsega.txt"); // allow playing the OGG version if txt file exists (maybe INI option later on)
	}

	bool apply() override
	{
		constexpr int SumoFrontEnd_state1_SegaBegin_HookAddr = 0x455AA;
		constexpr int CSound__Play_LoopFlagOverride_HookAddr = 0x12863;

		begin = safetyhook::create_mid(Module::exe_ptr(SumoFrontEnd_state1_SegaBegin_HookAddr), dest_begin);
		if (!begin)
			return false;

		CSound__Play_LoopFlagOverride = safetyhook::create_mid(Module::exe_ptr(CSound__Play_LoopFlagOverride_HookAddr), CSound__Play_LoopFlagOverride_dest);

		return !!begin && !!CSound__Play_LoopFlagOverride;
	}

	static PlaySegaJingle instance;
};
PlaySegaJingle PlaySegaJingle::instance;

class AllowCharacterSelection : public Hook
{
	inline static fn_1args_class Sumo_LicenseScreenChangeSelection = nullptr;

	inline static SafetyHookInline hook_orig = {};
	static void __fastcall destination(uint8_t* thisptr, void* unk, int button)
	{
		// not the actual names
		constexpr int BTN_DOWN = 4;
		constexpr int BTN_UP = 2;
		constexpr int SELECTION_PLAYERNAME = 0;
		constexpr int SELECTION_CHARACTER = 1;
		constexpr int SELECTION_NATIONALITY = 2;
		constexpr int SPRITES_CHARACTER = 5;

		int curSelection = *(DWORD*)(thisptr + 0x3C);

		if ((curSelection == SELECTION_PLAYERNAME && button == BTN_DOWN) || // If playername selected and user pressed down
			(curSelection == SELECTION_NATIONALITY && button == BTN_UP)) // Or nationality selected and user pressed up...
		{
			// Change selection to character switcher
			*(DWORD*)(thisptr + 0x3C) = SELECTION_CHARACTER;
			Sumo_LicenseScreenChangeSelection(thisptr, 0, (void*)SPRITES_CHARACTER);
			return;
		}

		hook_orig.thiscall(thisptr, button);
	}

public:
	std::string_view description() override
	{
		return "AllowCharacterSelection";
	}

	bool validate() override
	{
		return Settings::AllowCharacterSelection;
	}

	bool apply() override
	{
		Sumo_LicenseScreenChangeSelection = Module::fn_ptr<fn_1args_class>(0xDD990);

		constexpr int Sumo_CharacterSelectBtnPress_Addr = 0xDDC50;
		hook_orig = safetyhook::create_inline(Module::exe_ptr(Sumo_CharacterSelectBtnPress_Addr), destination);

		return !!hook_orig;
	}

	static AllowCharacterSelection instance;
};
AllowCharacterSelection AllowCharacterSelection::instance;

class DefaultManualTransmission : public Hook
{
	inline static SafetyHookInline Sumo_TransmissionSelection_Init{};
	static char __fastcall destination2(uint8_t* thisptr)
	{
		char ret = Sumo_TransmissionSelection_Init.thiscall<char>(thisptr);

		*(int*)(thisptr + 0xE4) = 0; // prev selection = auto
		*(int*)(thisptr + 0xE8) = 1; // current selection = manual

		// This can force sprite to update to manual immediately, which should improve presentation
		// but this causes it to draw transmision menu while the animation for opening it plays out
		// (that anim also seems to show automatic selected despite this update, maybe the graphic for that animation has it selected...)
		//fn_0args_class sub_51BE30 = (fn_0args_class)0x51BE30;
		//sub_51BE30(thisptr + 0x34, 0);

		return ret;
	}

public:
	std::string_view description() override
	{
		return "DefaultManualTransmission";
	}

	bool validate() override
	{
		return Settings::DefaultManualTransmission;
	}

	bool apply() override
	{
		Sumo_TransmissionSelection_Init = safetyhook::create_inline(Module::exe_ptr(0xDD250), destination2);

		return !!Sumo_TransmissionSelection_Init;
	}

	static DefaultManualTransmission instance;
};
DefaultManualTransmission DefaultManualTransmission::instance;

class EnableHollyCourse2 : public Hook
{
	// TODO: Percentage might not be affected by the holly 2 missions atm, letting it go above 100% might be neat

	inline static SafetyHookMid Sumo_IsRacerUnlocked_midhook{};
	inline static SafetyHookMid Sumo_IsRacerUnlocked_midhook2{};

	inline static SafetyHookMid Sumo_GetRacerRank_midhook{};
	inline static SafetyHookMid Sumo_GetRacerRank_midhook2{};

	inline static SafetyHookMid Sumo_RacerMenuInput_midhook{};
	inline static SafetyHookMid Sumo_RacerMenuInput_midhook2{};

	inline static SafetyHookMid Sumo_RacerSetupGame_midhook{};
	inline static SafetyHookMid Sumo_RacerSetupGame_midhook2{};

	inline static SafetyHookMid Sumo_RacerDrawRank_midhook{};
	inline static SafetyHookMid Sumo_RacerDrawRank_midhook2{};
	inline static SafetyHookMid Sumo_RacerDrawRank_midhook3{};

	inline static int isRaceHolly2 = false;
	static void Sumo_RacerSwitch7to6_dest(SafetyHookContext& ctx)
	{
		isRaceHolly2 = false;
		if (ctx.eax == 7)
		{
			isRaceHolly2 = true;
			ctx.eax = 6;
		}
	}

	static void Sumo_GetRankForHolly2_dest(SafetyHookContext& ctx)
	{
		if (isRaceHolly2)
			*(int*)(ctx.esp) = 3;
	}

	static void Sumo_IsRacerUnlocked_CheckHolly1Rank_dest(SafetyHookContext& ctx)
	{
		if (isRaceHolly2)
			*(int*)(ctx.esp) = 2;
	}

	static void Sumo_AddFlagForRacer7_dest(SafetyHookContext& ctx)
	{
		if (isRaceHolly2)
			ctx.ecx = (ctx.ecx & ~0x600) | 0x700; // 0x700 will let game load mix 2 course menu
	}

	static void Sumo_RacerSetupGame_SwitchToHolly2_dest(SafetyHookContext& ctx)
	{
		if (isRaceHolly2)
			*(int*)(ctx.esp + 8) = 2; // switch from race 3 to race 2, seems to be the race num for holly2 (despite PSP_MIX coming /after/ PS2_MIX, weird)
	}

	static void Sumo_RacerDrawRank_StringFix_eax_dest(SafetyHookContext& ctx)
	{
		if (ctx.eax >= 7)
		{
			ctx.eip += 7;
			ctx.ecx = 0x394; // "Perfect! Holly's got chills!"
		}
	}

	static void Sumo_RacerDrawRank_StringFix_edx_dest(SafetyHookContext& ctx)
	{
		if (ctx.edx >= 7)
			ctx.edx = 6;
	}

	static void Sumo_RacerDrawLockedRank_StringFix_eax_dest(SafetyHookContext& ctx)
	{
		if (ctx.eax >= 7)
		{
			ctx.eax = 6;
			// TODO: if EnableHollyCourse2 == 2, make this say something like
			// "To unlock, beat Holly 1 at Rank A!"
			// Sadly doesn't seem lang files include any string for that, might have to hardcode it
			// 
			//ctx.eip += 7;
			//ctx.ecx = 0x397; // Play Mix 01 on your Playstation Portable! (doesn't seem fitting...)
		}
	}

	// if Settings::EnableHollyCourse2 == 3, only draw holly2 sprite once holly1 is complete
	inline static SafetyHookMid Sumo_RacerMenuDraw_NumSprites_midhook{};
	static void Sumo_RacerMenuDraw_CheckHollyCompletion_dest(SafetyHookContext& ctx)
	{
		uint32_t edi = ctx.edi;
		uint32_t cmp_value = 7;

		static bool holly2unlocked_previous = false;
		bool holly2unlocked = Game::Sumo_CheckRacerUnlocked(7);
		if (holly2unlocked)
			cmp_value = 8; // allow loop to draw holly2 sprite

		// Patch selection structs to change whether holly2 can be selected, if holly2 state has changed at all
		if (holly2unlocked_previous != holly2unlocked)
		{
			holly2unlocked_previous = holly2unlocked;

			// Fix selection structs so course 2 can be selected
			Memory::VP::Patch(Module::exe_ptr(0x1CE430) + 0x4, holly2unlocked ? int(7) : int(-1)); // down from flagman 4
			Memory::VP::Patch(Module::exe_ptr(0x1CE460) + 0x8, holly2unlocked ? int(7) : int(4)); // right from holly 1

			// Allow selections to go in reverse, why not?
			Memory::VP::Patch(Module::exe_ptr(0x1CE400) + 0xC, int(3)); // left from flagman 1
			Memory::VP::Patch(Module::exe_ptr(0x1CE430) + 0x8, int(0)); // right from flagman 4
			Memory::VP::Patch(Module::exe_ptr(0x1CE440) + 0xC, holly2unlocked ? int(7) : int(6)); // left from clarissa
			Memory::VP::Patch(Module::exe_ptr(0x1CE470) + 0x8, int(4)); // right from holly 2
		}

		uintptr_t eflags = 0;

		// Update CF (Carry Flag) - if edi < 7 in an unsigned comparison
		if (edi < cmp_value) eflags |= (1 << 0);  // CF is the 0th bit

		ctx.eflags = eflags;
	}

public:
	std::string_view description() override
	{
		return "EnableHollyCourse2";
	}

	bool validate() override
	{
		return Settings::EnableHollyCourse2 != 0;
	}

	bool apply() override
	{
		// Increase Sumo_RacerMenu alloc size to fit extra sprite
		Memory::VP::Patch(Module::exe_ptr(0x425A6) + 1, int(0x674 + 0xA0));

		// Move sprite_494 references to the new sprite slot at 0x674, so holly2 can go at 494
		Memory::VP::Patch(Module::exe_ptr(0xE817A) + 2, int(0x674));
		Memory::VP::Patch(Module::exe_ptr(0xE8A58) + 2, int(0x674));
		Memory::VP::Patch(Module::exe_ptr(0xE8AD0) + 2, int(0x674));
		Memory::VP::Patch(Module::exe_ptr(0xE8DFA) + 2, int(0x674));

		// Increase racer sprite count from 7 to 8
		Memory::VP::Patch(Module::exe_ptr(0xE815C) + 1, uint8_t(8));
		Memory::VP::Patch(Module::exe_ptr(0xE8A43) + 1, int(8));
		Memory::VP::Patch(Module::exe_ptr(0xE8AE5) + 1, uint8_t(8));

		if (Settings::EnableHollyCourse2 == 3) // 3 = only show after holly1 is completed, hide if not complete
			Sumo_RacerMenuDraw_NumSprites_midhook = safetyhook::create_mid(Module::exe_ptr(0xE8D0E), Sumo_RacerMenuDraw_CheckHollyCompletion_dest);
		else
		{
			Memory::VP::Patch(Module::exe_ptr(0xE8D0B) + 2, uint8_t(8));

			// Fix selection structs so course 2 can be selected
			Memory::VP::Patch(Module::exe_ptr(0x1CE430) + 0x4, int(7)); // down from flagman 4
			Memory::VP::Patch(Module::exe_ptr(0x1CE460) + 0x8, int(7)); // right from holly 1

			// Allow selections to go in reverse, why not?
			Memory::VP::Patch(Module::exe_ptr(0x1CE400) + 0xC, int(3)); // left from flagman 1
			Memory::VP::Patch(Module::exe_ptr(0x1CE430) + 0x8, int(0)); // right from flagman 4
			Memory::VP::Patch(Module::exe_ptr(0x1CE440) + 0xC, int(7)); // left from clarissa
			Memory::VP::Patch(Module::exe_ptr(0x1CE470) + 0x8, int(4)); // right from holly 2
		}

		// Switch racer number from 7 to 6 to satisfy some switch cases
		Sumo_IsRacerUnlocked_midhook = safetyhook::create_mid(Module::exe_ptr(0xE8414), Sumo_RacerSwitch7to6_dest);
		Sumo_RacerMenuInput_midhook = safetyhook::create_mid(Module::exe_ptr(0xE87C6), Sumo_RacerSwitch7to6_dest);

		// Hook IsRacerUnlocked to make it check holly1 rank before unlocking holly2
		if (Settings::EnableHollyCourse2 == 2 || Settings::EnableHollyCourse2 == 3)
			Sumo_IsRacerUnlocked_midhook2 = safetyhook::create_mid(Module::exe_ptr(0xE8537), Sumo_IsRacerUnlocked_CheckHolly1Rank_dest);

		// Hook GetRacerRank to let it fetch holly2 rank
		Sumo_GetRacerRank_midhook = safetyhook::create_mid(Module::exe_ptr(0xE82B4), Sumo_RacerSwitch7to6_dest);
		Sumo_GetRacerRank_midhook2 = safetyhook::create_mid(Module::exe_ptr(0xE83E0), Sumo_GetRankForHolly2_dest);

		// Fix stringID crash when holly2 is selected...
		Sumo_RacerDrawRank_midhook = safetyhook::create_mid(Module::exe_ptr(0xE8E3D), Sumo_RacerDrawRank_StringFix_eax_dest);
		Sumo_RacerDrawRank_midhook2 = safetyhook::create_mid(Module::exe_ptr(0xE8E4D), Sumo_RacerDrawRank_StringFix_edx_dest);

		// Change text for locked Holly 2
		Sumo_RacerDrawRank_midhook3 = safetyhook::create_mid(Module::exe_ptr(0xE8EED), Sumo_RacerDrawLockedRank_StringFix_eax_dest);

		// Hacky fix to handle adding flag for racer 7
		Sumo_RacerMenuInput_midhook2 = safetyhook::create_mid(Module::exe_ptr(0xE8928), Sumo_AddFlagForRacer7_dest);

		// Hook game setup func to make it setup as Holly for race 2
		Sumo_RacerSetupGame_midhook = safetyhook::create_mid(Module::exe_ptr(0xEEB54), Sumo_RacerSwitch7to6_dest);
		Sumo_RacerSetupGame_midhook2 = safetyhook::create_mid(Module::exe_ptr(0xEEC0B), Sumo_RacerSetupGame_SwitchToHolly2_dest);
		
		return true;
	}

	static EnableHollyCourse2 instance;
};
EnableHollyCourse2 EnableHollyCourse2::instance;

class ShowOutRunMilesOnMenu : public Hook
{
	inline static SafetyHookMid midhook = {};
	static void destination(SafetyHookContext& ctx)
	{
		char* text_NotSignedIn = (char*)(ctx.eax);
		char* text_OutRunMiles = Game::Sumo_GetStringFromId(0x21D);
		int numMiles = (int)*Game::Sumo_NumOutRunMiles;

		Game::Sumo_Printf("%s - %s %d", text_NotSignedIn, text_OutRunMiles, numMiles);
	}

public:
	std::string_view description() override
	{
		return "ShowOutRunMilesOnMenu";
	}

	bool validate() override
	{
		return Settings::ShowOutRunMilesOnMenu;
	}

	bool apply() override
	{
		constexpr int PrintSignedInStatus_NotLoggedIn_Addr = 0x413E2;

		Memory::VP::Nop(Module::exe_ptr(PrintSignedInStatus_NotLoggedIn_Addr), 5);
		midhook = safetyhook::create_mid(Module::exe_ptr(PrintSignedInStatus_NotLoggedIn_Addr), destination);

		return !!midhook;
	}

	static ShowOutRunMilesOnMenu instance;
};
ShowOutRunMilesOnMenu ShowOutRunMilesOnMenu::instance;

class DemonwareServerOverride : public Hook
{
	inline static SafetyHookInline bdPlatformSocket__getHostByName_hook = {};
	static uint32_t __cdecl destination(const char* name, void* a2, int a3)
	{
		spdlog::debug("getHostByName: overriding host {} to {}", name, Settings::DemonwareServerOverride);
		auto ret = bdPlatformSocket__getHostByName_hook.ccall<uint32_t>(Settings::DemonwareServerOverride.c_str(), a2, a3);
		return ret;
	}

	inline static SafetyHookInline InitNetwork = {};
	static void __cdecl InitNetwork_dest()
	{
		InitNetwork.call();

		WSADATA tmp;
		WSAStartup(0x202, &tmp);

		int upnpError = UPNPDISCOVER_SUCCESS;
		UPNPDev* upnpDevice = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &upnpError);

		bool anyError = false;
		int ret = 0;
		if (upnpError != UPNPDISCOVER_SUCCESS || !upnpDevice)
		{
			spdlog::error("UPnP: upnpDiscover failed with error {}", upnpError);
			return;
		}

		struct UPNPUrls urls;
		struct IGDdatas data;
		char lanaddr[16];
		char wanaddr[16];

		ret = UPNP_GetValidIGD(upnpDevice, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
		if (ret != 1 && ret != 2 && ret != 3) // UPNP_GetValidIGD returning 1/2/3 should be fine
		{
			spdlog::error("UPnP: UPNP_GetValidIGD failed with error {}", ret);
			return;
		}

		std::list<int> portNums = { 41455, 41456, 41457 };

		for (auto port : portNums)
		{
			for (int i = 0; i < 2; i++)
			{
				ret = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
					std::to_string(port).c_str(), std::to_string(port).c_str(),
					lanaddr, "OutRun2006", i == 0 ? "TCP" : "UDP", NULL, NULL);
				if (ret != UPNPCOMMAND_SUCCESS)
				{
					spdlog::error("UPnP: UPNP_AddPortMapping failed for port {}/{}, error code {}", port, i, ret);
					anyError = true;
				}
			}
		}

		if (ret == 0 && !anyError)
		{
			spdlog::info("UPnP: port mappings succeeded");
		}
	}

public:
	std::string_view description() override
	{
		return "DemonwareServerOverride";
	}

	bool validate() override
	{
		return !Settings::DemonwareServerOverride.empty();
	}

	bool apply() override
	{
		constexpr int InitNetwork_Addr = 0x5ACB0;
		InitNetwork = safetyhook::create_inline(Module::exe_ptr(InitNetwork_Addr), InitNetwork_dest);

		constexpr int bdPlatformSocket__getHostByName_Addr = 0x1349B0;
		bdPlatformSocket__getHostByName_hook = safetyhook::create_inline(Module::exe_ptr(bdPlatformSocket__getHostByName_Addr), destination);

		// DW has a very simple check to see if hostname is an IP: if first digit is a number, it's an IP addr
		// this is unfortunate when google cloud gives hostnames that start with a number...
		// Disable isalpha check if our override isn't an IP address
		{
			constexpr int bdPlatformSocket__getHostByName_isalpha_Addr = 0x1349C7;

			bool isIPAddr = true;
			for (auto c : Settings::DemonwareServerOverride)
				if (isalpha(c))
				{
					isIPAddr = false;
					break;
				}

			if (!isIPAddr)
				Memory::VP::Nop(Module::exe_ptr(bdPlatformSocket__getHostByName_isalpha_Addr), 2);
		}

		return !!bdPlatformSocket__getHostByName_hook;
	}

	static DemonwareServerOverride instance;
};
DemonwareServerOverride DemonwareServerOverride::instance;

class RandomHighwayAnimSets : public Hook
{
	inline static SafetyHookMid GetBranchRenditionType_midhook = {};
	static void GetBranchRenditionType_dest(SafetyHookContext& ctx)
	{
		constexpr int br_step_tbl_Count = 15;
		constexpr int UniqueSetCount = 4;

		std::random_device rd;
		std::mt19937 g(rd());
		std::uniform_int_distribution<> distrib(0, UniqueSetCount - 1);

		ctx.eax = int(distrib(g));
		spdlog::debug("GetBranchRenditionType_dest: using set {}", int(ctx.eax));
	}

public:
	std::string_view description() override
	{
		return "RandomHighwayAnimSets";
	}

	bool validate() override
	{
		return Settings::RandomHighwayAnimSets;
	}

	bool apply() override
	{
		constexpr int GetBranchRenditionType_HookAddr = 0x4D429;

		GetBranchRenditionType_midhook = safetyhook::create_mid(Module::exe_ptr(GetBranchRenditionType_HookAddr), GetBranchRenditionType_dest);
		return !!GetBranchRenditionType_midhook;
	}

	static RandomHighwayAnimSets instance;
};
RandomHighwayAnimSets RandomHighwayAnimSets::instance;

class SingleCoreAffinity : public Hook
{
	inline static SafetyHookMid Sumo_InitFileLoad_midhook{};
	inline static SafetyHookMid Sumo_FileLoadThread_midhook{};
	inline static SafetyHookMid Sumo_FileCreateThread_midhook{};

	static void destination(SafetyHookContext& ctx)
	{
		DWORD_PTR threadAffinityMask = 1;
		SetThreadAffinityMask(GetCurrentThread(), 1);
	}

public:
	std::string_view description() override
	{
		return "SingleCoreAffinity";
	}

	bool validate() override
	{
		return Settings::SingleCoreAffinity;
	}

	bool apply() override
	{
		// Hook file load thread functions to make them share same core as main thread
		// (this allows other threads spawned by D3D/DSound/etc to still use different cores)

		Sumo_InitFileLoad_midhook = safetyhook::create_mid(Module::exe_ptr(0x231E0), destination);
		Sumo_FileLoadThread_midhook = safetyhook::create_mid(Module::exe_ptr(0x23940), destination);
		Sumo_FileCreateThread_midhook = safetyhook::create_mid(Module::exe_ptr(0x24090), destination);

		return true;
	}

	static SingleCoreAffinity instance;
};
SingleCoreAffinity SingleCoreAffinity::instance;

class RestoreJPClarissa : public Hook
{
	// Patch get_load_heroine_chrset to use the non-USA models
	// (oddly there are 3 USA models defined, but code only seems to use 2 of them?)

	inline static SafetyHookMid request_ending_data{};
	static void request_ending_data_dest(SafetyHookContext& ctx)
	{
		int* ea_scn_char = Module::exe_ptr<int>(0x3D3A7C);
		if (*ea_scn_char == 3)
			*ea_scn_char = 2;
	}

public:
	std::string_view description() override
	{
		return "RestoreJPClarissa";
	}

	bool validate() override
	{
		return Settings::RestoreJPClarissa;
	}

	bool apply() override
	{
		constexpr int get_load_heroine_chrset_Addr1 = 0x88044 + 1;
		constexpr int get_load_heroine_chrset_Addr2 = 0x8804E + 1;

		// chrset 6 -> 5 (or 6 -> 7 if HiDefCharacters are used)
		Memory::VP::Patch(Module::exe_ptr<int>(get_load_heroine_chrset_Addr1), (Settings::UseHiDefCharacters ? ChrSet::CHR_DR_GH00 : ChrSet::CHR_DR_G00));
		// chrset 8 -> 7
		Memory::VP::Patch(Module::exe_ptr<int>(get_load_heroine_chrset_Addr2), ChrSet::CHR_DR_GH00);

		// ending cutscene fixes (switches CHR_GAL_USA -> CHR_GAL, and fixes cutscene code to make use of it)
		{
			constexpr int ending_char_tbl = 0x1A4618;
			constexpr int get_chrset_PatchAddr = 0xB5AE6;
			constexpr int request_ending_data_HookAddr = 0x529A5;

			// ending_char_tbl[1] xmt 0xCF -> 0xCE (obj_chr_gal_usa_pmt -> obj_chr_gal_pmt)
			Memory::VP::Patch(Module::exe_ptr<int>(ending_char_tbl + 4), 0xCE);

			// remove code that forces model 0xE to 0xF
			Memory::VP::Nop(Module::exe_ptr(get_chrset_PatchAddr), 5);

			// Switch ea_scn_char 3 to ea_scn_char 2 to allow new model to be displayed in ending cutscene
			request_ending_data = safetyhook::create_mid(Module::exe_ptr(request_ending_data_HookAddr), request_ending_data_dest);
		}

		return true;
	}

	static RestoreJPClarissa instance;
};
RestoreJPClarissa RestoreJPClarissa::instance;

class CommandLineArguments : public Hook
{
	const static int Win32_CfgRead_Addr = 0xE4D0;

	inline static SafetyHookInline hook_orig = {};
	static void destination(char* cfgFileName)
	{
		hook_orig.call(cfgFileName);

		// game settings that we want to override after game has loaded in config file

		int argc;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv)
			return;

		for (int i = 1; i < argc; ++i)
		{
			try
			{
				if (!wcsicmp(argv[i], L"-width") && i + 1 < argc)
					Game::screen_resolution->x = std::stol(argv[++i], 0, 0);
				else if (!wcsicmp(argv[i], L"-height") && i + 1 < argc)
					Game::screen_resolution->y = std::stol(argv[++i], 0, 0);
				else if ((!wcsicmp(argv[i], L"-antialiasing") || !wcsicmp(argv[i], L"-aa")) && i + 1 < argc)
					*Game::D3DAntialiasing = std::stol(argv[++i], 0, 0); // TODO: clamp to valid AA options
				else if (!wcsicmp(argv[i], L"-window") || !wcsicmp(argv[i], L"-windowed") || !wcsicmp(argv[i], L"-w"))
					*Game::D3DWindowed = 1;
				else if (!wcsicmp(argv[i], L"-fullscreen") || !wcsicmp(argv[i], L"-fs"))
					*Game::D3DWindowed = 0;
				else if (!wcsicmp(argv[i], L"-fog"))
					*Game::D3DFogEnabled = 1;
				else if (!wcsicmp(argv[i], L"-nofog"))
					*Game::D3DFogEnabled = 0;
			}
			catch (...) {}
		}

		LocalFree(argv);
	}

public:
	std::string_view description() override
	{
		return "CommandLineArguments";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		// cmd-line args we want to apply before game runs
		// TODO: should probably merge this into Settings::read somehow
		int argc;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv)
			return true; // no cmd-line so hook is pointless..

		for (int i = 1; i < argc; ++i)
		{
			try
			{
				if (!wcsicmp(argv[i], L"-SkipIntros"))
				{
					spdlog::info("CommandLineArguments: SkipIntroLogos = true");
					Settings::SkipIntroLogos = true;
				}
				else if (!wcsicmp(argv[i], L"-OuttaTime")) // same arg as FXT wrapper
				{
					spdlog::info("CommandLineArguments: DisableCountdownTimer = true");
					Settings::DisableCountdownTimer = true;
				}
				else if (!wcsicmp(argv[i], L"-LevelSelect"))
				{
					spdlog::info("CommandLineArguments: EnableLevelSelect = true");
					Settings::EnableLevelSelect = true;
					Settings::SkipIntroLogos = true;
				}
			}
			catch (...) {}
		}
		LocalFree(argv);

		hook_orig = safetyhook::create_inline(Module::exe_ptr(Win32_CfgRead_Addr), destination);
		return !!hook_orig;
	}

	static CommandLineArguments instance;
};
CommandLineArguments CommandLineArguments::instance;

class GameDefaultConfigOverride : public Hook
{
public:
	std::string_view description() override
	{
		return "GameDefaultConfigOverride";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		// Override the default settings used when game INI doesn't exist/is empty
		// (these can still be overridden via INI if needed)

		if (Settings::SkipIntroLogos)
			*Game::Sumo_IntroLogosEnable = false;
		if (Settings::DisableCountdownTimer)
			*Game::Sumo_CountdownTimerEnable = false;

		if (Settings::AutoDetectResolution)
		{
			// Use the width/height of the primary screen
			int width = GetSystemMetrics(SM_CXSCREEN);
			int height = GetSystemMetrics(SM_CYSCREEN);
			if (width < 640 || height < 480)
				return false; // bail out if resolution is less than the default

			Game::screen_resolution->x = width;
			Game::screen_resolution->y = height;

			*Game::D3DFogEnabled = true;
			*Game::D3DWindowed = true;
			*Game::D3DAntialiasing = 2;
			*Game::CfgLanguage = 0;

			spdlog::info("GameDefaultConfigOverride: default resolution set to {}x{}, windowed enabled, fog enabled, antialiasing 2", width, height);
		}

		return true;
	}

	static GameDefaultConfigOverride instance;
};
GameDefaultConfigOverride GameDefaultConfigOverride::instance;

class EnableLevelSelect : public Hook
{
	inline static SafetyHookMid midhook = {};
	static void destination(SafetyHookContext& ctx)
	{
		// Game checked SwitchNow(0x10) which checks for X/horn press
		// but because of how we force level-select to show, that will display sumo sign-in screen instead...
		// we'll add an extra SwitchNow(0x100) check here to check for LS press as well
		
		if (Game::SwitchNow(0x100))
			ctx.eax = 1;
		if (GetAsyncKeyState(VK_SHIFT))
			ctx.eax = 1;
	}

public:
	std::string_view description() override
	{
		return "EnableLevelSelect";
	}

	bool validate() override
	{
		return Settings::EnableLevelSelect;
	}

	bool apply() override
	{
		// Change the param for "GetMenuForId" call from 0 to 0x1B
		constexpr int SumoFrontEnd__eventStep1_GetMenuForIdCall_Param = 0x45756 + 1;
		constexpr int SumoLevelSelect__update_SwitchNowResult_Addr = 0xE7838;
		Memory::VP::Patch(Module::exe_ptr(SumoFrontEnd__eventStep1_GetMenuForIdCall_Param), uint8_t(0x1B));

		midhook = safetyhook::create_mid(Module::exe_ptr(SumoLevelSelect__update_SwitchNowResult_Addr), destination);
		return !!midhook;
	}

	static EnableLevelSelect instance;
};
EnableLevelSelect EnableLevelSelect::instance;
