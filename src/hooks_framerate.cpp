#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "overlay/overlay.hpp"

// from timeapi.h, which we can't include since our proxy timeBeginPeriod etc funcs will conflict...
typedef struct timecaps_tag {
	UINT    wPeriodMin;     /* minimum period supported  */
	UINT    wPeriodMax;     /* maximum period supported  */
} TIMECAPS;

#include <d3d9.h>

class Snooze
{
	// Based on https://github.com/blat-blatnik/Snippets/blob/main/precise_sleep.c

	static inline HANDLE Timer;
	static inline int SchedulerPeriodMs;
	static inline INT64 QpcPerSecond;

public:
	static void PreciseSleep(double seconds)
	{
		LARGE_INTEGER qpc;
		QueryPerformanceCounter(&qpc);
		INT64 targetQpc = (INT64)(qpc.QuadPart + seconds * QpcPerSecond);

		if (Timer) // Try using a high resolution timer first.
		{
			const double TOLERANCE = 0.001'02;
			INT64 maxTicks = (INT64)SchedulerPeriodMs * 9'500;
			for (;;) // Break sleep up into parts that are lower than scheduler period.
			{
				double remainingSeconds = (targetQpc - qpc.QuadPart) / (double)QpcPerSecond;
				INT64 sleepTicks = (INT64)((remainingSeconds - TOLERANCE) * 10'000'000);
				if (sleepTicks <= 0)
					break;

				LARGE_INTEGER due;
				due.QuadPart = -(sleepTicks > maxTicks ? maxTicks : sleepTicks);
				SetWaitableTimerEx(Timer, &due, 0, NULL, NULL, NULL, 0);
				WaitForSingleObject(Timer, INFINITE);
				QueryPerformanceCounter(&qpc);
			}
		}
		else // Fallback to Sleep.
		{
			const double TOLERANCE = 0.000'02;
			double sleepMs = (seconds - TOLERANCE) * 1000 - SchedulerPeriodMs; // Sleep for 1 scheduler period less than requested.
			int sleepSlices = (int)(sleepMs / SchedulerPeriodMs);
			if (sleepSlices > 0)
				Sleep((DWORD)sleepSlices * SchedulerPeriodMs);
			QueryPerformanceCounter(&qpc);
		}

		while (qpc.QuadPart < targetQpc) // Spin for any remaining time.
		{
			YieldProcessor();
			QueryPerformanceCounter(&qpc);
		}
	}

