#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <atomic>
#include <intrin.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

class FixRightSideBunkiAnimations : public Hook
{
	// When heading through bunki on right side certain animations become inverted, with backside vertices appearing in front
	// These anims are normally setup for playing on left side, for the right side the game has to invert some matrices for them
	// However it also sets a flag on the model for the anim too, the purpose of the flag is unknown
	// With the flag set PutModel will cause D3DRS_CULLMODE to be set to D3DCULL_CCW, forcing backside vertices to become culled
	// This sounds like it should be correct, but somehow this causes the vertices issue above...
	// 
	// Fortunately removing the flag from being setup seems to remedy this (though it would be nice to know the true reason why)
public:
	std::string_view description() override
	{
		return "FixRightSideBunkiAnimations";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		constexpr int LoadBranchRenditionObject_SetsCCWFlag_PatchAddr = 0x4F6DC;
		Memory::VP::Nop(Module::exe_ptr(LoadBranchRenditionObject_SetsCCWFlag_PatchAddr), 7);

		return true;
	}

	static FixRightSideBunkiAnimations instance;
};
FixRightSideBunkiAnimations FixRightSideBunkiAnimations::instance;

class FixParticleRendering : public Hook
{
	// Code for rendering grass particles is same as Xbox ver, which scales some vertices based on screen resolution
	// However in Xbox ver the variable it uses for the screen res is actually resolution / 2, but on PC the variable used is the full resolution
	// This causes grass particles not to display properly on PC, usually being rendered out of view
	// (Possibly other particles too which seem to use similar scaling code - but only the code for grass particles is hooked here)
	// Fortunately the fix is simple, just divide the values calculated by the function by 2
	inline static SafetyHookMid midhook{};
	static void destination(SafetyHookContext& ctx)
	{
		float ptcl_size;
		__asm
		{
			fstp [ptcl_size]
		}

		float* esp_1C = (float*)(ctx.esp + 0x1C); // based on width
		float* esp_18 = (float*)(ctx.esp + 0x18); // based on height

		float mult = 0.5f;

		*esp_1C = *esp_1C * mult;
		*esp_18 = *esp_18 * mult;
		ptcl_size = ptcl_size * mult;

		__asm
		{
			fld [ptcl_size]
		}
	}

	// Fix metropolis firework texture issue (https://github.com/emoose/OutRun2006Tweaks/issues/52)
	// DispStage tries to draw object 0x630001, but since no CollNode list is passed it uses default CollNode 0, which likely causes road barriers to get drawn
	// We'll fix this by making it draw stage object with CollNode list used by DrawDistanceBehind = 2 instead
	// This seems to let it draw the fireworks properly, though this is a little hacky
	// I'm not really sure if there's any other way to fix it, seems that a CollNode array is required for it to draw model correctly...
	inline static SafetyHookMid midhook_fireworks{};
	static void destination_fireworks(SafetyHookContext& ctx)
	{
		static uint16_t DrawDistBehind_equals_2_CollNodes[] = 
		{ 0, 4, 5, 6, 7, 0xB, 0xC, 0xD, 0xE, 0x12, 0x13, 0x14, 0x15, 0x1A, 0x1B, 0x1F, 0x20, 0x21, 0xFFFF };

		Game::DrawObject_Internal(0x001a000a, 0, DrawDistBehind_equals_2_CollNodes, 0, -1, 0);

		ctx.eip = (uintptr_t)Module::exe_ptr(0x4E190);
	}

public:
	std::string_view description() override
	{
		return "FixParticleRendering";
	}

	bool validate() override
	{
		return Settings::FixParticleRendering;
	}

	bool apply() override
	{
		constexpr int particle_draw_rect2_HookAddr = 0x19009;
		midhook = safetyhook::create_mid(Module::exe_ptr(particle_draw_rect2_HookAddr), destination);

		constexpr int DispStage_HookAddr = 0x4E179;
		midhook_fireworks = safetyhook::create_mid(Module::exe_ptr(DispStage_HookAddr), destination_fireworks);

		return !!midhook;
	}

