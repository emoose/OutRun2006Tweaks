#pragma once

namespace Interp
{
	// Puts every interpolated value back to what the game last computed. Called
	// at the top of each tick anyway, so it is only needed directly when
	// FramerateInterpolation is turned off part way through a session: nothing
	// else would undo the values left applied for the frame that was rendering.
	void Reset();

	// Call at the top of each game tick, before any tick code runs. Undoes the
	// previous frame's interpolated values, records the pre-tick state to
	// interpolate from, and starts collecting the objects this tick touches.
	void BeforeTick();

	// Call at the bottom of each game tick. Stops that collection.
	void AfterTick();

	// Call once per rendered frame, after every tick has run and immediately
	// before the game's render path. Works out the alpha from how far the frame
	// sits between ticks, then applies it. qpcFreqMs is QPC ticks per
	// millisecond.
	void AfterTicks(double qpcFreqMs);

	// Installs the hooks. Returns false if any failed.
	bool Apply();

	// Runtime switches and counters for tracking down interpolation artifacts.
	// Not INI settings: they are meant to be flipped while an artifact is on
	// screen, and none of them should survive a restart.
	struct DebugState
	{
		// Each interpolated subsystem can be turned off on its own, so an
		// artifact can be narrowed to whichever one is producing it. Turning one
		// off still restores whatever it already applied, so it is safe to
		// toggle mid-frame.
		bool doCars = true;
		bool doCamera = true;
		bool doParticles = true;
		bool doConnections = true;
		bool doHearts = true;
		bool doStageScale = true;

		// What to shift particles back along. The car's display lag is correct
		// for particles emitted during the tick, which are the ones at the tyre
		// where an artifact shows. Off falls back to each particle's own
		// velocity, which suits only particles that already existed and lands
		// about four times short for fresh ones.
		bool particleUseDispLag = true;

		// Scales that shift. 1.0 is the correct amount; needing anything else
		// means the basis is wrong rather than the amount.
		float particleOffsetScale = 1.0f;

#ifdef _DEBUG
		// Refreshed every rendered frame, for the overlay readout.
		float alpha = 0.0f;             // from the sub-tick remainder
		float effectiveAlpha = 0.0f;    // what the interpolators actually used
		int carsReplayed = 0;
		int particleSourcesSeen = 0;
		int particlesMoved = 0;
		float particleLastShift = 0.0f;
		float carDispLag = 0.0f;        // how far behind its tick position the car is drawn
#endif
	};

	inline DebugState Debug;
}
