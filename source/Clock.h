#pragma once

/**
	The audio envelope's clock: seconds since the plugin's first frame, in
	double, from whatever the host hands SetTime.

	The map itself is a pure function of the frame and never reads the clock.
	Only the sea's audio drive has state across frames (a release, an onset
	baseline), and it needs an honest dt. Three problems, all inherited from
	the fleet:

	**What unit is the host's clock in?** The FFGL header never says. Resolume
	sends milliseconds; the offline harness and oxbow send seconds. The unit is
	voted on against a steady wall clock over the first few frames (readout's
	voting, by way of cadence and standards) and then stuck to. The harness
	DECLARES its unit, because it renders as fast as the GPU allows and there
	is nothing for the voting to measure.

	**Resolume's clock overflows a float.** It has been measured at ~499
	million ms, where a float resolves ~0.03 s. So the clock is kept as an
	origin and an offset in double, and nothing absolute reaches a shader.

	**A scrub, a loop or a stall is not a frame.** A delta that is backwards or
	longer than half a second steps the clock one nominal frame and reports
	`Jumped()`, and the audio detector re-primes on it rather than reading the
	jump as a very long, very quiet frame. Copied from standards unchanged.
*/
namespace contour
{
class Clock
{
public:
	/// Advance to this frame. `hostTime` is whatever the host last handed to
	/// SetTime, or negative if it never has.
	void Update( double hostTime );

	/// Declare the host's unit instead of letting Update infer it.
	void SetScaleForTest( double scale )
	{
		clockScale = scale;
	}

	/// The offline --clock check's negative control: keep the arithmetic in
	/// float, the way an absolute host clock would be if it reached a shader.
	void SetFloatForTest( bool on )
	{
		useFloat = on;
	}

	/// Seconds since the first frame. Monotonic.
	double Now() const
	{
		return now;
	}

	/// True on a frame whose delta was not believed.
	bool Jumped() const
	{
		return jumped;
	}

	/// True until the first Update.
	bool Fresh() const
	{
		return lastScaled < 0.0 && !started;
	}

	double ClockScale() const
	{
		return clockScale;
	}

	void Reset();

	/// Longer than this between two frames is a jump, not a frame.
	static constexpr double kMaxFrameSeconds = 0.5;
	/// What a jump advances the clock by.
	static constexpr double kNominalFrameSeconds = 1.0 / 60.0;

private:
	double clockScale   = 0.0;///< 0 undecided, 1 seconds, 0.001 milliseconds
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;

	bool started      = false;
	double lastScaled = -1.0;
	double anchor     = 0.0;///< the scaled host time the current run started at
	double offset     = 0.0;///< the clock's value at that moment
	double now        = 0.0;
	bool jumped       = false;
	bool useFloat     = false;
};

} // namespace contour