	static FixParticleRendering instance;
};
FixParticleRendering FixParticleRendering::instance;
																							
class FixIncorrectShading : public Hook
{
public:
	std::string_view description() override
	{
		return "FixIncorrectShading";
	}

	bool validate() override
	{
		return Settings::FixIncorrectShading;
	}

	bool apply() override
	{
		// Cutscene chars (and some stage models) are setup with M_MA_DOUBLESIDE_LIGHTING MatAttrib flag, which makes use of the VSULT_TWOSIDE_* shaders
		// Unfortunately these shaders seem incomplete and just flip the lighting direction, causing these objects to appear incorrect
		// This "fixes" them by just pointing VSULT_TWOSIDE_* vertex shaders to use VSULT_DOUBLE_* vertex shader code instead
		// (this isn't a complete fix since only one side will be shaded correctly, but it's still an improvement)
		// 
		// Online Arcade includes HLSL_LambertLit.shad shader code which likely reveals the problem with these
		// a commented out line was meant to set oB0 output register, to shade the backside of vertexes
		// but this register only appears to exist on Xbox OG, and sadly wasn't carried forward into DX9
		// 
		// In C2C it seems instead of trying to emulate this with pixel shaders, the TWOSIDE shaders instead just flip the light direction
		// Maybe this helped to fix /something/ in the game, but it ended up breaking a whole lot more
		// By patching the TWOSIDE shaders to use the normal DOUBLE ones instead that seems to fix most of the shading issues found
		// 
		// In Lindbergh it seems DOUBLE_LAMBERT is handled by fixed_vs000.vp, and TWOSIDE_LAMBERT by fixed_vs018.vp
		// Interestingly Lindbergh seems to calculate the backside color different to how C2C and the commented-out Online Arcade code did
		// Sadly trying to copy that implementation doesn't work, guess Lindbergh might use a pixel shader somewhere to split the colors of each side?

		auto* VSULT_DOUBLE_LAMBERT = Module::exe_ptr<uint8_t>(0x21FB48);
		auto* VSULT_DOUBLE_SPECULAR = Module::exe_ptr<uint8_t>(0x21FC88);

		constexpr int VSULT_TWOSIDE_LAMBERT_Pointer = 0x33F04C;
		constexpr int VSULT_TWOSIDE_SPECULAR_Pointer = 0x33F050;

		Memory::VP::Patch(Module::exe_ptr<void*>(VSULT_TWOSIDE_LAMBERT_Pointer), VSULT_DOUBLE_LAMBERT);
		Memory::VP::Patch(Module::exe_ptr<void*>(VSULT_TWOSIDE_SPECULAR_Pointer), VSULT_DOUBLE_SPECULAR);

		return true;
	}

	static FixIncorrectShading instance;
};
FixIncorrectShading FixIncorrectShading::instance;

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
		n++;

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

class FixFileLoadRace : public Hook
{
	static constexpr int ServiceRequest_Addr = 0x23670;      // Sumo_FileLoadServiceRequest
	static constexpr int ServiceRequestMoveDone_Addr = 0x2371A;
	static constexpr int sumo_fread_Addr = 0x23CF0;
	static constexpr int sumo_fread_finished_Addr = 0x23E30;

	// Request id meaning "nothing is in flight", answered without consulting
	// either list.
	static constexpr int NoRequest = 0x80000000;

	// The loader keeps its read requests on three doubly linked lists: free,
	// pending (queued, not started) and active (in progress). Each list header
	// holds its own guard semaphore at +0x8, and Sumo_ListPopFront and
	// Sumo_ListPushBack treat a null there as "no guard needed, mutate directly".
	//
	// Sumo_FileLoadServiceRequest takes both guard semaphores, nulls both header
	// fields, moves one request from pending to active, then puts the fields back
	// and releases. Nulling them is how it stops its own pop and push from
	// deadlocking on a semaphore it already holds, but the fields are global, so
	// for the length of that move every other thread reads "no guard needed" too:
	//
	//  - sumo_fread pushes onto the pending list with no lock, against the
	//    loader's concurrent pop, which can lose or duplicate a request.
	//  - sumo_fread_finished waits on a null handle. The wait fails, so it skips
	//    both list searches and answers "finished" for a read that has not
	//    started. LoadXmtsetObject then advances its state machine over object
	//    data that has not arrived.
	//
	// Holding a critical section across the move, and across the two functions
	// the main thread reaches those lists through, gives back the exclusion the
	// nulled fields drop. Nothing takes a guard semaphore before this lock, so
	// the two cannot deadlock against each other. The loader releases it before
	// the read itself, which suspends its own thread and would otherwise strand
	// anyone waiting.
	inline static CRITICAL_SECTION ListLock{};

