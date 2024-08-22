#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

#include <d3d9.h>

class SumoUIFlashingTextFix : public Hook
{
	static SumoUIFlashingTextFix instance;

	// Hacky fix for the flashing "Not Signed In" / "Signed In As" text when playing above 60FPS
	// Normally SumoFrontEndEvent_Ctrl calls into SumoFrontEnd_animate function, which then handles drawing the text
	// SumoFrontEndEvent_Ctrl is only ran at 60FPS however, and will skip frames when running above that
	// (If playing at 120FPS, 1/2 frames will skip calling EventControl, which will skip running SumoFrontEndEvent_Ctrl, and the text won't be drawn that frame)
	// 
	// Unfortunately running SumoFrontEndEvent_Ctrl every frame makes the C2C UI speed up too
	// Instead this just removes the original SumoFrontEnd_animate caller, and handles calling that function ourselves every frame instead
	// 
	// TODO: this fixes the Not Signed In text, but there are still other flashing Sumo menus that need a fix too (eg. showroom menus), maybe there's others too?
public:
	static void draw()
	{
		if (!instance.active())
			return;

		// Make sure sumo FE event is active...
		uint8_t* status = Module::exe_ptr<uint8_t>(0x39FB48);
		if ((status[0x195] & 0x18) == 0 && (status[0x195] & 2) != 0) // 0x195 = EVENT_SUMOFE
		{
			uint8_t* frontend = (uint8_t*)Game::SumoFrontEnd_GetSingleton_4035F0();
			// Check we're in the right state #
			if (frontend && *(int*)(frontend + 0x218) == 2)
				Game::SumoFrontEnd_animate_443110(frontend, 0);
		}
	}
	std::string_view description() override
	{
		return "SumoUIFlashingTextFix";
	}

	bool validate() override
	{
		return (Settings::FramerateLimit == 0 || Settings::FramerateLimit > 60) && Settings::FramerateUnlockExperimental;
	}

	bool apply() override
	{

		constexpr int SumoFe_Animate_CallerAddr = 0x45C4E;
		Memory::VP::Nop(Module::exe_ptr(SumoFe_Animate_CallerAddr), 5);
		return true;
	}

};
SumoUIFlashingTextFix SumoUIFlashingTextFix::instance;

class ReplaceGameUpdateLoop : public Hook
{
	inline static double FramelimiterFrequency = 0;
	inline static double FramelimiterPrevCounter = 0;

	inline static double FramelimiterTargetFrametime = double(1000.f) / double(60.f);

	inline static double FramelimiterMaxDeviation = FramelimiterTargetFrametime / 160.f;
	inline static double FramelimiterDeviation = 0;

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		auto CurGameState = *Game::current_mode;

		// Skip framelimiter during load screens to help reduce load times
		bool skipFrameLimiter = Settings::FramerateLimit == 0;
		if (Settings::FramerateFastLoad > 0 && !skipFrameLimiter)
		{
			if (Settings::FramerateFastLoad != 3)
			{
				static bool isLoadScreenStarted = false;
				bool isLoadScreen = false;

				// Check if we're on load screen, if we are then disable framelimiter while game hasn't started (progress_code 65)
				if (CurGameState == STATE_START)
				{
					isLoadScreen = *Game::game_start_progress_code != 65;
				}

				skipFrameLimiter = isLoadScreen;

				// Toggle vsync if load screen state changed
				if (Settings::FramerateFastLoad > 1)
				{
					if (Game::D3DPresentParams->PresentationInterval != 0 && Game::D3DPresentParams->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE)
					{
						if (!isLoadScreenStarted && isLoadScreen)
						{
							auto NewParams = *Game::D3DPresentParams;
							NewParams.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

							Game::Sumo_D3DResourcesRelease();
							Game::D3DDevice()->Reset(&NewParams);
							Game::Sumo_D3DResourcesCreate();

							isLoadScreenStarted = true;
						}

						if (isLoadScreenStarted && !isLoadScreen)
						{
							Game::Sumo_D3DResourcesRelease();
							Game::D3DDevice()->Reset(Game::D3DPresentParams);
							Game::Sumo_D3DResourcesCreate();

							isLoadScreenStarted = false;
						}
					}
				}
			}
		}

		if (!skipFrameLimiter)
		{
			// Framelimiter
			double timeElapsed = 0;
			double timeCurrent = 0;
			LARGE_INTEGER counter;

			do
			{
				QueryPerformanceCounter(&counter);
				timeCurrent = double(counter.QuadPart) / FramelimiterFrequency;
				timeElapsed = timeCurrent - FramelimiterPrevCounter;

				if (Settings::FramerateFastLoad == 3)
					Game::FileLoad_Ctrl();

				if (Settings::FramerateLimitMode == 0) // "efficient" mode
				{
					if (FramelimiterTargetFrametime + FramelimiterDeviation <= timeElapsed)
					{
						FramelimiterDeviation = 0;
						break;
					}
					else if ((FramelimiterTargetFrametime + FramelimiterDeviation) - timeElapsed > 2.0)
						Sleep(1); // Sleep for ~1ms
					else
						Sleep(0); // Yield thread's time-slice (does not actually sleep)
				}
			} while (FramelimiterTargetFrametime + FramelimiterDeviation > timeElapsed);

			QueryPerformanceCounter(&counter);
			timeCurrent = double(counter.QuadPart) / FramelimiterFrequency;
			timeElapsed = timeCurrent - FramelimiterPrevCounter;

#if 0
			// Compensate for the deviation in the next frame (based on dxvk util_fps_limiter)
			double deviation = timeElapsed - FramelimiterTargetFrametime;
			FramelimiterDeviation += deviation;
			// Limit the cumulative deviation
			FramelimiterDeviation = std::clamp(FramelimiterDeviation, -FramelimiterMaxDeviation, FramelimiterMaxDeviation);
#endif

			FramelimiterPrevCounter = timeCurrent;
		}
		else
		{
			// Framelimiter is disabled, make sure to call FileLoad_Ctrl if we're using fastload3
			if (Settings::FramerateFastLoad == 3)
				Game::FileLoad_Ctrl();
		}

