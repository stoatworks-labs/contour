#pragma once

/**
	The sea's audio drive: 64 FFT bins in, one number in 0..1 out.

	The only state in the plugin that lives across frames, and so the only
	thing a clock, a clip trigger or a resize can break. CPU only, no GL, so
	`cntest --detector` checks it with no context at all.

	  instantaneous   the chosen band's mean of sqrt( bin ). sqrt because bin
	                  magnitudes bunch against zero (regauss's reason); the
	                  mean because a peak hops between bins. Nobody has
	                  measured Resolume's bins, so nothing assumes they are
	                  linear in anything.
	  Level           fast up, slow down: the instantaneous value when it
	                  rises, a 0.25 s release when it falls. The sea breathes
	                  with the music.
	  Onset           the rise of the instantaneous value above a quick (0.1 s)
	                  baseline, times four, held by a 0.3 s release. The sea
	                  surges on a hit and drains after it.

	**Primed on the first frame and after every jump.** An unprimed baseline
	starts at silence, so the first frame of a clip with audio reads as the
	loudest onset there has ever been, and the sea floods on every clip
	trigger. Priming sets the baseline and the level to the instantaneous
	value and the envelope to zero. A dt of zero is a frame with no release,
	never a snap.
*/
namespace contour
{
class SeaDrive
{
public:
	/// Forget everything; the next Update primes.
	void Reset()
	{
		primed = false;
	}

	/// One frame. `bins` may be null (no audio routed), which reads as
	/// silence. `jumped` re-primes. Returns the drive, 0..1.
	float Update( const float* bins, int binCount, int band, int mode, double now, bool jumped, int perturb );

	float Drive() const
	{
		return drive;
	}

	/// The band's instantaneous value last frame, for the harness.
	float Instantaneous() const
	{
		return instant;
	}

	static constexpr double kLevelRelease  = 0.25;
	static constexpr double kBaselineTime  = 0.1;
	static constexpr double kOnsetRelease  = 0.3;
	static constexpr double kOnsetGain     = 4.0;

private:
	bool primed     = false;
	double last     = 0.0;
	double level    = 0.0;
	double baseline = 0.0;
	double envelope = 0.0;
	float instant   = 0.0f;
	float drive     = 0.0f;
};

} // namespace contour
