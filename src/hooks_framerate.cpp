#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "overlay/overlay.hpp"
#include "interpolation.hpp"

// from timeapi.h, which we can't include since our proxy timeBeginPeriod etc funcs will conflict...
typedef struct timecaps_tag {
	UINT    wPeriodMin;     /* minimum period supported  */
	UINT    wPeriodMax;     /* maximum period supported  */
} TIMECAPS;

#include <d3d9.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace Settings
{
	Setting<int> FramerateLimit{ "Performance", "FramerateLimit", 0,
		"0 will disable framelimiter and rely on vsync to match your monitor refresh rate instead (or an external limiter). "
		"Framerates above 60 will duplicate frames, unless FramerateInterpolation is also enabled." };
	Setting<int> FramerateFastLoad{ "Performance", "FramerateFastLoad", 3,
		"Unlimits framerate during load screens to help reduce load times.",
		{ "Disable", "Unlimit framerate during load screens", "Unlimit framerate & disable vsync (may cause screen flash)",
		  "(fastest) Process files during framelimit period" } };
	Setting<bool> FramerateInterpolation{ "Performance", "FramerateInterpolation", true,
		"Smooths car & camera movement when running above 60FPS, by interpolating positions between game ticks. "
		"Requires FramerateLimit to be set above 60 (or set to 0). EXPERIMENTAL: may cause visual glitches, disable if you notice any." };
	Setting<bool> FramerateLimitMode{ "Performance", "FramerateLimitMode", false,
		"Off is efficient mode, on is accurate mode. Efficient should work fine for most people, but if you have issues it might be worth trying accurate mode." };
	Setting<bool> FramerateUnlockExperimental{ "Performance", "FramerateUnlockExperimental", true,
		"Allows the game to render more than one frame per 60Hz game tick, which everything above 60FPS depends on." };
}

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

//
// Every 2D draw in the game, sprites and individual text glyphs alike, is
// queued rather than drawn immediately: draw_entried_sprites walks the queue
// once per rendered frame, drawing and unlinking as it goes, a queued draw 
// only survives that frame.
//
// Most of the game queues draws from a _Disp callback, which runs every rendered
// frame. The Sumo front-end UI queues from its _Ctrl instead, which only runs
// on a game sim tick.
// Above 60FPS not every frame runs a tick, and on the frames that don't, nothing 
// queues that UI draw at all, causing a flicker to show on some UI elements where 
// the draw had been skipped.
// (text entry UI, showroom purchase prompt, sign-in status text)
//
// Rather than trying to hook all the individual callers (which the Sumo UI has 
// dozens of, behind different vtables), this works on the queue itself.
// Immediately after the sim tick the queue only holds whatever the sim tick had
// chosen to draw, and nothing else, since the render path has not run yet and the 
// last frame's entries were already unlinked at the end of it.
//
// So we just snapshot there on frames that tick, and re-queue that snapshot on frames
// that don't.
//
namespace SumoUISpriteReplay
{
	// One captured draw. kind_C selects which payload struct the draw uses and
	// the other is left zeroed, so keeping both avoids interpreting either.
	struct Entry
	{
		float priority;
		uint32_t kind;
		SPRARGS args;
		SPRARGS2 args2;
	};

	static Entry Captured[Game::SpriteNodeMax];
	static int CapturedCount = 0;

	static bool available()
	{
		return Game::sprite_prio_root && Game::put_sprite_ex;
	}

	// Copies out every pending draw. The walk matches draw_entried_sprites: one
	// list head per priority, first node at next_0, then next_0 node to node.
	static void capture()
	{
		CapturedCount = 0;
		if (!available())
			return;

		for (int prio = 0; prio < Game::SpritePriorityCount; prio++)
		{
			const SpriteNode* root = Game::sprite_prio_root[prio];
			if (!root)
				continue;

			for (const SpriteNode* node = root->next_0; node; node = node->next_0)
			{
				// The node pool is SpriteNodeMax entries, so the queue can't exceed it.
				if (CapturedCount >= Game::SpriteNodeMax)
					return;

				Entry& entry = Captured[CapturedCount++];
				entry.priority = float(prio);
				entry.kind = node->kind_C;
				entry.args = node->args_10;
				entry.args2 = node->args2_58;
			}
		}
	}

