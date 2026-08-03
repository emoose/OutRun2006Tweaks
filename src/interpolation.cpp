#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "overlay/overlay.hpp"
#include "interpolation.hpp"

#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>

//
// Frame interpolation: draws cars, camera and effects at positions between two
// game ticks, so the picture stays smooth above the fixed 60Hz update rate.
//
// The game already contains the parts needed for this, unused. Every car *_Ctrl
// function copies its transform into a "previous" slot each tick, and the
// display matrix builders lerp between that and the current one using an alpha
// from sub_4493E0, which reads g_InterpAlpha.
// 
// Nothing in the game ever writes g_InterpAlpha however, so the alpha is always 
// 1.0 and the lerp always lands on the current transform. 
// Writing a fractional alpha and running the display builders again each rendered 
// frame lets us make use of the existing interpolation.
//
// Though that alone is not enough, as the engine also computes derived transforms
// during the tick and stores them for the draw to reuse: character matrices,
// camera prev-state, Oso objects, HAM hearts and lines, stage scale.
// 
// Most of those are built from car matrices, so they are wrong as soon as a car is
// drawn somewhere other than its tick position, and each needs its own previous/current pair.
// Some of them are read back by tick code as well as by drawing code, so they are put back 
// before the next tick runs.
//
namespace Interp
{

static float LerpF(float a, float b, float t) { return a + (b - a) * t; }

// cam_ang_128 is euler radians; take the shortest arc so a wrap through
// +/-pi doesn't spin the view the long way round for one frame.
static float LerpAngle(float a, float b, float t)
{
	constexpr float kPi = 3.14159265358979f;
	float d = b - a;
	while (d > kPi) d -= kPi * 2.0f;
	while (d < -kPi) d += kPi * 2.0f;
	return a + d * t;
}

static void LerpVec(D3DVECTOR& out, const D3DVECTOR& a, const D3DVECTOR& b, float t)
{
	out.x = LerpF(a.x, b.x, t);
	out.y = LerpF(a.y, b.y, t);
	out.z = LerpF(a.z, b.z, t);
}

// Cars observed during the most recent game tick. The game re-registers
// every live car each tick, so this list is rebuilt from scratch then.
// On 0-tick frames we deliberately keep the previous contents and replay
// against them - those pointers stay valid because car alloc/free only
// happens inside ModeControl/EventControl, which only run when a tick runs.
static std::vector<EVWORK_CAR*> InterpCars;
static bool InterpCollecting = false;

// In-car characters whose base matrix was baked from the car this tick.
struct CharEntry { EVWORK_CAR* car; void* rob; int index; };
static std::vector<CharEntry> InterpChars;

// --- Oso objects (beach balls, cones, UFOs, coins, rings...) ---
//
// OsoDynamics_Disp already interpolates: it lerps position between
// d3dmatrix_0 (current) and d3dmatrixE0 (prev), and rotation via
// SolveMatrixYXZ + sub_40F480, all off sub_4493E0's alpha. But it finishes
// with
//
//     qmemcpy(&a1->d3dmatrixE0, a1, sizeof(a1->d3dmatrixE0));   // prev = cur
//
// INSIDE the Disp - so the prev-state advances once per rendered frame
// instead of once per tick. The frame after a tick sees prev == cur and
// draws uninterpolated, so the object alternates and reads as 60Hz. Same
// self-consuming prev-state shape as the camera's sub_482F20.
//
// Fix: keep our own per-object prev, captured at OsoDynamics_Ctrl entry
// (where d3dmatrix_0 still holds the previous tick's final state), and
// write it into d3dmatrixE0 before each Disp. The function's own writeback
// then doesn't matter, because we overwrite it again next frame.
static constexpr int OsoDynMaxTracked = 64;

struct OsoDynEntry { EvWorkOsoDynamics* obj; D3DMATRIX prev; };
static OsoDynEntry OsoDynPrev[OsoDynMaxTracked]{};
static int OsoDynPrevCount = 0;

static OsoDynEntry* OsoDynFind(EvWorkOsoDynamics* obj)
{
	for (int i = 0; i < OsoDynPrevCount; i++)
	{
		if (OsoDynPrev[i].obj == obj)
			return &OsoDynPrev[i];
	}
	return nullptr;
}

static SafetyHookMid OsoDynCtrl_hook = {};
static void OsoDynCtrl_dest(SafetyHookContext& ctx)
{
	if (!Settings::FramerateInterpolation)
		return;

	auto* obj = reinterpret_cast<EvWorkOsoDynamics**>(ctx.esp)[1]; // [esp]=retaddr
	if (!obj)
		return;

	OsoDynEntry* e = OsoDynFind(obj);
	if (!e)
	{
		if (OsoDynPrevCount >= OsoDynMaxTracked)
			return;
		e = &OsoDynPrev[OsoDynPrevCount++];
		e->obj = obj;
	}

	// d3dmatrix_0 still holds last tick's final state at this point.
	e->prev = obj->d3dmatrix_0;
}

static SafetyHookMid OsoDynDisp_hook = {};
static void OsoDynDisp_dest(SafetyHookContext& ctx)
{
	if (!Settings::FramerateInterpolation)
		return;

	auto* obj = reinterpret_cast<EvWorkOsoDynamics**>(ctx.esp)[1];
	if (!obj)
		return;

	if (OsoDynEntry* e = OsoDynFind(obj))
		obj->d3dmatrixE0 = e->prev;
}

// OsoCommonFunc_Disp has no interpolation at all - it draws straight from
// the object's matrix at offset 0 via mxPushLoadMatrix, and there is no
// prev-state anywhere to lerp from. So we keep our own prev/cur per object
// and briefly substitute an interpolated matrix across the push:
//
//   4a8aae  push esi                 <- entry hook: write interpolated
//   4a8aaf  call mxPushLoadMatrix        (copies it onto the matrix stack)
//   4a8ab4  add  esp, 4              <- post hook: restore the real one
//
// Both sites are past the two early-return branches (which jump to
// 0x4A8B49), and esi holds the object at both. Because the struct is only
// modified across that one call, no tick code can ever observe an
// interpolated value - unlike field_D28, this cannot leak into gameplay.
static uint32_t InterpTickCounter = 0;

static constexpr int OsoCommonMaxTracked = 128;
struct OsoCommonEntry
{
	OsoCommonWork* obj;
	uint32_t tick;      // tick the prev<-cur shift last ran for
	uint32_t lastSeen;  // tick we last observed this object being drawn
	float prev[16];
	float cur[16];
};
static OsoCommonEntry OsoCommonPrev[OsoCommonMaxTracked]{};
static int OsoCommonCount = 0;

static float OsoCommonSaved[16]{};
static OsoCommonWork* OsoCommonPending = nullptr;

static SafetyHookMid OsoCommonDisp_hook = {};
static void OsoCommonDisp_dest(SafetyHookContext& ctx)
{
	OsoCommonPending = nullptr;

	if (!Settings::FramerateInterpolation)
		return;

	auto* obj = reinterpret_cast<OsoCommonWork*>(ctx.esi);
	if (!obj)
		return;

	OsoCommonEntry* e = nullptr;
	for (int i = 0; i < OsoCommonCount; i++)
	{
		if (OsoCommonPrev[i].obj == obj)
		{
			e = &OsoCommonPrev[i];
			break;
		}
	}

	if (!e)
	{
		if (OsoCommonCount < OsoCommonMaxTracked)
		{
			e = &OsoCommonPrev[OsoCommonCount++];
		}
		else
		{
			// Entries are never explicitly retired, objects just stop being drawn
			// so recycle whichever slot has gone longest unseen rather than giving 
			// up on interpolating new objects entirely.
			e = &OsoCommonPrev[0];
			for (int i = 1; i < OsoCommonCount; i++)
				if (uint32_t(InterpTickCounter - OsoCommonPrev[i].lastSeen) >
					uint32_t(InterpTickCounter - e->lastSeen))
					e = &OsoCommonPrev[i];
		}

		e->obj = obj;
		e->lastSeen = InterpTickCounter;
		e->tick = InterpTickCounter;
		memcpy(e->cur, &obj->matrix_0, sizeof(e->cur));
		memcpy(e->prev, &obj->matrix_0, sizeof(e->prev));
	}

	// Our hook is past this Disp's early-return, so a hidden object stops
	// updating here entirely. If we missed more than one tick, whatever is
	// in prev/cur is stale - lerping from it would fling the object in from
	// wherever it was last drawn. Restart cleanly instead.
	const bool stale = uint32_t(InterpTickCounter - e->lastSeen) > 1;
	e->lastSeen = InterpTickCounter;

	if (stale)
	{
		memcpy(e->cur, &obj->matrix_0, sizeof(e->cur));
		memcpy(e->prev, &obj->matrix_0, sizeof(e->prev));
		e->tick = InterpTickCounter;
	}
	// Shift prev<-cur once per tick, not once per rendered frame. Doing it
	// per frame is exactly what breaks OsoDynamics_Disp.
	else if (e->tick != InterpTickCounter)
	{
		memcpy(e->prev, e->cur, sizeof(e->prev));
		memcpy(e->cur, &obj->matrix_0, sizeof(e->cur));
		e->tick = InterpTickCounter;
	}

	const float a = *Game::g_InterpAlpha;

	memcpy(OsoCommonSaved, &obj->matrix_0, sizeof(OsoCommonSaved));
	OsoCommonPending = obj;

	// Component-wise blend. Translation is exact; for the 3x3 this is an
	// approximation, but per-tick rotation deltas are small enough that the
	// orthonormality error is far below anything visible, and unlike
	// rebuilding from euler angles it preserves any scale baked into the
	// matrix (which this Disp does apply for some model ids).
	float* m = reinterpret_cast<float*>(&obj->matrix_0);
	for (int i = 0; i < 16; i++)
		m[i] = e->prev[i] + (e->cur[i] - e->prev[i]) * a;
}

static SafetyHookMid OsoCommonPost_hook = {};
static void OsoCommonPost_dest(SafetyHookContext&)
{
	if (!OsoCommonPending)
		return;

	memcpy(&OsoCommonPending->matrix_0, OsoCommonSaved, sizeof(OsoCommonSaved));
	OsoCommonPending = nullptr;
}

static SafetyHookMid CalcCharMatrix_hook = {};
static void CalcCharMatrix_dest(SafetyHookContext& ctx)
{
	if (!InterpCollecting)
		return;

	auto* car = reinterpret_cast<EVWORK_CAR*>(ctx.esi);
	auto* stack = reinterpret_cast<uint32_t*>(ctx.esp); // [0]=retaddr
	void* rob = reinterpret_cast<void*>(stack[1]);
	int index = static_cast<int>(stack[2]);

	if (!car || !rob)
		return;

	for (auto& e : InterpChars)
	{
		if (e.rob == rob)
		{
			e.car = car;
			e.index = index;
			return;
		}
	}

	InterpChars.push_back({ car, rob, index });
}

// --- camera prev-state ---
//
// sub_482F20 (reached via CalcCameraMatrix) interpolates the camera the
// same way CalcDispMatrix interpolates cars, but it keeps its prev-state
// inside the camera struct at +0x384..+0x3B4 and rewrites it to the
// current values at the end of every call:
//
//   if (cam_pos - prev > 10.0) prev = cur;      // teleport guard only
//   ang = lerp(prev_ang_384,  cam_ang_128, alpha)
//   pos = lerp(prev_pos_390,  cam_pos_F8,  alpha)
//   ...build view matrix into camera+0x140...
//   prev_ang_384 = cam_ang_128;  prev_pos_390 = cam_pos_F8;   // <-- writeback
//
// So the tick's own call already consumed the prev-state and replaced it
// with the current state. Calling CalcCameraMatrix a second time for our
// interpolation pass then lerps cur->cur, which is the identity no matter
// what alpha is - the camera stays pinned to the tick position while the
// cars interpolate. That relative mismatch reads as a doubled car:
// the car oscillates on screen by half a tick of movement every frame even
// though its world-space motion is perfectly smooth.
//
// Fix: snapshot the cache at the top of each tick (so it holds the
// state from BEFORE the final tick ran), then restore it before our
// replay so sub_482F20 sees a genuine prev->cur pair.
// Rather than rely on sub_482F20's internal prev-state (which it rewrites on
// the way out of every call, so a second call can only ever see prev==cur),
// keep our own copy of the camera's inputs and drive them directly.
// Camera interpolation drives cam_pos_F8 / look_pos_104 / cam_ang_128
// ourselves and forces sub_482F20's own lerp to the identity, rather than
// restoring its internal prev-state and letting it interpolate.

static D3DVECTOR CameraPrevPos{};
static D3DVECTOR CameraPrevLook{};
static D3DVECTOR CameraPrevAng{};
static bool CameraPrevValid = false;


// --- stage load-in scale ---
//
// DispStage does mxScale(1.0, stage_disp_scale, 1.0) to make new stages 
// rise out of the ground.
//
// CalcStageDispScale sets it from the player car's trip meter, so it 
// advances once per tick and the rise is steppy above 60fps. It is 
// linear in that trip meter within the 0..1 range, so lerping the scalar 
// itself is equivalent to interpolating its input.
//
// Must be unwound before the next tick: CalcStageDispScale's return value
// drives the stage streaming state machine in LoadNextStage, which also
// reads stage_disp_scale directly.
static float StageScalePrev = 0.0f;
static float StageScaleReal = 0.0f;
static bool StageScalePrevValid = false;
static bool StageScaleOverridden = false;

static void RestoreStageScale()
{
	if (!StageScaleOverridden)
		return;

	*Game::stage_disp_scale = StageScaleReal;
	StageScaleOverridden = false;
}

// --- HAM "cut the lines" ---
//
// ConnectBetweenCars bakes each linked pair's endpoints, rotation and line
// length from the two cars' position_14 once per tick; DispConnectBetweenCars
// draws the line and the 3D heart from those baked values, so they stall at
// tick rate.
//
// Re-baking would mean replicating ConnectBetweenCars' 2KB of connection
// logic, so interpolate its output instead - every field is a continuous
// function of the car positions. Unwound before the next tick because
// HeartCtrl_cut_line reads the table back to detect a cut.
static ConnectionEntry ConnPrev[ConnectionEntryCount]{};
static ConnectionEntry ConnReal[ConnectionEntryCount]{};
static bool ConnPrevValid = false;
static bool ConnOverridden = false;

// The heart above each line spins via a single global, advanced once per
// tick by ConnectBetweenCars. Interpolating it keeps the spin matching the
// now-smooth travel. Must be unwound before the next tick: the game's own
// update is a read-modify-write, so leaving an interpolated value there
// would make the spin rate drift.
static float HartRotPrev = 0.0f;
static float HartRotReal = 0.0f;
static bool HartRotPrevValid = false;
static bool HartRotOverridden = false;

static void RestoreConnections()
{
	if (HartRotOverridden)
	{
		*Game::hart_rot_f = HartRotReal;
		HartRotOverridden = false;
	}

	if (!ConnOverridden)
		return;

	for (int i = 0; i < ConnectionEntryCount; i++)
		Game::connection_tbl[i] = ConnReal[i];

	ConnOverridden = false;
}

static void CaptureConnPrev()
{
	HartRotPrev = *Game::hart_rot_f;
	HartRotPrevValid = true;

	for (int i = 0; i < ConnectionEntryCount; i++)
		ConnPrev[i] = Game::connection_tbl[i];

	ConnPrevValid = true;
}

static void InterpolateConnections(float alpha)
{
	if (!ConnPrevValid)
		return;

	for (int i = 0; i < ConnectionEntryCount; i++)
	{
		auto& live = Game::connection_tbl[i];
		const auto& prev = ConnPrev[i];

		if (live.eventIdA == ConnectionUnusedId || live.eventIdB == ConnectionUnusedId)
			continue;

		// A pair that only just linked up has no meaningful previous state.
		if (prev.eventIdA != live.eventIdA || prev.eventIdB != live.eventIdB)
			continue;

		if (!ConnOverridden)
			ConnReal[i] = live;

		const auto& real = ConnReal[i];

		LerpVec(live.posA, prev.posA, real.posA, alpha);
		LerpVec(live.posB, prev.posB, real.posB, alpha);
		live.rotX = LerpAngle(prev.rotX, real.rotX, alpha);
		live.rotY = LerpAngle(prev.rotY, real.rotY, alpha);
		live.lineScaleZ = LerpF(prev.lineScaleZ, real.lineScaleZ, alpha);
		live.heartScale = LerpF(prev.heartScale, real.heartScale, alpha);
	}

	ConnOverridden = true;
}

static void InterpolateHartRot(float alpha)
{
	if (!HartRotPrevValid)
		return;

	if (!HartRotOverridden)
		HartRotReal = *Game::hart_rot_f;

	// Shortest-arc: the spin wraps, so a plain lerp would unwind it
	// backwards across the seam.
	*Game::hart_rot_f = LerpAngle(HartRotPrev, HartRotReal, alpha);
	HartRotOverridden = true;
}

// --- HAM attached hearts ---
//
// AttachHeart bakes each heart's world position from its owner car's
// matrix_B0 during the tick; HeartDisp_car_heart then draws straight from
// those baked values. Once we interpolate matrix_B0 the bakes are stale, so
// the hearts sit at tick positions while the cars move smoothly.
//
// Re-bake them from the interpolated matrices, then put the tick values back
// before the next tick - HeartCtrl_car_heart measures these against the
// player position to decide when a heart is collected, so leaving
// interpolated data in place would shift collection timing.
static D3DVECTOR HeartWorldSaved[AttachHeartEntryCount][2]{};
static bool HeartOverridden = false;

static void RestoreHeartWorld()
{
	if (!HeartOverridden)
		return;

	for (int i = 0; i < AttachHeartEntryCount; i++)
	{
		auto& entry = Game::attach_heart_table[i];
		entry.world[0] = HeartWorldSaved[i][0];
		entry.world[1] = HeartWorldSaved[i][1];
	}

	HeartOverridden = false;
}

// Mirrors the bake loop inside AttachHeart, but only the transform - none of
// its allocation or entry-retirement bookkeeping, which must stay per-tick.
static void RebakeHeartWorld()
{
	for (int i = 0; i < AttachHeartEntryCount; i++)
	{
		auto& entry = Game::attach_heart_table[i];

		if (entry.eventId == AttachHeartUnusedId)
			continue;
		if ((Game::g_EventIsOpenFlag[entry.eventId] & 3) != 2)
			continue;

		auto* car = Game::event(entry.eventId)->data<EVWORK_CAR>();
		if (!car)
			continue;

		if (!HeartOverridden)
		{
			HeartWorldSaved[i][0] = entry.world[0];
			HeartWorldSaved[i][1] = entry.world[1];
		}

		Game::mxPushLoadMatrix(&car->matrix_B0);
		for (int k = 0; k < 2; k++)
			if (entry.active[k])
				Game::mxCalcPoint(&entry.world[k], &entry.local[k]);
		Game::mxPopMatrix();
	}

	HeartOverridden = true;
}

// The true (tick) values, saved while the interpolated ones are live.
static D3DVECTOR CameraRealPos{};
static D3DVECTOR CameraRealLook{};
static D3DVECTOR CameraRealAng{};
static bool CameraOverridden = false;

// Put the real values back. Must happen before the next tick runs:
// CalcEyeLookPos reads cam_pos_F8 back as an input (it clamps against its
// own previous value), so leaving interpolated data in place would feed
// back into the camera's smoothing and drift.
static void RestoreCameraReal(EvWorkCamera* cam)
{
	if (!CameraOverridden)
		return;

	cam->cam_pos_F8 = CameraRealPos;
	cam->look_pos_104 = CameraRealLook;
	cam->cam_ang_128 = CameraRealAng;
	CameraOverridden = false;
}

static void CaptureCameraPrev(EvWorkCamera* cam)
{
	CameraPrevPos = cam->cam_pos_F8;
	CameraPrevLook = cam->look_pos_104;
	CameraPrevAng = cam->cam_ang_128;
	CameraPrevValid = true;
}

static SafetyHookInline CalcDispMatrix_hook = {};
static void __cdecl CalcDispMatrix_dest(EVWORK_CAR* car)
{
	// CalcDispMatrix is also called mid-tick by push_out_by_time (twice)
	// and PlWrecker, so the same car can legitimately arrive more than
	// once per tick - dedupe.
	if (InterpCollecting && car)
	{
		if (std::find(InterpCars.begin(), InterpCars.end(), car) == InterpCars.end())
			InterpCars.push_back(car);
	}

	CalcDispMatrix_hook.ccall(car);
}

// Debug logging.
static constexpr int InterpLogMaxLines = 180;
static int InterpLogCount = 0;

static bool InterpLogActive()
{
	return Settings::FramerateInterpolationDebugLog && InterpLogCount < InterpLogMaxLines;
}

void AfterTicks(double qpcFreqMs)
{
	// The camera path dereferences the player car, and neither is valid
	// outside of gameplay. Drop the cached camera prev-state too, so we
	// never restore a stale one across a mode change - we patched out the
	// game's own snap-guard, so nothing else would catch it.
	if (!Game::is_in_game())
	{
		// Restore before dropping the cached prev-state, otherwise an override
		// stays applied with nothing left that would undo it.
		Reset();

		ConnPrevValid = false;
		HartRotPrevValid = false;
		StageScalePrevValid = false;
		CameraPrevValid = false;
		OsoDynPrevCount = 0; // object pointers do not survive a mode change
		OsoCommonCount = 0;
		OsoCommonPending = nullptr;
		return;
	}

	const bool logThis = InterpLogActive();

	// Sub-tick remainder left by CalcNumUpdatesToRun, in QPC ticks.
	int64_t remainder = *Game::frameskip_remainder;
	double qpcFreq = qpcFreqMs * 1000.0;

	if (qpcFreq <= 0.0)
	{
		*Game::g_InterpAlpha = 1.0f;
		return;
	}

	float alpha = float((double(remainder) * 60.0) / qpcFreq);
	alpha = std::clamp(alpha, 0.0f, 1.0f);

	// Debug override for bisecting interpolation artifacts. >= 0 forces a
	// fixed alpha; 1.0 makes the replay value-identical to vanilla, so any
	// artifact that survives it is caused by the extra CalcDispMatrix call
	// rather than by the interpolated value.
	if (Settings::FramerateInterpolationDebugAlpha >= 0.0f)
		alpha = std::clamp(Settings::FramerateInterpolationDebugAlpha, 0.0f, 1.0f);

	// From here until the next tick, everything renders at this alpha.
	*Game::g_InterpAlpha = alpha;

	// The alpha actually in force: sub_4493E0 returns a hard 1.0 during
	// autoscenes, and everything we interpolate must agree with whatever
	// CalcDispMatrix used, or one thing slides while another is frozen.
	const float effectiveAlpha = Game::GetInterpAlpha();

	for (EVWORK_CAR* car : InterpCars)
	{
		if (!car)
			continue;

		// CalcDispMatrix writes matrix_B0 (display, which we want moved)
		// but also field_D28, a derived world point that gameplay reads:
		// GetForceFromCar (physics), IsHitCar (collision), CheckGoal,
		// OsoRing_Ctrl. Replaying per rendered frame would leave the
		// interpolated - i.e. up to a tick stale - position in there for the
		// next tick's collision and force maths to consume, which can shove
		// a car around. Keep the tick's value.
		const D3DVECTOR realD28 = car->field_D28;

		Game::CalcDispMatrix(car);

		car->field_D28 = realD28;
	}

	// Attached hearts are baked from matrix_B0 too.
	RebakeHeartWorld();

	// "Cut the lines" endpoints are baked from raw car positions.
	InterpolateConnections(effectiveAlpha);
	InterpolateHartRot(effectiveAlpha);

	// Stage load-in scale.
	if (StageScalePrevValid && !StageScaleOverridden)
	{
		StageScaleReal = *Game::stage_disp_scale;
		*Game::stage_disp_scale = LerpF(StageScalePrev, StageScaleReal, effectiveAlpha);
		StageScaleOverridden = true;
	}

	// Characters ride the car's matrix, but their own matrix was baked from
	// it during the tick - rebake it now that matrix_B0 has moved.
	for (const CharEntry& e : InterpChars)
	{
		if (e.car && e.rob)
			Game::CalcCharMatrix(e.car, e.rob, e.index);
	}

	if (logThis)
	{
		InterpLogCount++;

		EVWORK_CAR* pc = Game::pl_car();
		spdlog::info("interp #{} alpha={:.4f} rem={} cars={} prevValid={} cam={} plcar={}",
			InterpLogCount, alpha, remainder, InterpCars.size(),
			CameraPrevValid ? 1 : 0, fmt::ptr(Game::camera()), fmt::ptr(pc));

		if (pc)
		{
			spdlog::info("  car pos=({:.3f},{:.3f},{:.3f}) prev=({:.3f},{:.3f},{:.3f}) drawnB0=({:.3f},{:.3f},{:.3f})",
				pc->position_14.x, pc->position_14.y, pc->position_14.z,
				pc->field_16C.x, pc->field_16C.y, pc->field_16C.z,
				pc->matrix_B0._41, pc->matrix_B0._42, pc->matrix_B0._43);
		}

		if (!CameraPrevValid)
			spdlog::info("  cam SKIPPED: CameraPrevValid=0 (snapshot never ran)");
		else if (!Game::camera())
			spdlog::info("  cam SKIPPED: Game::camera() null");
		else if (!Game::pl_car())
			spdlog::info("  cam SKIPPED: pl_car null");
	}

	// The camera must be interpolated in lockstep with the cars. Cars move
	// smoothly in world space, but the camera is the frame of reference -
	// if it only steps at tick rate, the car appears to jump back and forth
	// on screen every frame even though its world motion is correct.
	//
	// Restore the pre-tick cache first so sub_482F20 sees prev != cur.
	if (CameraPrevValid)
	{
		EvWorkCamera* cam = Game::camera();
		if (cam && Game::pl_car())
		{
			// Stash the real values
			if (!CameraOverridden)
			{
				CameraRealPos = cam->cam_pos_F8;
				CameraRealLook = cam->look_pos_104;
				CameraRealAng = cam->cam_ang_128;
				CameraOverridden = true;
			}

			// Local copy so the teleport guard below can force it to 1.0
			// for this frame without affecting anything else.
			float camAlpha = effectiveAlpha;

			// Teleport guard. sub_482F20 has its own (>10.0 since the last
			// call) but we force its lerp to the identity below, so it never
			// gets a chance to fire. Race starts and camera-mode changes
			// (Camera_Ctrl_SingleStart / VsStart) cut the camera hard; at
			// ~0.35 units per tick while driving, 10.0 is ~28 ticks of
			// travel, so this only trips on a genuine cut.
			const float dz = CameraRealPos.z - CameraPrevPos.z;
			const float dy = CameraRealPos.y - CameraPrevPos.y;
			const float dx = CameraRealPos.x - CameraPrevPos.x;
			if ((dx * dx + dy * dy + dz * dz) > (10.0f * 10.0f))
				camAlpha = 1.0f;

			LerpVec(cam->cam_pos_F8, CameraPrevPos, CameraRealPos, camAlpha);
			LerpVec(cam->look_pos_104, CameraPrevLook, CameraRealLook, camAlpha);
			cam->cam_ang_128.x = LerpAngle(CameraPrevAng.x, CameraRealAng.x, camAlpha);
			cam->cam_ang_128.y = LerpAngle(CameraPrevAng.y, CameraRealAng.y, camAlpha);
			cam->cam_ang_128.z = LerpAngle(CameraPrevAng.z, CameraRealAng.z, camAlpha);

			// Force sub_482F20's own lerp to the identity so it simply
			// builds the view matrix from the values we just wrote.
			*Game::g_InterpAlpha = 1.0f;
			Game::CalcCameraMatrix(cam);
			*Game::g_InterpAlpha = alpha;

			// Deliberately leave the interpolated values live through the
			// render phase, so anything else that reads the camera position
			// while drawing (culling, billboards, flares) agrees with the
			// view matrix. RestoreCameraReal puts them back at the next tick.

			if (logThis)
			{
				spdlog::info("  cam={} prev=({:.3f},{:.3f},{:.3f}) cur=({:.3f},{:.3f},{:.3f}) lerp=({:.3f},{:.3f},{:.3f}) d_prev_cur={:.4f}",
					fmt::ptr(cam),
					CameraPrevPos.x, CameraPrevPos.y, CameraPrevPos.z,
					CameraRealPos.x, CameraRealPos.y, CameraRealPos.z,
					cam->cam_pos_F8.x, cam->cam_pos_F8.y, cam->cam_pos_F8.z,
					sqrtf((CameraRealPos.x - CameraPrevPos.x) * (CameraRealPos.x - CameraPrevPos.x)
						+ (CameraRealPos.y - CameraPrevPos.y) * (CameraRealPos.y - CameraPrevPos.y)
						+ (CameraRealPos.z - CameraPrevPos.z) * (CameraRealPos.z - CameraPrevPos.z)));

				spdlog::info("  view140 t=({:.3f},{:.3f},{:.3f})  prevPrevCache=({:.3f},{:.3f},{:.3f})",
					cam->d3dmatrix140._41, cam->d3dmatrix140._42, cam->d3dmatrix140._43,
					*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(cam) + 0x390),
					*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(cam) + 0x394),
					*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(cam) + 0x398));
			}
		}
	}

	// g_InterpAlpha deliberately left at the fractional value here.
	//
	// Six functions read it via sub_4493E0, split by phase:
	//
	//   CalcDispMatrix    (tick)   per-tick prev-state lerp - cars
	//   sub_482F20        (tick)   per-tick prev-state lerp - camera
	//   OsoWing_Ctrl      (tick)
	//   ExecPhysics       (tick)   <- physics, must see 1.0
	//   LensFlare_Disp    (render)
	//   OsoDynamics_Disp  (render)
	//
	// We reset g_InterpAlpha before the tick functions are run, so they
	// always run like vanilla game, but the two render functions run
	// after our interp hook, we can pass the real g_InterpAlpha value
	// for them to use for their own interpolation code.
}

