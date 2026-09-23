#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace contour::controls
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}
} // namespace

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double HeightScaleFromParam( float value )
{
	return 2.0 * unit( value );
}

double SmoothSigmaFromParam( float value )
{
	const double p = unit( value );
	return 16.0 * p * p;
}

double GuardSlope( double heightScale, double sigma )
{
	return kGuardSlope * heightScale / std::max( 1.0, sigma );
}

double IntervalFromParam( float value )
{
	const double p = unit( value );
	return std::max( kMinInterval, 0.25 * p * p );
}

double LineWidthFromParam( float value )
{
	return 4.0 * unit( value );
}

double IndexWidthFromParam( float value )
{
	return 6.0 * unit( value );
}

double ShoreWidthFromParam( float value )
{
	return 6.0 * unit( value );
}

double AzimuthDegreesFromParam( float value )
{
	return 360.0 * unit( value );
}

double AltitudeDegreesFromParam( float value )
{
	return 90.0 * unit( value );
}

double ZFactorFromParam( float value )
{
	return std::pow( 10.0, 2.0 * unit( value ) - 1.0 );
}

double SeaLevelFromParam( float value, double heightScale )
{
	return unit( value ) * heightScale;
}

} // namespace contour::controls