	// Puts the captured draws back, in capture order and at the same point in the
	// frame the originals were queued, so draws sharing a priority keep their
	// relative order.
	//
	// Nodes are allocated by calling put_sprite_ex rather than by hand, to keep
	// the game's pool bookkeeping correct, then overwritten with the captured
	// body. It is only ever given a throwaway copy: hooks_textures hooks it and
	// rewrites the SPRARGS in place to remap UVs onto a replacement texture,
	// which a captured draw has already had done to it once.
	static void replay()
	{
		if (!available())
			return;

		for (int i = 0; i < CapturedCount; i++)
		{
			const Entry& entry = Captured[i];
			const int prio = int(entry.priority);

			const SpriteNode* before = Game::sprite_prio_root[prio];
			const SpriteNode* tailBefore = before ? before->tail_4 : nullptr;

			SPRARGS scratch = entry.args;
			Game::put_sprite_ex(&scratch, entry.priority);

			// tail_4 is the node linked last, so an unchanged tail means the
			// pool was full and nothing was linked.
			SpriteNode* root = Game::sprite_prio_root[prio];
			SpriteNode* node = root ? root->tail_4 : nullptr;
			if (!node || node == tailBefore)
				continue;

			node->kind_C = entry.kind;
			node->args_10 = entry.args;
			node->args2_58 = entry.args2;
		}
	}

	// Call once per rendered frame, after the tick loop, before the render path.
	static void update(int numUpdates)
	{
		if (numUpdates > 0)
			capture();
		else
			replay();
	}
}

bool IsKeyPressed(int vk)
{
	static bool prev[256] = {};

	bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
	bool pressed = down && !prev[vk];
	prev[vk] = down;
	return pressed;
}

class ReplaceGameUpdateLoop : public Hook
{
	inline static double FramelimiterFrequency = 0;

	// Time the previous frame should ideally have been released at, in ms. This
	// advances by exactly one frame interval each time rather than snapping to
	// when the wait actually finished, so a wait that runs long is made up by
	// the next frame instead of pushing every later frame back.
	inline static double FramelimiterPrevCounter = 0;

	// Snooze::PreciseSleep spins out the last millisecond or so of a request
	// itself, so anything shorter than that spins for its whole duration.
	static constexpr double FramelimiterMinSleepMs = 2.0;

	// FramerateFastLoad 3 leaves the limiter running during loads and pumps the
	// file loader while it waits, so its sleeps are capped short enough to keep
	// coming back to it.
	static constexpr double FramelimiterFastLoadSleepMs = 4.0;

	// Longest a single FileLoad_Ctrl can take. LoadXmtsetObject spends up to half
	// a millisecond building objects or textures before returning, and the other
	// request servers it drives are bounded the same way.
	static constexpr double FileLoadSliceMs = 0.5;

	// FileLoad_Ctrl returns zero once every request server has run dry, so one
	// call answers immediately when there is nothing to load and today's single
	// call per sleep is all an idle frame pays. While a load is running though
	// that one call advances it by a slice and then the limiter sleeps for
	// milliseconds, leaving the loader idle for most of the wait. Keep handing it
	// slices until it reports nothing left or the frame no longer has room for
	// another, so the limiter still releases the frame on time.
	static void PumpFileLoader(double deadline)
	{
		LARGE_INTEGER counter;

		for (;;)
		{
			if (!Game::FileLoad_Ctrl())
				return;

			QueryPerformanceCounter(&counter);
			if (deadline - (double(counter.QuadPart) / FramelimiterFrequency) <= FileLoadSliceMs)
				return;
		}
	}

	inline static SafetyHookMid dest_hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		auto CurGameState = *Game::current_mode;

		// TEMP: Allow toggling interpolation with K key
		if (IsKeyPressed('K'))
		{
			Settings::FramerateInterpolation = !Settings::FramerateInterpolation;
			Interp::Reset();
		}

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
			double timeCurrent = 0;
			LARGE_INTEGER counter;

			const double FramelimiterTargetFrametime = 1000.0 / double(Settings::FramerateLimit);
			const double deadline = FramelimiterPrevCounter + FramelimiterTargetFrametime;

