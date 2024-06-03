#include "hook_mgr.hpp"
#include "plugin.hpp"

#include <d3d9.h>

class ReplaceGameUpdateLoop : public Hook
{
	const static int HookAddr = 0x17C7B;

	inline static double FramelimiterFrequency = 0;
	inline static double FramelimiterPrevCounter = 0;

	inline static double FramelimiterTargetFrametime = double(1000.f) / double(60.f);

	inline static double FramelimiterMaxDeviation = FramelimiterTargetFrametime / 160.f;
	inline static double FramelimiterDeviation = 0;

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		auto SetFrameStartCpuTime = (fn_0args)0x449430;
		auto CalcNumUpdatesToRun = (fn_1arg_int)0x417890;

		auto CurGameState = *Module::exe_ptr<GameState>(0x38026C);

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

			// Compensate for the deviation in the next frame
			double deviation = timeElapsed - FramelimiterTargetFrametime;
			FramelimiterDeviation += deviation;
			// Limit the cumulative deviation
			FramelimiterDeviation = std::clamp(FramelimiterDeviation, -FramelimiterMaxDeviation, FramelimiterMaxDeviation);

			FramelimiterPrevCounter = timeCurrent;
		}

		SetFrameStartCpuTime();

		int numUpdates = CalcNumUpdatesToRun(60);

		// Vanilla game would always reset numUpdates to 1 if it was 0
		// when running above 60FPS CalcNumUpdatesToRun would return 0 since game was running fast, for it to skip the current update, but game would still force it to update
		// if FrameUnlockExperimental is set then we'll allow 0 updates to run, allowing it to skip updating when game is running fast

		int minUpdates = Settings::FramerateUnlockExperimental ? 0 : 1;

		if (numUpdates < minUpdates)
			numUpdates = minUpdates;

		// need to call 43FA10 in order for "extend time" gfx to disappear
		auto fn43FA10 = (fn_1arg)0x43FA10;
		fn43FA10(numUpdates);

		auto ReadIO = (fn_0args)0x453BB0; // ReadIO
		auto SoundControl_mb = (fn_0args)0x42F330; // SoundControl_mb
		auto LinkControlReceive = (fn_0args)0x455130; // LinkControlReceive
		auto ModeControl = (fn_0args)0x43FA20; // ModeControl
		auto EventControl = (fn_0args)0x43FAB0; // EventControl
		auto GhostCarExecServer = (fn_0args)0x480F80; // GhostCarExecServer
		auto fn4666A0 = (fn_0args)0x4666A0;

		for (int curUpdateIdx = 0; curUpdateIdx < numUpdates; curUpdateIdx++)
		{
			ReadIO();
			SoundControl_mb();
			LinkControlReceive();
			ModeControl();
			EventControl();
			GhostCarExecServer();
			fn4666A0();
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
		LARGE_INTEGER frequency;
		LARGE_INTEGER counter;

		typedef int(__stdcall* timeBeginPeriod_Fn) (int Period);
		timeBeginPeriod_Fn timeBeginPeriod_actual = (timeBeginPeriod_Fn)GetProcAddress(LoadLibraryA("winmm.dll"), "timeBeginPeriod");
		timeBeginPeriod_actual(1);

		QueryPerformanceFrequency(&frequency);
		FramelimiterFrequency = double(frequency.QuadPart) / double(1000.f);
		QueryPerformanceCounter(&counter);
		FramelimiterPrevCounter = double(counter.QuadPart) / FramelimiterFrequency;

		FramelimiterTargetFrametime = double(1000.f) / double(Settings::FramerateLimit);
		FramelimiterMaxDeviation = FramelimiterTargetFrametime / 160.f;

		Memory::VP::Nop(Module::exe_ptr<uint8_t>(HookAddr), 0xA3);
		dest_hook = safetyhook::create_mid(Module::exe_ptr<uint8_t>(HookAddr), destination);
		return true;
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
