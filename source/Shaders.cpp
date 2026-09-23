#include "Shaders.h"

namespace contour::shaders
{

const char* const kVertex = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// height: the picture read as ground. One texel per host pixel, same rows.
//---------------------------------------------------------------------------
const char* const kHeight = R"(#version 410 core

uniform sampler2D InputTexture;
uniform int Source;     //0 luma, 1 red, 2 green, 3 blue, 4 alpha
uniform int Invert;
uniform float Scale;    //elevation per unit of the channel

out vec4 fragColor;

void main()
{
	vec4 c = texelFetch( InputTexture, ivec2( gl_FragCoord.xy ), 0 );
	float s;
	if( Source == 0 )
		s = dot( c.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );//Rec. 709 luma
	else if( Source == 1 )
		s = c.r;
	else if( Source == 2 )
		s = c.g;
	else if( Source == 3 )
		s = c.b;
	else
		s = c.a;
	if( Invert != 0 )
		s = 1.0 - s;
	fragColor = vec4( s * Scale, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// blur: one axis of the generalising Gaussian. Weights[ k ] is the weight
// at distance k, normalised on the CPU in double over -Radius..Radius; the
// edge is clamped (the ground carries on level past the frame). Summed as
// symmetric pairs, centre first.
//---------------------------------------------------------------------------
const char* const kBlur = R"(#version 410 core

uniform sampler2D Source;
uniform int Width;
uniform int Height;
uniform int DirX;
uniform int DirY;
uniform int Radius;
uniform float Weights[ 49 ];

out vec4 fragColor;

float at( ivec2 p )
{
	return texelFetch( Source, clamp( p, ivec2( 0 ), ivec2( Width - 1, Height - 1 ) ), 0 ).r;
}

void main()
{
	ivec2 p   = ivec2( gl_FragCoord.xy );
	ivec2 dir = ivec2( DirX, DirY );
	float sum = Weights[ 0 ] * at( p );
	for( int k = 1; k <= Radius; ++k )
		sum += Weights[ k ] * ( at( p - k * dir ) + at( p + k * dir ) );
	fragColor = vec4( sum, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// map: the survey drawing.
//
// Every line is drawn from its DISTANCE ON THE PAGE, |h - level| / |grad h|
// in pixels, not from |h - level|: that is what keeps a line one pen wide on
// a cliff and on a plain, and what lets the spacing, interval / |grad h|,
// carry the slope. Coverage is the exact overlap of the pixel's footprint
// (one pixel wide, across the line) with a stroke of the full width -- the
// box of the stroke convolved with the box of the pixel -- so a straight
// line's coverage summed across it is its width, wherever it falls.
//
// The guard: below GuardSlope the ratio is two noises divided, and a
// plateau that sits on a level would ink wholesale. Lines fade in between
// the guard and twice it, and the denominator never drops below it, so the
// width is exact wherever a line is fully drawn and bounded where it is not.
//---------------------------------------------------------------------------
const char* const kMap = R"(#version 410 core

uniform sampler2D Height;
uniform sampler2D InputTexture;
uniform int HWidth;          //the height buffer's size
uniform int HHeight;
uniform int VpX;
uniform int VpY;
uniform int VpW;
uniform int VpH;

uniform float Interval;
uniform int IndexEvery;
uniform float HalfLine;      //half the stroke widths, in pixels
uniform float HalfIndex;
uniform float HalfShore;
uniform int ContoursOn;
uniform vec3 LineColour;

uniform float GuardSlope;    //elevation per pixel
uniform float RefSlope;      //the negative control's fixed slope

uniform vec3 Light;          //unit vector toward the light: x east, y north, z up
uniform float ZScale;        //Z Factor times the frame height in pixels
uniform float Hillshade;

uniform int TintMode;        //0 classic, 1 mono, 2 off
uniform float TintStrength;
uniform float TopOfRange;    //the highest elevation the picture can reach

uniform float SeaLevel;      //with the audio's rise already in it
uniform vec3 WaterColour;
uniform vec3 PaperColour;
uniform float MixAmount;
uniform int Perturb;

out vec4 fragColor;

float heightAt( ivec2 p )
{
	return texelFetch( Height, clamp( p, ivec2( 0 ), ivec2( HWidth - 1, HHeight - 1 ) ), 0 ).r;
}

// Coverage of a stroke of half-width hw by a pixel whose centre is d pixels
// from the stroke's centre line: | [d - 1/2, d + 1/2] intersect [-hw, hw] |.
float stroke( float d, float hw )
{
	return max( 0.0, min( d + 0.5, hw ) - max( d - 0.5, -hw ) );
}

vec3 classic( float t )
{
	const vec3 c0 = vec3( 0.44, 0.62, 0.40 );
	const vec3 c1 = vec3( 0.67, 0.78, 0.50 );
	const vec3 c2 = vec3( 0.90, 0.85, 0.60 );
	const vec3 c3 = vec3( 0.78, 0.60, 0.42 );
	const vec3 c4 = vec3( 0.95, 0.93, 0.90 );
	float u = clamp( t, 0.0, 1.0 ) * 4.0;
	if( u < 1.0 )
		return mix( c0, c1, u );
	if( u < 2.0 )
		return mix( c1, c2, u - 1.0 );
	if( u < 3.0 )
		return mix( c2, c3, u - 2.0 );
	return mix( c3, c4, u - 3.0 );
}

void main()
{
	int X = int( gl_FragCoord.x ) - VpX;
	int Y = int( gl_FragCoord.y ) - VpY;
	ivec2 p = ivec2( ( ( 2 * X + 1 ) * HWidth ) / ( 2 * VpW ), ( ( 2 * Y + 1 ) * HHeight ) / ( 2 * VpH ) );

	//--- the ground, and its slope in elevation per pixel, north up.
	float h = heightAt( p );
	vec2 g  = 0.5 * vec2( heightAt( p + ivec2( 1, 0 ) ) - heightAt( p - ivec2( 1, 0 ) ),
	                      heightAt( p + ivec2( 0, 1 ) ) - heightAt( p - ivec2( 0, 1 ) ) );
	float slope = length( g );

	//--- the guard.
	bool guarded = ( Perturb & 2 ) == 0;
	float fade   = guarded ? smoothstep( GuardSlope, 2.0 * GuardSlope, slope ) : 1.0;
	float across = guarded ? max( slope, GuardSlope ) : max( slope, 1e-30 );
	if( ( Perturb & 1 ) != 0 )
		across = RefSlope;

	//--- the nearest level, and which contour it is.
	float interval = ( Perturb & 32 ) != 0 ? Interval * 1.03 : Interval;
	float offset   = ( Perturb & 16 ) != 0 ? 0.5 : 0.0;
	float k        = floor( h / interval - offset + 0.5 );
	float level    = ( k + offset ) * interval;
	float n        = float( IndexEvery );
	float kk       = ( Perturb & 8 ) != 0 ? k + 1.0 : k;
	bool isIndex   = kk - n * floor( kk / n ) == 0.0;
	float contour  = float( ContoursOn ) * fade * stroke( abs( h - level ) / across, isIndex ? HalfIndex : HalfLine );

	//--- the sea.
	float seaFill = ( Perturb & 64 ) != 0 ? SeaLevel - 0.25 * Interval : SeaLevel;
	bool sea      = h < seaFill;
	float shoreH  = ( Perturb & 128 ) != 0 ? h - SeaLevel - 0.5 * across : h - SeaLevel;
	float shore   = fade * stroke( abs( shoreH ) / across, HalfShore );

	//--- the band this pixel is in, for the tints: between two levels.
	float bandMid = ( floor( h / interval ) + 0.5 ) * interval;

	vec3 colour;
	if( sea )
	{
		float depth = clamp( ( SeaLevel - bandMid ) / max( SeaLevel, 1e-6 ), 0.0, 1.0 );
		colour      = TintMode == 2 ? WaterColour : WaterColour * mix( 1.0, 1.0 - 0.55 * depth, TintStrength );
	}
	else
	{
		float t = clamp( ( bandMid - SeaLevel ) / max( TopOfRange - SeaLevel, 1e-6 ), 0.0, 1.0 );
		colour  = PaperColour;
		if( TintMode == 0 )
			colour = mix( PaperColour, classic( t ), TintStrength );
		else if( TintMode == 1 )
			colour = mix( PaperColour, vec3( mix( 0.62, 0.97, t ) ), TintStrength );

		//Hillshade: Lambert on the surface normal, the surface lifted by
		//ZScale so a slope is in pixels over pixels.
		vec3 normal = normalize( vec3( -ZScale * g, 1.0 ) );
		float hs    = max( 0.0, dot( normal, Light ) );
		colour *= mix( 1.0, hs, Hillshade );

		colour = mix( colour, LineColour, contour );
	}
	colour = mix( colour, 0.4 * WaterColour, shore );

	vec4 map = vec4( colour, 1.0 );
	if( MixAmount >= 1.0 )
	{
		fragColor = map;
		return;
	}
	vec4 clip = texelFetch( InputTexture, p, 0 );
	fragColor = mix( clip, map, MixAmount );
}
)";

} // namespace contour::shaders
