#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

#include <d3d9.h>

class ReplaceGameUpdateLoop : public Hook
{
	const static int HookAddr = 0x17C7B;
	const static int GameLoopFrameLimiterAddr = 0x17DD3;

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
		// TODO: currently only detects loading into a stage, loading back to main menu isn't detected
		//   (need to find a way to check the sumo menu state?)

		bool skipFrameLimiter = Settings::FramerateLimit == 0;
		if (!skipFrameLimiter && Settings::FramerateFastLoad && CurGameState == STATE_START)
		{
			auto game_start_progress_code = *Module::exe_ptr<int>(0x4367A8);
			skipFrameLimiter = game_start_progress_code != 65;
		}

		// Framelimiter
		if (!skipFrameLimiter)
		{
			double timeElapsed = 0;
			double timeCurrent = 0;
			LARGE_INTEGER counter;

			do
			{
				QueryPerformanceCounter(&counter);
				timeCurrent = double(counter.QuadPart) / FramelimiterFrequency;
				timeElapsed = timeCurrent - FramelimiterPrevCounter;

				if (FramelimiterTargetFrametime + FramelimiterDeviation <= timeElapsed)
				{
					FramelimiterDeviation = 0;
					break;
				}
				else if ((FramelimiterTargetFrametime + FramelimiterDeviation) - timeElapsed > 2.0)
					Sleep(1); // Sleep for ~1ms
				else
					Sleep(0); // Yield thread's time-slice (does not actually sleep)
			} while (FramelimiterTargetFrametime + FramelimiterDeviation > timeElapsed);

			QueryPerformanceCounter(&counter);
			timeCurrent = double(counter.QuadPart) / FramelimiterFrequency;
			timeElapsed = timeCurrent - FramelimiterPrevCounter;

			// Compensate for the deviation in the next frame (based on dxvk util_fps_limiter)
			double deviation = timeElapsed - FramelimiterTargetFrametime;
			FramelimiterDeviation += deviation;
			// Limit the cumulative deviation
			FramelimiterDeviation = std::clamp(FramelimiterDeviation, -FramelimiterMaxDeviation, FramelimiterMaxDeviation);

			FramelimiterPrevCounter = timeCurrent;
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

		for (int curUpdateIdx = 0; curUpdateIdx < numUpdates; curUpdateIdx++)
		{
			Game::ReadIO();
			Game::SoundControl_mb();
			Game::LinkControlReceive();
			Game::ModeControl();
			Game::EventControl();
			Game::GhostCarExecServer();
			Game::fn4666A0();
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

		// disable broken framelimiter
		Memory::VP::Nop(Module::exe_ptr(GameLoopFrameLimiterAddr), 2);

		// replace game update loop with custom version
		Memory::VP::Nop(Module::exe_ptr<uint8_t>(HookAddr), 0xA3);
		dest_hook = safetyhook::create_mid(Module::exe_ptr<uint8_t>(HookAddr), destination);
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