// The attached hearts ("catch the hearts") animate from a uint16 tick counter:
//
//   movsx ecx, word ptr [esi+2Ch]   ; counter, +1 per tick via AttachHeart
//   imul  ecx, 444h                 ; integer multiply, before any float
//   call  Long2Float
//   fmul  flt_628254                ; x 2pi/65536   -> ~6 degrees per tick
//
// That angle drives the spin (mxRotateY), the bob height and the squash/stretch
// (all from Sinf of it), so all three step at tick rate.
//
// It cannot be interpolated the way everything else here is as the phase lives in
// an integer field, so there is no sub-tick value to write - counter + round(alpha)
// is just the same step taken early. Instead we intercept the computed angle.
//
// ebp holds it at the push below and survives the Sinf call (callee-saved), so
// it is still live for mxRotateY afterwards - one hook covers all three. And
// because AttachHeart always increments by exactly 1, the interpolated phase is
// simply (counter - 1 + alpha); no per-entry prev-state is needed.
//
static SafetyHookMid HeartPulse_hook = {};
static void HeartPulse_dest(SafetyHookContext& ctx)
{
	if (!Settings::FramerateInterpolation)
		return;

	const float alpha = Game::GetInterpAlpha();

	// esi points 0x2C into the entry, so the counter sits at esi+0x2C.
	const int16_t counter = *reinterpret_cast<int16_t*>(ctx.esi + 0x2C);

	constexpr float AngleStep = 0x444 * 9.58738019107841e-05f; // matches flt_628254
	const float angle = (float(counter) - (1.0f - alpha)) * AngleStep;

	ctx.ebp = *reinterpret_cast<const uint32_t*>(&angle);
}

