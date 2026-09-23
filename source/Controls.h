#pragma once

/**
	What a host parameter means, in the map's own units.

	Every ranged host parameter is 0..1 (SetParamInfo clamps a STANDARD
	default into 0..1 before a range can be attached), and an option
	parameter's range reads back 0..1 whatever its element count -- so an
	option is its element INDEX, rounded and clamped here, never a fraction
	of a range. Index Every is the one real integer (FF_TYPE_INTEGER is not
	clamped) and is passed through as one.

	The units:

	  elevation   the height the picture is read as: the source channel
	              (0..1) times Height Scale. Interval and Sea Level are in it.
	  pixels      Smooth, Line Width, Index Width and Shore Width. A survey
	              map's lines are a pen width on the page, and so are these.
	  degrees     Azimuth (compass, clockwise from north = up) and Altitude.

	Every mapping is linear or a plain power so a harness can choose an
	exactly representable value (Interval 0.5 is 1/16 of the elevation unit,
	Line Width 0.3 is 1.2 px), and every mapping is stated independently in
	tools/cntest, which reads the plugin only through the picture.
*/
namespace contour::controls
{

/// An option's stored value, as an index into its `count` elements.
int OptionIndex( float value, int count );

/// Height Scale: elevation per unit of the source channel, 0..2 (1 at 0.5).
double HeightScaleFromParam( float value );

/// Smooth: the generalising Gaussian's sigma in pixels, 16 p^2 (0..16).
double SmoothSigmaFromParam( float value );

/// The guard slope for this Height Scale and sigma: kGuardSlope times the
/// scale, over max( 1, sigma ).
double GuardSlope( double heightScale, double sigma );

/// Interval: elevation between contours, p^2 / 4, floored at kMinInterval.
double IntervalFromParam( float value );

/// Line Width and Index Width: full stroke width in pixels, 4 p and 6 p.
double LineWidthFromParam( float value );
double IndexWidthFromParam( float value );

/// Shore Width: the shoreline stroke's full width in pixels, 6 p.
double ShoreWidthFromParam( float value );

/// Azimuth in degrees clockwise from north (up), 360 p. Altitude in degrees
/// above the horizon, 90 p.
double AzimuthDegreesFromParam( float value );
double AltitudeDegreesFromParam( float value );

/// Z Factor: vertical exaggeration, 10^( 2p - 1 ), 0.1 .. 10 (1 at 0.5).
double ZFactorFromParam( float value );

/// Sea Level as elevation: p times Height Scale (a fraction of the range the
/// picture can reach).
double SeaLevelFromParam( float value, double heightScale );

/// Elevation the sea rises at full audio drive: half the height range.
constexpr double kAudioRiseRange = 0.5;

/// The smallest interval, so a zero slider cannot ask for a contour at every
/// float.
constexpr double kMinInterval = 1.0 / 512.0;

/// The largest sigma the blur's weight table holds: radius ceil( 3 sigma ).
constexpr int kMaxBlurRadius = 48;

/// **The flat-ground guard.** Below this slope, in elevation per pixel per
/// unit of Height Scale, the isoline's position is set by quantisation and
/// noise rather than by the picture, and its distance estimate
/// |h - level| / |grad h| is a ratio of two numbers that are both noise. The
/// unit is one 8-bit code value over eight pixels; smoothing averages the
/// quantisation down in proportion to its sigma, so the guard is divided by
/// max( 1, sigma ) -- generalising more lets flatter ground carry a contour.
/// Lines fade in between the guard and twice it and are fully drawn above.
/// See AGENTS.md, "The guard".
constexpr double kGuardSlope = 1.0 / ( 255.0 * 8.0 );

/// What Height Source stores.
enum HeightSource
{
	kSourceLuma = 0,
	kSourceRed,
	kSourceGreen,
	kSourceBlue,
	kSourceAlpha,
	kSourceCount
};

/// What Tint Mode stores.
enum TintMode
{
	kTintClassic = 0,
	kTintMono,
	kTintOff,
	kTintCount
};

/// What Audio Mode stores.
enum AudioMode
{
	kAudioLevel = 0,
	kAudioOnset,
	kAudioModeCount
};

/// What Audio Band stores, and the bins each band averages over the 64 the
/// host is asked for (regauss's three-way crossover split). Nobody has
/// measured Resolume's bins, so nothing here assumes they are linear.
enum AudioBand
{
	kBandFull = 0,
	kBandBass,
	kBandMid,
	kBandTreble,
	kBandCount
};
constexpr int kAudioBins                     = 64;
constexpr int kBandRange[ kBandCount ][ 2 ] = { { 0, 63 }, { 0, 7 }, { 8, 27 }, { 28, 63 } };

} // namespace contour::controls

/**
	Negative-control hooks. Each bit perturbs the PLUGIN's model -- never the
	harness's expectation -- so `cntest --negative` can prove each check fails
	against a wrong plugin. Always 0 outside the harness.
*/
namespace contour::perturb
{
constexpr int kNoSlopeDivide   = 1 << 0;///< line distance |h - L| / g_ref: the division by |grad h| dropped
constexpr int kNoGuard         = 1 << 1;///< no flat-ground fade; the denominator only kept off zero
constexpr int kLightFromSouth  = 1 << 2;///< the light's north component negated
constexpr int kIndexOffByOne   = 1 << 3;///< index contours on k = -1 mod N
constexpr int kLevelsHalfStep  = 1 << 4;///< levels at ( k + 1/2 ) interval
constexpr int kIntervalDetune  = 1 << 5;///< interval 3% wide
constexpr int kSeaLow          = 1 << 6;///< water filled to a quarter interval below the sea
constexpr int kShoreOffset     = 1 << 7;///< the shoreline stroke drawn half a pixel uphill
constexpr int kUnprimed        = 1 << 8;///< the onset detector starts from silence
constexpr int kResizeStale     = 1 << 9;///< a resize keeps the old height buffers
constexpr int kNoZFactor       = 1 << 10;///< the hillshade ignores Z Factor
constexpr int kClockFloat      = 1 << 11;///< the clock kept in float
constexpr int kDriveOnResize   = 1 << 12;///< a resize resets the detector without priming it
} // namespace contour::perturb