	inline static SafetyHookMid ServiceRequest_hook = {};
	static void ServiceRequest_dest(SafetyHookContext& ctx)
	{
		EnterCriticalSection(&ListLock);
	}

	inline static SafetyHookMid ServiceRequestMoveDone_hook = {};
	static void ServiceRequestMoveDone_dest(SafetyHookContext& ctx)
	{
		LeaveCriticalSection(&ListLock);
	}

	inline static SafetyHookInline sumo_fread_hook = {};
	static uint32_t __cdecl sumo_fread_dest(void* buf, int numBytes, int a3, void* file, int* outSize)
	{
		EnterCriticalSection(&ListLock);
		uint32_t result = sumo_fread_hook.ccall<uint32_t>(buf, numBytes, a3, file, outSize);
		LeaveCriticalSection(&ListLock);

		// A read that queues nothing still hands back an id, and sumo_fread_finished
		// answers "finished" for it without consulting a list, so callers walk their
		// state machine over a buffer that was never filled. Reaching this with a
		// real read to make means the free list of request records ran dry.
		if (result == NoRequest && numBytes > 0 && file)
			spdlog::error("FixFileLoadRace: file loader refused a {} byte read, its data will be left unset", numBytes);

		return result;
	}

	inline static SafetyHookInline sumo_fread_finished_hook = {};
	static int __cdecl sumo_fread_finished_dest(int file, int requestId)
	{
		if (requestId == NoRequest)
			return sumo_fread_finished_hook.ccall<int>(file, requestId);

		EnterCriticalSection(&ListLock);
		int result = sumo_fread_finished_hook.ccall<int>(file, requestId);
		LeaveCriticalSection(&ListLock);

		return result;
	}

	static constexpr int LoadTextures_Addr = 0x2E8D1;
	static constexpr int XPR0Entry_Addr = 0x557BE0;    // g_Xmt_XPR0Entry
	static constexpr int TextureIndex_Addr = 0x55B22C;
	static constexpr int XPR0EntrySize = 20;

