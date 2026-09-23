#include "Audio.h"

#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace contour
{

float SeaDrive::Update( const float* bins, int binCount, int band, int mode, double now, bool jumped, int perturb )
{
	using namespace controls;

	const int b    = std::clamp( band, 0, kBandCount - 1 );
	const int from = kBandRange[ b ][ 0 ];
	const int to   = std::min( kBandRange[ b ][ 1 ], binCount - 1 );
	double sum     = 0.0;
	int counted    = 0;
	for( int i = from; bins != nullptr && i <= to; ++i, ++counted )
		sum += std::sqrt( std::max( 0.0, static_cast< double >( bins[ i ] ) ) );
	const double x = counted > 0 ? std::clamp( sum / counted, 0.0, 1.0 ) : 0.0;
	instant        = static_cast< float >( x );

	if( !primed || jumped )
	{
		//Prime: what is playing now is the baseline, not an onset. The
		//negative control starts from silence instead, as an unprimed
		//detector would.
		const bool fromSilence = ( perturb & perturb::kUnprimed ) != 0;
		level                  = fromSilence ? 0.0 : x;
		baseline               = fromSilence ? 0.0 : x;
		envelope               = 0.0;
		last                   = now;
		primed                 = true;
	}

	const double dt = std::max( 0.0, now - last );
	last            = now;

	//Level: fast up, slow down.
	if( x >= level )
		level = x;
	else
		level += ( x - level ) * ( 1.0 - std::exp( -dt / kLevelRelease ) );

	//Onset: the rise above the baseline as it stood before this frame.
	const double flux = std::max( 0.0, x - baseline );
	baseline += ( x - baseline ) * ( 1.0 - std::exp( -dt / kBaselineTime ) );
	envelope = std::max( std::min( 1.0, flux * kOnsetGain ), envelope * std::exp( -dt / kOnsetRelease ) );

	drive = static_cast< float >( mode == kAudioOnset ? envelope : std::clamp( level, 0.0, 1.0 ) );
	return drive;
}

} // namespace contour
