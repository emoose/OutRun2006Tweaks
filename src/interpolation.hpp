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
}