	// LoadTextures creates one texture per call from a 20 byte XPR0 entry,
	// walking g_Xmt_XPR0Entry through the xmtset's system memory block.
	// ReadXmtsetTexture builds that pointer out of the block's own contents, so
	// any read that lands late leaves it pointing outside the block and the first
	// field read faults. Finishing the xmtset short of textures is survivable,
	// reading unmapped memory is not.
	inline static SafetyHookMid LoadTextures_hook = {};
	static void LoadTextures_dest(SafetyHookContext& ctx)
	{
		uint8_t* xmt = reinterpret_cast<uint8_t*>(ctx.edi);
		uint8_t* head = *reinterpret_cast<uint8_t**>(xmt);                    // XMTSET::head
		uint8_t** objectHandle = *reinterpret_cast<uint8_t***>(xmt + 0x28);   // XMTSET::ObjectHandle
		const uint32_t blockSize = *reinterpret_cast<uint32_t*>(xmt + 0x3C);  // XMTSET::dwSysMemDataSize

		uint32_t& textureIdx = *reinterpret_cast<uint32_t*>(Module::exe_ptr(TextureIndex_Addr));

		// Same test LoadTextures makes a few instructions later, repeated here so
		// an xmtset already given up on doesn't report again on every call.
		if (!head || textureIdx >= *reinterpret_cast<uint32_t*>(head + 8))
			return;

		uint8_t* entry = *reinterpret_cast<uint8_t**>(Module::exe_ptr(XPR0Entry_Addr));
		uint8_t* block = objectHandle ? *objectHandle : nullptr;

		if (block && entry >= block && entry + XPR0EntrySize <= block + blockSize)
			return;

		spdlog::error("FixFileLoadRace: XPR0 entry {:p} outside xmtset block {:p}+{:X}, skipping its remaining textures",
			(void*)entry, (void*)block, blockSize);

		// Raising the index to the texture count makes that test read as done.
		textureIdx = *reinterpret_cast<uint32_t*>(head + 8);
	}

public:
	std::string_view description() override
	{
		return "FixFileLoadRace";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		// Held only across a list pop and push, so a waiter is better off spinning
		// than paying for the trip into the kernel.
		InitializeCriticalSectionAndSpinCount(&ListLock, 4000);

		ServiceRequest_hook = safetyhook::create_mid(Module::exe_ptr(ServiceRequest_Addr), ServiceRequest_dest);
		ServiceRequestMoveDone_hook = safetyhook::create_mid(Module::exe_ptr(ServiceRequestMoveDone_Addr), ServiceRequestMoveDone_dest);
		sumo_fread_hook = safetyhook::create_inline(Module::exe_ptr(sumo_fread_Addr), sumo_fread_dest);
		sumo_fread_finished_hook = safetyhook::create_inline(Module::exe_ptr(sumo_fread_finished_Addr), sumo_fread_finished_dest);
		LoadTextures_hook = safetyhook::create_mid(Module::exe_ptr(LoadTextures_Addr), LoadTextures_dest);

		return ServiceRequest_hook && ServiceRequestMoveDone_hook && sumo_fread_hook && sumo_fread_finished_hook && LoadTextures_hook;
	}

	static FixFileLoadRace instance;
};
FixFileLoadRace FixFileLoadRace::instance;

// LoadXmtsetObject builds an xmtset's objects in one state and its textures in
// the next, each by calling a worker over and over until half a millisecond of
// the frame has gone.
// 
// The worker says when it has nothing left to do, but that only gets checked
// after the loop has exited, so a state that finishes early still wastes time
// in the loop.
//
// Each loop saves the time it started at and repeats while GetNowCpuTime() minus
// that stays under 0.5. Backdating the saved time makes the next comparison read
// as over budget, ending the loop on its own so it falls into the completion
// check it was always going to make.
class FileLoadSliceEndsEarly : public Hook
{
	// Both sites are "mov bl, al" straight after the worker returns, so al still
	// holds its answer and the following call is the only relative operand.
	static constexpr int ReadObjects_Addr = 0x2E399;
	static constexpr int LoadTextures_Addr = 0x2E3F5;

	// Where both loops keep the millisecond their slice began at.
	static constexpr int SliceStart_Offset = 0x1C;

	// Far enough back that the subtraction clears 0.5 by any margin a frame timer
	// could produce.
	static constexpr float Backdated = -1.0e9f;

	inline static SafetyHookMid ReadObjects_hook = {};
	inline static SafetyHookMid LoadTextures_hook = {};

	static void EndSliceWhenDone(SafetyHookContext& ctx)
	{
		if (ctx.eax & 0xFF)
			*reinterpret_cast<float*>(ctx.esp + SliceStart_Offset) = Backdated;
	}

public:
	std::string_view description() override
	{
		return "FileLoadSliceEndsEarly";
	}

	bool validate() override
	{
		return Settings::FramerateFastLoad > 0;
	}

	bool apply() override
	{
		ReadObjects_hook = safetyhook::create_mid(Module::exe_ptr(ReadObjects_Addr), EndSliceWhenDone);
		LoadTextures_hook = safetyhook::create_mid(Module::exe_ptr(LoadTextures_Addr), EndSliceWhenDone);

		return ReadObjects_hook && LoadTextures_hook;
	}

	static FileLoadSliceEndsEarly instance;
};
FileLoadSliceEndsEarly FileLoadSliceEndsEarly::instance;