			for (;;)
			{
				if (Settings::FramerateFastLoad == 3)
					PumpFileLoader(deadline);

				QueryPerformanceCounter(&counter);
				timeCurrent = double(counter.QuadPart) / FramelimiterFrequency;

				double remaining = deadline - timeCurrent;
				if (remaining <= 0.0)
					break;

				if (Settings::FramerateLimitMode != 0) // busy-wait mode
				{
					YieldProcessor();
					continue;
				}

				if (Settings::FramerateFastLoad == 3)
					remaining = min(remaining, FramelimiterFastLoadSleepMs);

				// PreciseSleep returns right on the deadline, having spun out
				// the last part of the wait, so this normally runs once.
				if (remaining > FramelimiterMinSleepMs)
					Snooze::PreciseSleep(remaining / 1000.0);
				else
					Sleep(0); // Yield thread's time-slice (does not actually sleep)
			}

			FramelimiterPrevCounter += FramelimiterTargetFrametime;

			// A stall (loading, alt-tab) leaves the ideal timeline far behind.
			// Resync rather than releasing a burst of uncapped frames to catch up.
			if (timeCurrent - FramelimiterPrevCounter > FramelimiterTargetFrametime)
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

		for (int curUpdateIdx = 0; curUpdateIdx < numUpdates; curUpdateIdx++)
		{
			Interp::BeforeTick();

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

			Interp::AfterTick();
		}

		// Keeps tick-drawn UI present on frames that skip a tick. Must sit after
		// the last tick and before the render path queues any draw of its own.
		if (Settings::FramerateUnlockExperimental)
			SumoUISpriteReplay::update(numUpdates);

		// Re-run the display-matrix builders with a fractional alpha. Must be
		// the last thing we do: the mid-hook returns straight into the game's
		// render path (sub_454670 / BeginScene / SceneControl).
		//
		// Only meaningful when the experimental unlock is on - otherwise
		// numUpdates is clamped to >=1, alpha sits near 0, and we would render
		// a frame behind rather than smoothly between frames.
		if (Settings::FramerateUnlockExperimental && Settings::FramerateInterpolation)
			Interp::AfterTicks(FramelimiterFrequency);
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

	// GameOthCar_Disp - a *display* function, which for some reason alters
	// the EVWORK_CAR::field_B62/field_B64 timers, and also flips the
	// flash-parity byte there. All three are gameplay state read from tick code:
	//
	//   SetCollisionImpactPlcar  sets B64 = 90 (or 60) and flags_C5C |= 1 when
	//                            the player rams a traffic car
	//   ColiCar_CheckPl2Oth      skips car-to-car collision while B64 != 0
	//   CloseOthcarEventByVanishReq  EventCloses the car once B64 <= 0
	//
	// So a hit car flashes, turns non-solid, then despawns, but this is all paced
	// by the render code, not the tick code - above 60FPS this causes cars to vanish
	// sooner.
	//
	// Rather than patch the instructions out, pre-adjust the field so the game's
	// own dec/xor lands on the value a 60Hz tick rate would have produced. That
	// keeps every other read of these fields untouched.
	inline static SafetyHookMid OthCarTimer_midhook1 = {};
	inline static SafetyHookMid OthCarTimer_midhook2 = {};
	inline static SafetyHookMid OthCarFlash_midhook = {};

	static void OthCarTimer_scale(uintptr_t car, int fieldOffset)
	{
		int16_t* counter = reinterpret_cast<int16_t*>(car + fieldOffset);

		int value = *counter - *Game::sprani_num_ticks;
		if (value < 0)
			value = 0; // several readers test <= 0, never let it run past zero

		// +1 because the dec we're standing on is about to take one off.
		*counter = int16_t(value + 1);
	}

	static void OthCarTimerB62_dest(SafetyHookContext& ctx)
	{
		OthCarTimer_scale(ctx.esi, 0xB62);
	}

	static void OthCarTimerB64_dest(SafetyHookContext& ctx)
	{
		OthCarTimer_scale(ctx.esi, 0xB64);
	}

	static void OthCarFlash_dest(SafetyHookContext& ctx)
	{
		// The xor below flips the parity byte once per call and the car is drawn
		// on every other flip, so the flash rate tracks the render rate.
		// Undo the flip on frames that ran an even number of ticks (usually zero) to
		// hold it at the vanilla 30Hz. bl is 0 while paused, which no-ops both
		// the game's xor and ours.
		if ((*Game::sprani_num_ticks & 1) == 0)
			*reinterpret_cast<uint8_t*>(ctx.esi + 0xB53) ^= uint8_t(ctx.ebx);
	}