	static void Init(void)
	{
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 4
#endif
		// Prevent timer resolution getting reset on Win11
		// https://stackoverflow.com/questions/77182958/windows-11-application-timing-becomes-uneven-when-backgrounded
		// (SPI call will silently fail on other OS)
		PROCESS_POWER_THROTTLING_STATE state = { 0 };
		state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
		state.StateMask = 0;
		SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));

		typedef int(__stdcall* timeBeginPeriod_Fn) (int Period);
		typedef int(__stdcall* timeGetDevCaps_Fn) (TIMECAPS* ptc, UINT cbtc);

		auto winmm = LoadLibraryA("winmm.dll");
		auto timeBeginPeriod = (timeBeginPeriod_Fn)GetProcAddress(winmm, "timeBeginPeriod");
		auto timeGetDevCaps = (timeGetDevCaps_Fn)GetProcAddress(winmm, "timeGetDevCaps");

		// Initialization
		Timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		TIMECAPS caps;
		timeGetDevCaps(&caps, sizeof caps);
		timeBeginPeriod(caps.wPeriodMin);
		SchedulerPeriodMs = (int)caps.wPeriodMin;
		LARGE_INTEGER qpf;
		QueryPerformanceFrequency(&qpf);
		QpcPerSecond = qpf.QuadPart;
	}
};

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

			double FramelimiterTargetFrametime = double(1000.f) / double(Settings::FramerateLimit);
			double FramelimiterMaxDeviation = FramelimiterTargetFrametime / (16.f * 1000.f);

			do
			{
				if (Settings::FramerateFastLoad == 3)
					Game::FileLoad_Ctrl();

				QueryPerformanceCounter(&counter);
				timeCurrent = double(counter.QuadPart) / FramelimiterFrequency;
				timeElapsed = timeCurrent - FramelimiterPrevCounter;

				if (Settings::FramerateLimitMode == 0) // "efficient" mode
				{
					if (FramelimiterTargetFrametime + FramelimiterDeviation <= timeElapsed)
					{
						FramelimiterDeviation = 0;
						break;
					}
					else if ((FramelimiterTargetFrametime + FramelimiterDeviation) - timeElapsed > 2.0)
						Snooze::PreciseSleep(1.f / 1000.f); // Sleep for ~1ms
					else
						Sleep(0); // Yield thread's time-slice (does not actually sleep)
				}
			} while (FramelimiterTargetFrametime + FramelimiterDeviation > timeElapsed);

			QueryPerformanceCounter(&counter);
			timeCurrent = double(counter.QuadPart) / FramelimiterFrequency;
			timeElapsed = timeCurrent - FramelimiterPrevCounter;

			// Compensate for any deviation, in the next frame (based on dxvk util_fps_limiter)
			double deviation = timeElapsed - FramelimiterTargetFrametime;
			FramelimiterDeviation += deviation;
			// Limit the cumulative deviation
			FramelimiterDeviation = std::clamp(FramelimiterDeviation, -FramelimiterMaxDeviation, FramelimiterMaxDeviation);

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

			if (!Overlay::IsActive || Overlay::IsBindingDialogActive)
			{
				void InputManager_Update();
				InputManager_Update();
			}

			if (!Overlay::IsActive)
			{
				Game::ReadIO();
			}

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

	// EventDisplay adds 0.10471975 to sin_param every frame, if GetPauseFlag is false
	// This causes speed of flashing cars to change depending on framerate
	// We'll update it similar to SetTweeningTable so it only increments if a game tick is being ran
	// TODO: would probably be smoother to scale that 0.10471975 by deltatime instead
	inline static SafetyHookMid EventDisplay_midhook1 = {};
	inline static SafetyHookMid EventDisplay_midhook2 = {};
	inline static SafetyHookMid DispPlCar_midhook = {};
	static void EventDisplay_dest(SafetyHookContext& ctx)
	{
		if (*Game::sprani_num_ticks == 0)
			ctx.eax = 1; // make func skip adding to sin_param
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
			Snooze::Init();

			LARGE_INTEGER frequency;
			LARGE_INTEGER counter;

			QueryPerformanceFrequency(&frequency);
			FramelimiterFrequency = double(frequency.QuadPart) / double(1000.f);
			QueryPerformanceCounter(&counter);
			FramelimiterPrevCounter = double(counter.QuadPart) / FramelimiterFrequency;
		}

		constexpr int HookAddr = 0x17C7B;
		constexpr int GameLoopFrameLimiterAddr = 0x17DD3;
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
		{
			constexpr int SetTweeningTable_Addr = 0xED60;
			SetTweeningTable = safetyhook::create_inline(Module::exe_ptr(SetTweeningTable_Addr), SetTweeningTable_dest);

			constexpr int EventDisplay_HookAddr1 = 0x3FC48;
			constexpr int EventDisplay_HookAddr2 = 0x3FE51;
			constexpr int DispPlCar_HookAddr = 0x6BE27;
			EventDisplay_midhook1 = safetyhook::create_mid(Module::exe_ptr(EventDisplay_HookAddr1), EventDisplay_dest);
			EventDisplay_midhook2 = safetyhook::create_mid(Module::exe_ptr(EventDisplay_HookAddr2), EventDisplay_dest);
			DispPlCar_midhook = safetyhook::create_mid(Module::exe_ptr(DispPlCar_HookAddr), EventDisplay_dest);
		}

		// Increase reflection update rate, default is 3 (30fps)
		// Set it to framerate limit div 10 (add 9 to make it round up to nearest 10)
		int numUpdates = (Settings::FramerateLimit + 9) / 10;
		if (numUpdates > 3)
		{
			constexpr int Envmap_RenderToCubeMap_PatchAddr = 0x1447E;
			Memory::VP::Nop(Module::exe_ptr(Envmap_RenderToCubeMap_PatchAddr), 2);

			constexpr int Envmap_RenderToCubeMap_PatchAddr2 = 0x14480 + 1;
			Memory::VP::Patch(Module::exe_ptr(Envmap_RenderToCubeMap_PatchAddr2), numUpdates);
		}

		return !!dest_hook;
	}

	static ReplaceGameUpdateLoop instance;
};
ReplaceGameUpdateLoop ReplaceGameUpdateLoop::instance;

class FullscreenRefreshRate : public Hook
{

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
		constexpr int PatchAddr = 0xE9B9;
		Memory::VP::Patch(Module::exe_ptr<uint8_t>(PatchAddr), Settings::FramerateLimit);

		return true;
	}

	static FullscreenRefreshRate instance;
};
FullscreenRefreshRate FullscreenRefreshRate::instance;