//
// Entry points used by the replaced game loop.
//

// Puts back every value AfterTicks wrote into the game and returns the alpha to
// 1.0. Each step does nothing when that value is not currently overridden, so
// this is safe to call at any point.
void Reset()
{
	// sub_4493E0 hands this alpha to ExecPhysics as well as to the display
	// matrix builders, so a fractional value left behind changes how the game
	// runs rather than only how it looks.
	*Game::g_InterpAlpha = 1.0f;

	if (EvWorkCamera* cam = Game::camera())
		RestoreCameraReal(cam);

	RestoreHeartWorld();
	RestoreConnections();
	RestoreStageScale();
}

void BeforeTick()
{
	// Restoring happens whatever FramerateInterpolation is set to. It can change
	// between frames, and a value already written into the game still has to be
	// put back before tick code reads it.
	Reset();

	if (Settings::FramerateInterpolation)
	{
		// Keep the restored values as the prev side of the next frame's lerp.
		if (EvWorkCamera* cam = Game::camera())
			CaptureCameraPrev(cam);

		CaptureConnPrev();

		StageScalePrev = *Game::stage_disp_scale;
		StageScalePrevValid = true;
	}

	// Drives the prev<-cur shift for objects we track ourselves, so it happens
	// once per tick rather than once per rendered frame.
	InterpTickCounter++;

	// Rebuild the per-tick object lists. Cleared per iteration so the final
	// tick's set is what we replay against.
	InterpCars.clear();
	InterpChars.clear();
	InterpCollecting = true;
}