	// DispNextStageInfo, a _Disp, increments stage_info_timer every render frame
	// Hooked code to only increment on actual sim-tick frames.
	inline static SafetyHookMid StageInfoTimer_midhook1 = {};
	inline static SafetyHookMid StageInfoTimer_midhook2 = {};
	static void StageInfoTimer_dest(SafetyHookContext& ctx)
	{
		if (*Game::sprani_num_ticks > 0)
			*Game::stage_info_timer += 1;
	}

	// DispScore draws the popup turning a passed car into "PASS!" and its score.
	// Its state is asd_tbl, and every part of an entry advances once per call:
	// field_C counts 140 down and picks the frame as 140 - field_C, field_10 eases,
	// field_14 rises then fades. DispScore runs from NaviPub_Disp, a _Disp, so that
	// is per rendered frame and the popup passes too fast above 60FPS. Advances are
	// undone on frames without a tick rather than scaled by sprani_num_ticks, since
	// sparkles spawn on exact values of field_C.
	inline static SafetyHookMid ScorePopupPos_midhook = {};
	static void ScorePopupPos_dest(SafetyHookContext& ctx)
	{
		// eax is the new position, about to overwrite field_10.
		if (*Game::sprani_num_ticks == 0)
			ctx.eax = *reinterpret_cast<uint32_t*>(ctx.esi - 4);
	}

	// field_14's add and multiply are NOPed and reapplied here, since undoing a
	// float in place would drift.
	inline static SafetyHookMid ScorePopupRise_midhook = {};
	static void ScorePopupRise_dest(SafetyHookContext& ctx)
	{
		if (*Game::sprani_num_ticks > 0)
			*reinterpret_cast<float*>(ctx.esi) += 0.1f;
	}

	inline static SafetyHookMid ScorePopupFade_midhook = {};
	static void ScorePopupFade_dest(SafetyHookContext& ctx)
	{
		if (*Game::sprani_num_ticks > 0)
			*reinterpret_cast<float*>(ctx.esi) *= 0.8f;
	}

	inline static SafetyHookMid ScorePopupTimer_midhook = {};
	static void ScorePopupTimer_dest(SafetyHookContext& ctx)
	{
		// The dec after this reloads field_C, so adding one cancels it.
		if (*Game::sprani_num_ticks == 0)
			*reinterpret_cast<int32_t*>(ctx.esi - 8) += 1;
	}

	// field_C now holds still for several frames, so sparkle spawns keyed to it
	// would repeat and drain the 64 entry sprani list. Returning -1 is what
	// sprani_add_list does when full, and sprani_set_matrix ignores a negative id.
	// Nothing else uses 0x2B0046.
	inline static SafetyHookInline SpraniAddList_hook = {};
	static int __cdecl SpraniAddList_dest(uint32_t id, int a2, int a3)
	{
		if (id == 0x2B0046 && *Game::sprani_num_ticks == 0)
			return -1;

		return SpraniAddList_hook.ccall<int>(id, a2, a3);
	}

