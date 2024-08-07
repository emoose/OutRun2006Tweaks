#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

#include <d3d9.h>

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

		AudioHooks_Update(numUpdates);

		if (numUpdates > 0)
		{
			// Reset vibration if we're not in main game state
			if (Settings::VibrationMode != 0 && CurGameState != GameState::STATE_GAME)
				SetVibration(Settings::VibrationControllerId, 0.0f, 0.0f);

			if (Settings::ControllerHotPlug)
				DInput_RegisterNewDevices();
		}

		for (int curUpdateIdx = 0; curUpdateIdx < numUpdates; curUpdateIdx++)
		{
			// Fetch latest input state
			// (do this inside our update-loop so that any hooked game funcs have accurate state...)
			Input::PadUpdate(Settings::VibrationControllerId);

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