void AfterTick()
{
	InterpCollecting = false;
}

bool Apply()
{
	// Track which cars are live each tick so we can replay CalcDispMatrix over
	// them on non-tick frames.
	CalcDispMatrix_hook = safetyhook::create_inline(Module::exe_ptr(GameAddr::CalcDispMatrix), CalcDispMatrix_dest);
	if (!CalcDispMatrix_hook)
		return false;

	// In-car characters, whose baked base matrix must be rebuilt once the car
	// matrices move.
	CalcCharMatrix_hook = safetyhook::create_mid(Module::exe_ptr(GameAddr::CalcCharMatrix), CalcCharMatrix_dest);
	if (!CalcCharMatrix_hook)
		return false;

	// Oso objects: supply a per-tick prev-state to OsoDynamics_Disp, whose own
	// writeback advances once per rendered frame.
	OsoDynCtrl_hook = safetyhook::create_mid(Module::exe_ptr(GameAddr::OsoDynamics_Ctrl), OsoDynCtrl_dest);
	OsoDynDisp_hook = safetyhook::create_mid(Module::exe_ptr(GameAddr::OsoDynamics_Disp), OsoDynDisp_dest);
	if (!OsoDynCtrl_hook || !OsoDynDisp_hook)
		return false;

	// OsoCommonFunc_Disp has no interpolation of its own: substitute an
	// interpolated matrix across the mxPushLoadMatrix call, then put the real
	// one straight back.
	OsoCommonDisp_hook = safetyhook::create_mid(Module::exe_ptr(GameAddr::OsoCommon_PushMatrix), OsoCommonDisp_dest);
	OsoCommonPost_hook = safetyhook::create_mid(Module::exe_ptr(GameAddr::OsoCommon_AfterPush), OsoCommonPost_dest);
	if (!OsoCommonDisp_hook || !OsoCommonPost_hook)
		return false;

	// Attached-heart pulse: rewrite the computed animation angle in-register.
	HeartPulse_hook = safetyhook::create_mid(Module::exe_ptr(GameAddr::HeartDisp_PulseAngle), HeartPulse_dest);
	if (!HeartPulse_hook)
		return false;

	InterpCars.reserve(32);
	InterpChars.reserve(8);
	return true;
}

}