// Sumo UI wraparound fix
// For some reason UIs with options layed out vertically (pause menu) allow wraparound
// from top/bottom ends, but the UIs that are horizontal (main menu) don't.
// 
// Added hooks to allow these horizontal menus to wrap-around too, makes it a
// little quicker to get to the ends of the lists now.
class MenuSelectionWrap : public Hook
{
	static constexpr int Increment_Addr = 0x11BB10;
	static constexpr int Decrement_Addr = 0x11BBA0;

	static constexpr int MaxIndex_Offset = 0xAC;
	static constexpr int PrevIndex_Offset = 0xB0;
	static constexpr int CurIndex_Offset = 0xB4;

	// Some menus have their own selected-index tests which skip calling Increment/Decrement
	// if the tests fail, not really sure why since Inc/Dec also tested index too.
	// Just nop these out so they call into our hooks.
	struct Guard { int addr; int size; };
	static constexpr Guard Guards[] = {
		{ 0xD35BF, 6 }, // stage select, left (right side guard checks for higher value than list actually contains, dev mistake?)
		{ 0xCBDB6, 6 }, // game mode select, left
		{ 0xCBE66, 6 }, // game mode select, right
		{ 0xCF51B, 6 }, // sub_4CEF30, left
		{ 0xCF5E8, 6 }, // sub_4CEF30, right
		{ 0xD91E8, 2 }, // sub_4D90A0, left
		{ 0xD91FE, 2 }, // sub_4D90A0, right
	};

	static int& field(void* thisptr, int offset)
	{
		return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(thisptr) + offset);
	}

	// Neither function's index arithmetic can land on an odd entry, so the resting
	// one for the target is handed over as entry 0, where an index of -1 sends the
	// increment half. init_465860 copies the three dwords out during the call, so
	// the stand-in only has to outlive that.
	static int wrapTo(void* thisptr, int target)
	{
		uint32_t** table = reinterpret_cast<uint32_t**>(thisptr);
		uint32_t* entries = *table;

		const int resting = 3 * (2 * target + 1);
		uint32_t wrapEntry[3] = { entries[resting], entries[resting + 1], entries[resting + 2] };

		*table = wrapEntry;
		field(thisptr, CurIndex_Offset) = -1;
		int result = Increment_hook.thiscall<int>(thisptr);
		*table = entries;

		const int landedOn = field(thisptr, CurIndex_Offset);

		field(thisptr, CurIndex_Offset) = target;
		field(thisptr, PrevIndex_Offset) = target;

		return result;
	}

	inline static SafetyHookInline Increment_hook = {};
	static int __fastcall Increment_dest(void* thisptr, int unused)
	{
		int& current = field(thisptr, CurIndex_Offset);
		const int maxIndex = field(thisptr, MaxIndex_Offset);

		if (current < maxIndex)
			return Increment_hook.thiscall<int>(thisptr);

		return wrapTo(thisptr, 0);
	}

	inline static SafetyHookInline Decrement_hook = {};
	static int __fastcall Decrement_dest(void* thisptr, int unused)
	{
		int& current = field(thisptr, CurIndex_Offset);
		const int maxIndex = field(thisptr, MaxIndex_Offset);

		// A single option has nowhere to wrap to.
		if (current > 0 || maxIndex <= 0)
			return Decrement_hook.thiscall<int>(thisptr);

		return wrapTo(thisptr, maxIndex);
	}

public:
	std::string_view description() override
	{
		return "MenuSelectionWrap";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		Increment_hook = safetyhook::create_inline(Module::exe_ptr(Increment_Addr), Increment_dest);
		Decrement_hook = safetyhook::create_inline(Module::exe_ptr(Decrement_Addr), Decrement_dest);

		for (const Guard& guard : Guards)
			Memory::VP::Nop(Module::exe_ptr<uint8_t>(guard.addr), guard.size);

		return !!Increment_hook && !!Decrement_hook;
	}

	static MenuSelectionWrap instance;
};
MenuSelectionWrap MenuSelectionWrap::instance;
