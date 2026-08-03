#pragma once

//
// Frame interpolation.
//
// The game ships a complete render-interpolation system that was left dormant:
// every car *_Ctrl copies its transform into a "previous" slot each tick, and
// the display-matrix builders lerp between previous and current using a single
// global alpha (sub_4493E0 -> g_InterpAlpha) that nothing ever writes. Setting
// a fractional alpha and replaying the display builders once per rendered frame
// re-enables it.
//
// Most of the work is the long tail: the engine repeatedly *bakes* derived
// transforms during the tick and reads the baked copy at draw time (character
// matrices, camera prev-state, Oso objects, HAM hearts and lines, stage scale).
// Each of those goes stale the moment the car matrices move, and each needs its
// own prev/cur handling. Several are also gameplay state rather than display
// only, so they are unwound before the next tick runs.
//
namespace Interp
{
	// Call at the top of each game tick, before any tick code runs: unwinds the
	// previous frame's interpolated overrides, snapshots prev-state, and starts
	// collecting the objects this tick touches.
	void BeforeTick();

	// Call at the bottom of each game tick.
	void AfterTick();

	// Call once per rendered frame, after all ticks and immediately before the
	// game's render path. qpcFreqMs is QPC ticks per millisecond.
	void AfterTicks(double qpcFreqMs);

	// Installs the hooks. Returns false if any failed.
	bool Apply();
}