	// A SumoSprite slides and stretches into place from a rate per second that
	// update_move_tween and update_scale_tween multiply by this, the milliseconds
	// since the last rendered frame.
	// Race announcements step theirs from a _Ctrl, one call per 60Hz tick, so above 
	// 60FPS a step covers only one frame of the time it stands for, causing the tween 
	// to crawl extremely slowly at high FPS.
	// The frame's tick count suits both those and the selector screens, which step once per _Disp.
	inline static SafetyHookInline SumoSpriteDeltaTime = {};
	static double __cdecl SumoSpriteDeltaTime_dest()
	{
		return double(*Game::sprani_num_ticks) * (1000.0 / 60.0);
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

	void declare_settings() override
	{
		Settings::FramerateInterpolation.watch([] { Interp::Reset(); });
		Settings::FramerateUnlockExperimental.needs_restart();
		Settings::FramerateFastLoad.needs_restart([] {
			// Mode 3 nops the FileLoad_Ctrl call and patches LoadADVData to retn.
			// 0, 1 and 2 are only tested by the loop, so they interchange freely.
			return (Settings::FramerateFastLoad.startup_value() == 3) != (Settings::FramerateFastLoad.get() == 3);
		});
		// Input::Update is driven from this loop, and latches the key it parses.
		Settings::HudToggleKey.needs_restart();
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
		{
			Memory::VP::Nop(Module::exe_ptr(GameLoopFileLoad_CtrlCaller), 5);

			// FileLoad_Ctrl reports whether any of its five request servers still
			// has work, which is what PumpFileLoader stops on, but the last of them
			// answers "yes" every time:
			//
			//   if (file_load_progress[1] != 2) LoadADVData(load_index);
			//   return 1;
			//
			// LoadADVData already returns 0 for that state, and 1 only while it has
			// an ADV file open or waiting, so dropping the test and handing back
			// what it says makes the whole chain honest. Without this the pump never
			// sees an idle loader and spins out every frame.
			constexpr int LoadADVSkipTest_Addr = 0x5AEC7;
			constexpr int LoadADVReturnOne_Addr = 0x5AED7;

			Memory::VP::Nop(Module::exe_ptr(LoadADVSkipTest_Addr), 2);
			Memory::VP::Patch<uint8_t>(Module::exe_ptr(LoadADVReturnOne_Addr), 0xC3); // retn
		}

		if (Settings::FramerateUnlockExperimental)
		{
			if (Settings::FramerateInterpolation)
			{
				Interp::Apply();
			}

			constexpr int SetTweeningTable_Addr = 0xED60;
			SetTweeningTable = safetyhook::create_inline(Module::exe_ptr(SetTweeningTable_Addr), SetTweeningTable_dest);

			constexpr int EventDisplay_HookAddr1 = 0x3FC48;
			constexpr int EventDisplay_HookAddr2 = 0x3FE51;
			constexpr int DispPlCar_HookAddr = 0x6BE27;
			EventDisplay_midhook1 = safetyhook::create_mid(Module::exe_ptr(EventDisplay_HookAddr1), EventDisplay_dest);
			EventDisplay_midhook2 = safetyhook::create_mid(Module::exe_ptr(EventDisplay_HookAddr2), EventDisplay_dest);
			DispPlCar_midhook = safetyhook::create_mid(Module::exe_ptr(DispPlCar_HookAddr), EventDisplay_dest);

			OthCarTimer_midhook1 = safetyhook::create_mid(Module::exe_ptr(0xAE65C), OthCarTimerB62_dest);
			OthCarTimer_midhook2 = safetyhook::create_mid(Module::exe_ptr(0xAE6A9), OthCarTimerB64_dest);
			OthCarFlash_midhook = safetyhook::create_mid(Module::exe_ptr(0xAE6B0), OthCarFlash_dest);

			constexpr int SumoSpriteDeltaTime_Addr = 0x49E30;
			SumoSpriteDeltaTime = safetyhook::create_inline(Module::exe_ptr(SumoSpriteDeltaTime_Addr), SumoSpriteDeltaTime_dest);

			// Fix "Next Stage" not blinking at high FPS
			Memory::VP::Nop(Module::exe_ptr<uint8_t>(0xB9554), 7);
			Memory::VP::Nop(Module::exe_ptr<uint8_t>(0xB960D), 7);
			StageInfoTimer_midhook1 = safetyhook::create_mid(Module::exe_ptr(0xB9554), StageInfoTimer_dest);
			StageInfoTimer_midhook2 = safetyhook::create_mid(Module::exe_ptr(0xB960D), StageInfoTimer_dest);

			// Fix car-pass score animation speed
			ScorePopupPos_midhook = safetyhook::create_mid(Module::exe_ptr(0xBD6BA), ScorePopupPos_dest);
			Memory::VP::Nop(Module::exe_ptr<uint8_t>(0xBD6D0), 16);
			ScorePopupRise_midhook = safetyhook::create_mid(Module::exe_ptr(0xBD6D0), ScorePopupRise_dest);
			Memory::VP::Nop(Module::exe_ptr<uint8_t>(0xBD6E5), 16);
			ScorePopupFade_midhook = safetyhook::create_mid(Module::exe_ptr(0xBD6E5), ScorePopupFade_dest);
			ScorePopupTimer_midhook = safetyhook::create_mid(Module::exe_ptr(0xBD8D5), ScorePopupTimer_dest);
			SpraniAddList_hook = safetyhook::create_inline(Module::exe_ptr(0x28320), SpraniAddList_dest);
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
		Memory::VP::Patch(Module::exe_ptr<uint8_t>(PatchAddr), Settings::FramerateLimit.get());

		return true;
	}

	static FullscreenRefreshRate instance;
};
FullscreenRefreshRate FullscreenRefreshRate::instance;