		Game::SetFrameStartCpuTime();

		int numUpdates = Game::CalcNumUpdatesToRun(60);

		// Vanilla game would always reset numUpdates to 1 if it was 0
		// when running above 60FPS CalcNumUpdatesToRun would return 0 since game was running fast, for it to skip the current update, but game would still force it to update
		// if FrameUnlockExperimental is set then we'll allow 0 updates to run, allowing it to skip updating when game is running fast

		int minUpdates = Settings::FramerateUnlockExperimental ? 0 : 1;

		if (numUpdates < minUpdates)
			numUpdates = minUpdates;

		// need to call 43FA10 in order for "extend time" gfx to disappear
		Game::fn43FA10(numUpdates);

		// Increment power_on_timer based on numUpdates value, fixes above-60-fps animation issues such as water anims
		// This should be incremented at the end of the games main loop, but we don't have any hook near the end
		// Incrementing at the beginning before the main loop body should be equivalent
		*Game::power_on_timer = *Game::power_on_timer + numUpdates;

		AudioHooks_Update(numUpdates);

		if (numUpdates > 0)
		{
			// Reset vibration if we're not in main game state
			if (Settings::VibrationMode != 0 && CurGameState != GameState::STATE_GAME)
				SetVibration(Settings::VibrationControllerId, 0.0f, 0.0f);

			if (Settings::ControllerHotPlug)
				DInput_RegisterNewDevices();
		}

		SumoUIFlashingTextFix::draw();

		for (int curUpdateIdx = 0; curUpdateIdx < numUpdates; curUpdateIdx++)
		{
			// Fetch latest input state
			// (do this inside our update-loop so that any hooked game funcs have accurate state...)
			Input::Update();

			Game::ReadIO();
			Game::SoundControl_mb();
			Game::LinkControlReceive();
			Game::ModeControl();
			Game::EventControl();
			Game::GhostCarExecServer();
			Game::fn4666A0();
		}
	}

	// Fixes animation rate of certain stage textures (beach waves / street lights...)
	// Vanilla game would add 1 to app_time every frame, new code will only add if a game tick is being ran on this frame
	// (as a bonus, this should also fix anim speed when running lower than 60FPS too)
	inline static SafetyHookInline SetTweeningTable = {};
	static void SetTweeningTable_dest()
	{
		if (*Game::sprani_num_ticks > 0)
		{
			*Game::app_time += *Game::sprani_num_ticks;
		}
	}

public:
	std::string_view description() override
	{
		return "ReplaceGameUpdateLoop";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		// framelimiter init
		{
			LARGE_INTEGER frequency;
			LARGE_INTEGER counter;

			typedef int(__stdcall* timeBeginPeriod_Fn) (int Period);
			auto timeBeginPeriod = (timeBeginPeriod_Fn)GetProcAddress(LoadLibraryA("winmm.dll"), "timeBeginPeriod");
			timeBeginPeriod(1);

			QueryPerformanceFrequency(&frequency);
			FramelimiterFrequency = double(frequency.QuadPart) / double(1000.f);
			QueryPerformanceCounter(&counter);
			FramelimiterPrevCounter = double(counter.QuadPart) / FramelimiterFrequency;

			FramelimiterTargetFrametime = double(1000.f) / double(Settings::FramerateLimit);
			FramelimiterMaxDeviation = FramelimiterTargetFrametime / 160.f;
		}

		constexpr int HookAddr = 0x17C7B;
		constexpr int GameLoopFrameLimiterAddr = 0x17DD3;
		constexpr int SetTweeningTable_Addr = 0xED60;
		constexpr int GameLoopFileLoad_CtrlCaller = 0x17D8D;

		// disable broken framelimiter
		Memory::VP::Nop(Module::exe_ptr(GameLoopFrameLimiterAddr), 2);

		// replace game update loop with custom version
		Memory::VP::Nop(Module::exe_ptr<uint8_t>(HookAddr), 0xA3);
		dest_hook = safetyhook::create_mid(Module::exe_ptr<uint8_t>(HookAddr), destination);

		// disable power_on_timer increment so we can handle it
		Memory::VP::Nop(Module::exe_ptr<uint8_t>(0x17D87), 6);

		// Disable FileLoad_Ctrl call, we'll handle it above ourselves
		if (Settings::FramerateFastLoad == 3)
			Memory::VP::Nop(Module::exe_ptr(GameLoopFileLoad_CtrlCaller), 5);

		if (Settings::FramerateUnlockExperimental)
			SetTweeningTable = safetyhook::create_inline(Module::exe_ptr<uint8_t>(SetTweeningTable_Addr), SetTweeningTable_dest);

		return !!dest_hook;
	}

	static ReplaceGameUpdateLoop instance;
};
ReplaceGameUpdateLoop ReplaceGameUpdateLoop::instance;

class FullscreenRefreshRate : public Hook
{
	const static int PatchAddr = 0xE9B9;

public:
	std::string_view description() override
	{
		return "FullscreenRefreshRate";
	}

	bool validate() override
	{
		return Settings::FramerateLimit != 60;
	}

	bool apply() override
	{
		auto* patch_addr = Module::exe_ptr<uint8_t>(PatchAddr);
		Memory::VP::Patch(patch_addr, Settings::FramerateLimit);

		return true;
	}

	static FullscreenRefreshRate instance;
};
FullscreenRefreshRate FullscreenRefreshRate::instance;
