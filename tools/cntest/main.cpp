/**
	cntest -- render Contour offline, and read the survey back out of it.

	How many contours a ramp crosses, how far apart they sit, how wide each
	one is drawn, how bright a plane of known slope is shaded, where the sea
	stops and the shoreline runs, and whether flat noisy ground stays blank
	are facts with one right answer each. Every check here drives the REAL
	plugin class through a headless GL context and measures the answer out of
	the picture it made:

		cntest --out /tmp/frame.png     a picture, on the moving terrain card
		cntest --list                   every parameter, its kind and default
		cntest --count                  contours crossed along a ramp: an exact
		                                integer, by the stated boundary rule
		cntest --spacing                adjacent isolines interval / g apart,
		                                whole-pixel and fractional, separately
		cntest --width                  one pen width on slopes spanning 8:1
		                                (the headline claim)
		cntest --hillshade              planes of known slope and aspect shade
		                                to the closed-form cartographic value
		cntest --sea                    water exactly below sea level; the
		                                shoreline centred on its isoline
		cntest --index                  every Nth line, and only those, heavy
		cntest --flat                   a noisy plateau ON a level draws nothing
		cntest --audio                  the music raises the sea, and the onset
		                                detector is primed
		cntest --resize                 a resize mid-run changes nothing it
		                                should not
		cntest --negative               every check above can FAIL
		cntest --offline                the checks that need no GL
		cntest --bench                  the render cost
		cntest --dump-shaders DIR       the exact GLSL the plugin compiles
		cntest --pipe                   raw frames in, raw frames out

	Every check takes --size; run each at the raster you care about and at
	320x180, which is CI's. AGENTS.md has one line per check on where each
	tolerance comes from.

	The control mappings, the hillshade and the audio laws the checks predict
	from are stated HERE, from the README's definitions, and never read out
	of Controls.h or the shaders: a constant typed wrong there has to show up
	as a failed check, not as an agreement. The hillshade in particular is
	stated in the cartographer's zenith / slope / aspect form, where the
	shader uses the Lambert vector form -- two statements of one law, which
	agree only if north, the azimuth's direction and the sign of the aspect
	are all right.
*/

#include "Audio.h"
#include "Clock.h"
#include "Contour.h"
#include "Controls.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace pb = contour::perturb;

int g_checks   = 0;
int g_failures = 0;

constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The mappings, stated from the README -- not read out of Controls.h.
//---------------------------------------------------------------------------
namespace stated
{
double scale( double p ) { return 2.0 * p; }
double sigma( double p ) { return 16.0 * p * p; }
double interval( double p ) { return std::max( 1.0 / 512.0, 0.25 * p * p ); }
double lineWidth( double p ) { return 4.0 * p; }
double indexWidth( double p ) { return 6.0 * p; }
double shoreWidth( double p ) { return 6.0 * p; }
double azimuth( double p ) { return 360.0 * p; }
double altitude( double p ) { return 90.0 * p; }
double zFactor( double p ) { return std::pow( 10.0, 2.0 * p - 1.0 ); }
double seaLevel( double p, double scale ) { return p * scale; }
/// The guard: one 8-bit code value of elevation over eight pixels, over
/// max( 1, sigma ) when the height is smoothed.
double guardSlope( double scale, double sigma ) { return scale / ( 255.0 * 8.0 ) / std::max( 1.0, sigma ); }
/// The sea rises by half the height range at full drive.
constexpr double kRiseRange = 0.5;
/// The detector: Level releases in 0.25 s; Onset is four times the rise
/// above a 0.1 s baseline, released in 0.3 s.
constexpr double kLevelRelease = 0.25;
constexpr double kBaselineTime = 0.1;
constexpr double kOnsetRelease = 0.3;
constexpr double kOnsetGain    = 4.0;
} // namespace stated

/// One unit in the last place of a float at magnitude v.
double ulpf( double v )
{
	const float f = static_cast< float >( std::fabs( v ) );
	return static_cast< double >( std::nextafter( f, INFINITY ) - f );
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Contour::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_RED: return "red";
	case FF_TYPE_GREEN: return "green";
	case FF_TYPE_BLUE: return "blue";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Contour& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Contour::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range. An integer's range is its own.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct r = plugin.GetParamRange( i );
			p.low               = r.min;
			p.high              = r.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Contour& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Contour& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Contour& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and a 60 fps clock.
//---------------------------------------------------------------------------
struct Session
{
	Contour plugin;
	int width        = 0;
	int height       = 0;
	double fps       = 60.0;
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip changes size: the SAME instance handed
	/// a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	/// Every bin of the spectrum, as a host would set them.
	void spectrum( const std::vector< float >& bins )
	{
		for( size_t i = 0; i < bins.size(); ++i )
			plugin.SetParamElementValue( Contour::PT_AUDIO_FFT, static_cast< unsigned int >( i ), bins[ i ] );
	}

	bool renderAtTime( double seconds )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( seconds );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed at t = %.4f\n", seconds );
		return ok;
	}

	bool renderAt( int64_t frame )
	{
		return renderAtTime( static_cast< double >( frame ) / fps );
	}

	void upload( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void upload( const std::vector< float >& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool render( int64_t frame, const std::vector< unsigned char >& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	bool render( int64_t frame, const std::vector< float >& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	/// The whole output, top row first, as floats.
	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( !quiet )
	{
		va_list args;
		va_start( args, format );
		std::vprintf( format, args );
		va_end( args );
		std::printf( "  %s\n", verdict( ok ) );
	}
	return ok ? 0 : 1;
}

/// A picture whose red, green and blue are all the elevation at each pixel,
/// alpha 1, top row first. `h( x, north )` takes the column and the row
/// counted up from the bottom, so north is up as on a map.
std::vector< float > terrain( int width, int height, const std::function< double( int, int ) >& h )
{
	std::vector< float > img( static_cast< size_t >( width ) * height * 4 );
	for( int r = 0; r < height; ++r )
		for( int x = 0; x < width; ++x )
		{
			const float v = static_cast< float >( h( x, height - 1 - r ) );
			float* px     = img.data() + ( static_cast< size_t >( r ) * width + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = v;
			px[ 3 ]                     = 1.0f;
		}
	return img;
}

/// The parameter values every check starts from: the height is the red
/// channel exactly (no luma weights to round), Height Scale 1, no smoothing,
/// interval 1/16, lines 1.2 px in black on white paper, index lines the same
/// width, no relief, no tints, no sea (level 0, and nothing is below 0), no
/// shoreline, no audio rise, no mix. Each check moves what it is about.
struct Baseline
{
	float smooth     = 0.0f;
	float interval   = 0.5f; //1/16
	float lineWidth  = 0.3f; //1.2 px
	float indexWidth = 0.2f; //1.2 px
	float indexEvery = 10.0f;
	float contours   = 1.0f;
	float hillshade  = 0.0f;
	float azimuth    = 0.875f;
	float altitude   = 0.5f;
	float zFactor    = 0.5f;
	float seaLevel   = 0.0f;
	float audioRise  = 0.0f;
	float audioMode  = 0.0f;
	float audioBand  = 0.0f;
	float shoreWidth = 0.0f;
	float water[ 3 ] = { 0.0f, 0.0f, 1.0f };
};

void apply( Contour& p, const Baseline& b )
{
	set( p, "Height Source", 1.0f );
	set( p, "Height Scale", 0.5f );
	set( p, "Invert", 0.0f );
	set( p, "Smooth", b.smooth );
	set( p, "Interval", b.interval );
	set( p, "Index Every", b.indexEvery );
	set( p, "Line Width", b.lineWidth );
	set( p, "Index Width", b.indexWidth );
	set( p, "Line Colour", 0.0f );
	set( p, "Line_Green", 0.0f );
	set( p, "Line_Blue", 0.0f );
	set( p, "Contours On", b.contours );
	set( p, "Hillshade", b.hillshade );
	set( p, "Azimuth", b.azimuth );
	set( p, "Altitude", b.altitude );
	set( p, "Z Factor", b.zFactor );
	set( p, "Tint Mode", 2.0f );
	set( p, "Tint Strength", 0.0f );
	set( p, "Sea Level", b.seaLevel );
	set( p, "Audio Rise", b.audioRise );
	set( p, "Water Colour", b.water[ 0 ] );
	set( p, "Water_Green", b.water[ 1 ] );
	set( p, "Water_Blue", b.water[ 2 ] );
	set( p, "Shore Width", b.shoreWidth );
	set( p, "Audio Mode", b.audioMode );
	set( p, "Audio Band", b.audioBand );
	set( p, "Paper Colour", 1.0f );
	set( p, "Paper_Green", 1.0f );
	set( p, "Paper_Blue", 1.0f );
	set( p, "Mix", 1.0f );
}

/// Render one still terrain and read it back. The plugin is a pure function
/// of the frame apart from the audio drive, so one frame is the picture.
bool renderTerrain( int width, int height, const Baseline& b, int perturb, const std::vector< float >& img, std::vector< float >& out )
{
	Session s;
	apply( s.plugin, b );
	s.plugin.SetPerturbForTest( perturb );
	if( !s.begin( width, height ) )
		return false;
	const bool ok = s.render( 0, img );
	if( ok )
		out = s.readBackFloat();
	s.end();
	return ok;
}

/// Ink along one row: 1 - red, on white paper with black lines.
std::vector< double > inkRow( const std::vector< float >& out, int width, int row, int channel = 0, double scaleBy = 1.0 )
{
	std::vector< double > ink( static_cast< size_t >( width ) );
	for( int x = 0; x < width; ++x )
		ink[ static_cast< size_t >( x ) ] = ( 1.0 - out[ ( static_cast< size_t >( row ) * width + x ) * 4 + channel ] ) / scaleBy;
	return ink;
}

/// A run of inked pixels along a row: its mass (the coverage summed across
/// it, which for a straight line crossing the row at right angles is the
/// line's width) and its centroid, in pixel-centre coordinates (pixel x has
/// its centre at x).
struct Run
{
	int first, last;
	double mass;
	double centroid;
};

std::vector< Run > runsOf( const std::vector< double >& ink )
{
	std::vector< Run > runs;
	const int n = static_cast< int >( ink.size() );
	for( int x = 0; x < n; )
	{
		if( ink[ static_cast< size_t >( x ) ] <= 0.0 )
		{
			++x;
			continue;
		}
		Run r { x, x, 0.0, 0.0 };
		double moment = 0.0;
		while( x < n && ink[ static_cast< size_t >( x ) ] > 0.0 )
		{
			r.mass += ink[ static_cast< size_t >( x ) ];
			moment += x * ink[ static_cast< size_t >( x ) ];
			r.last = x++;
		}
		r.centroid = moment / r.mass;
		runs.push_back( r );
	}
	return runs;
}

/// How far a sampled stroke's centroid can sit from the stroke's centre
/// line, from the lattice alone: the coverage is the stroke box convolved
/// with the pixel box, so its first moment over unit-spaced samples is the
/// centre plus the mean of a unit sawtooth over the stroke width. Whole
/// periods integrate to zero; the fraction r = frac( w ) left over integrates
/// to at most r ( 1 - r ) / 2. Zero for an integer width.
double latticeCentroidBound( double w )
{
	const double r = w - std::floor( w );
	return r * ( 1.0 - r ) / ( 2.0 * w );
}

/// How much a stroke's centroid can move from float rounding of the distance
/// on the page: each covered pixel's coverage moves by at most `eps` (its
/// slope in d is 1), the moment arm is at most ( w + 1 ) / 2, and at most
/// w + 2 pixels are covered.
double floatCentroidBound( double w, double eps )
{
	return ( w + 2.0 ) * ( w + 1.0 ) * 0.5 * eps / w;
}

/// The per-pixel error of d = | h - L | / g in float, for elevations up to
/// hmax and a slope of g per pixel: h, L and h - L each round (3 ulp at
/// hmax), the slope's two samples round (half an ulp each, halved by the
/// central difference, relative to g -- scaled by d, bounded by the line's
/// reach), the division rounds, and the readback of 1 - coverage rounds.
double distanceEps( double hmax, double g, double reach )
{
	const double u = ulpf( hmax );
	return 3.0 * u / g + reach * ( u / g + 2.0 * std::ldexp( 1.0, -24 ) ) + 2.0 * std::ldexp( 1.0, -24 );
}

//---------------------------------------------------------------------------
// --count
//
// A linear ramp h( x ) = h0 + g x along every row. The levels are k I, and a
// contour is crossed wherever a level lies strictly between the first and
// the last pixel centre. So the count is an integer with no tolerance:
//
//     n = #{ k : h( 0 ) < k I < h( W - 1 ) }
//       = floor( h( W - 1 ) / I ) - floor( h( 0 ) / I )   (neither end on a level)
//
// which is floor( R / I ) or floor( R / I ) + 1 for a ramp of range R,
// according as frac( h( 0 ) / I ) + frac( R / I ) is below 1 or not. That
// is the boundary rule. A precondition keeps every level at least a stroke
// and a pixel from either end, so no stroke is cut by the frame edge and no
// level just outside the ends leaves ink inside them.
//---------------------------------------------------------------------------
int runCount( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	Baseline b;
	const double I  = stated::interval( b.interval );
	const double w  = stated::lineWidth( b.lineWidth );
	const double margin = 0.5 * w + 1.5;

	//( nominal spacing in px, fraction of an interval at the first pixel ).
	//The spacing is then adjusted so the ramp spans N + 1/2 intervals
	//exactly: the start's and the end's fractions sit half an interval
	//apart, on opposite sides of 1/2 -- so the boundary rule is exercised
	//both ways (+0 and +1), and levels at ( k + 1/2 ) I would count one
	//more or one fewer, never the same.
	const double cases[][ 2 ] = { { 7.0, 0.25 }, { 9.5, 0.75 }, { 12.25, 0.25 }, { 16.0, 0.75 }, { 6.5, 0.25 } };
	int done = 0;
	for( const auto& c : cases )
	{
		const int N     = static_cast< int >( std::floor( ( width - 1 ) / c[ 0 ] ) );
		const double s  = ( width - 1 ) / ( N + 0.5 );
		const double g  = I / s;
		const double h0 = ( 1.0 + c[ 1 ] ) * I;
		const double hEnd = h0 + g * ( width - 1 );
		std::vector< float > img = terrain( width, height, [ & ]( int x, int ) { return h0 + g * x; } );

		//Stated count and the precondition.
		int expected = 0;
		bool clear   = N >= 3;
		for( int k = static_cast< int >( std::floor( h0 / I ) ); k <= static_cast< int >( std::ceil( hEnd / I ) ); ++k )
		{
			const double xk = ( k * I - h0 ) / g;
			if( xk > 0.0 && xk < width - 1 )
				++expected;
			if( std::fabs( xk ) < margin || std::fabs( xk - ( width - 1 ) ) < margin )
				clear = false;
		}
		if( !clear )
			continue;
		const double R    = hEnd - h0;
		const int floorRI = static_cast< int >( std::floor( R / I ) );
		const double f0 = h0 / I - std::floor( h0 / I ), fr = R / I - std::floor( R / I );
		const int rule  = floorRI + ( f0 + fr >= 1.0 ? 1 : 0 );

		std::vector< float > out;
		if( !renderTerrain( width, height, b, perturb, img, out ) )
			return failures + 1;
		int worstRow = expected;
		bool all     = true;
		for( int row : { 0, height / 2, height - 1 } )
		{
			const int n = static_cast< int >( runsOf( inkRow( out, width, row ) ).size() );
			if( n != expected )
			{
				all      = false;
				worstRow = n;
			}
		}
		++done;
		failures += report( all && rule == expected, quiet,
		                    "count: spacing %6.3f px, R/I = %6.3f: %d contours crossed, stated %d (floor(R/I) = %d, +%d by the boundary rule)",
		                    s, R / I, worstRow, expected, floorRI, rule - floorRI );
	}
	failures += report( done >= 3, quiet, "count: %d ramps met the precondition (a stroke and a pixel clear of both ends)", done );
	return failures;
}

//---------------------------------------------------------------------------
// --spacing
//
// A ramp of gradient g: adjacent isolines interval / g apart, measured as
// the distance between the centroids of their strokes. Two different checks:
//
//   whole-pixel   s an integer and every level on a pixel centre: the stroke
//                 is sampled symmetrically about its centre, so the centroid
//                 is the centre exactly and only float rounding moves it.
//   fractional    s not an integer and the levels anywhere: each centroid is
//                 off by at most the lattice's sawtooth bound for the stroke
//                 width (latticeCentroidBound), plus rounding.
//
// Each checks every centroid against its level's position, every adjacent
// spacing against s, and the least-squares spacing over the whole ramp.
//---------------------------------------------------------------------------
int runSpacing( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	Baseline b;
	const double I = stated::interval( b.interval );
	const double w = stated::lineWidth( b.lineWidth );

	struct Case
	{
		double s;
		bool whole;
		double phase;
	};
	const Case cases[] = { { 8.0, true, 0.0 }, { 12.0, true, 0.0 }, { 16.0, true, 0.0 },
		                   { 7.3, false, 0.37 }, { 10.6, false, 0.81 }, { 13.9, false, 0.12 } };
	for( const Case& c : cases )
	{
		const double g  = I / c.s;
		//Whole: level k at column x0 + k s exactly, x0 an integer. Fractional:
		//the first level at a fraction of a pixel.
		const double x0 = c.whole ? 5.0 : 5.0 + c.phase;
		const double h0 = 2.0 * I;//elevation at x0: level k = 2
		std::vector< float > img = terrain( width, height, [ & ]( int x, int ) { return h0 + g * ( x - x0 ); } );
		std::vector< float > out;
		if( !renderTerrain( width, height, b, perturb, img, out ) )
			return failures + 1;

		const double hmax = h0 + g * width;
		const double eps  = distanceEps( hmax, g, 0.5 * w + 1.0 );
		const double tol  = ( c.whole ? 0.0 : latticeCentroidBound( w ) ) + floatCentroidBound( w, eps );

		const std::vector< Run > runs = runsOf( inkRow( out, width, height / 2 ) );
		double worstPos = 0.0, worstStep = 0.0;
		int matched     = 0;
		std::vector< std::pair< double, double > > fit;//( k, centroid )
		for( const Run& r : runs )
		{
			//Runs touching the frame edge are cut, not measured.
			if( r.first == 0 || r.last == width - 1 )
				continue;
			const double k   = std::round( ( r.centroid - x0 ) / c.s );
			const double pos = x0 + k * c.s;
			worstPos         = std::max( worstPos, std::fabs( r.centroid - pos ) );
			fit.emplace_back( k, r.centroid );
			++matched;
		}
		for( size_t i = 1; i < fit.size(); ++i )
			worstStep = std::max( worstStep, std::fabs( ( fit[ i ].second - fit[ i - 1 ].second ) / ( fit[ i ].first - fit[ i - 1 ].first ) - c.s ) );
		double sk = 0, sc = 0, skk = 0, skc = 0;
		for( const auto& f : fit )
		{
			sk += f.first;
			sc += f.second;
			skk += f.first * f.first;
			skc += f.first * f.second;
		}
		const double n     = static_cast< double >( fit.size() );
		const double slope = n > 1 ? ( n * skc - sk * sc ) / ( n * skk - sk * sk ) : 0.0;
		//The fit's error: the least-squares slope is sum( k - kbar ) e_k /
		//sum( k - kbar )^2, and every |e_k| <= tol, so it is off by at most
		//tol sum| k - kbar | / sum( k - kbar )^2 -- exactly, for these k.
		double absDev = 0.0, sqDev = 0.0;
		for( const auto& f : fit )
		{
			absDev += std::fabs( f.first - sk / n );
			sqDev += ( f.first - sk / n ) * ( f.first - sk / n );
		}
		const double fitTol = sqDev > 0.0 ? tol * absDev / sqDev : 0.0;

		const bool enough = matched >= 5;
		failures += report( enough && worstPos <= tol && worstStep <= 2.0 * tol && std::fabs( slope - c.s ) <= fitTol, quiet,
		                    "spacing: %-10s s = %5.2f px: %2d isolines, centroid off %.2e (tol %.2e), step off %.2e (tol %.2e), fitted %.5f px (tol %.1e)",
		                    c.whole ? "whole-px" : "fractional", c.s, matched, worstPos, tol, worstStep, 2.0 * tol, slope, fitTol );
	}
	return failures;
}

/// The generalising Gaussian as the README states it: sigma = 16 p^2, radius
/// ceil( 3 sigma ), renormalised over what is kept. Index k is distance k.
std::vector< double > statedWeights( double sigma )
{
	const int radius = sigma > 0.0 ? std::min( 48, static_cast< int >( std::ceil( 3.0 * sigma ) ) ) : 0;
	std::vector< double > w( static_cast< size_t >( radius + 1 ) );
	double total = 0.0;
	for( int k = 0; k <= radius; ++k )
	{
		w[ static_cast< size_t >( k ) ] = radius == 0 ? 1.0 : std::exp( -0.5 * k * k / ( sigma * sigma ) );
		total += k == 0 ? w[ static_cast< size_t >( k ) ] : 2.0 * w[ static_cast< size_t >( k ) ];
	}
	for( double& v : w )
		v /= total;
	return w;
}

//---------------------------------------------------------------------------
// --width -- the headline claim.
//
// One terrain whose slope runs 8:1 across the frame, constant down every
// column: a plane at g0 for the western third, a plane at 8 g0 for the
// eastern third, and between them a quadratic that joins the two with a
// continuous slope. Every stroke crosses the row at right angles, so its
// mass along the row is its width. The claim: every contour is the same
// width on the page -- Line Width, or Index Width for an index contour --
// on the steep side, the gentle side and the bend alike. Dropping the
// division by |grad h| makes the width follow 1 / slope and must fail by a
// factor of up to eight.
//
// The tolerance is the distance estimate's, derived per stroke. On a
// curved profile |h - L| / |h'| is the true distance times
// ( 1 - kappa d / 2 + ... ), kappa = |h''| / h', so each covered sample's
// coverage moves by at most kappa d^2 / 2 (the coverage is 1-Lipschitz in
// d); at most four samples sit on the stroke's two one-pixel ramps, at
// d <= ( w + 1 ) / 2, so the mass moves by at most kappa ( w + 1 )^2 / 2,
// with kappa the largest over the stroke's reach plus a pixel (the central
// difference's). On the planes kappa = 0 and only float rounding is left.
//---------------------------------------------------------------------------
int runWidth( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( const float smooth : { 0.0f, 0.35f } )
	{
		Baseline b;
		b.smooth     = smooth;
		b.indexEvery = 5.0f;
		b.indexWidth = 0.4f;//2.4 px
		const double I  = stated::interval( b.interval );
		const double w  = stated::lineWidth( b.lineWidth );
		const double wi = stated::indexWidth( b.indexWidth );
		const int every = static_cast< int >( b.indexEvery );

		const int m     = 16;//past the blur's reach of the frame edge, ceil( 3 sigma ) = 6
		const double D  = width - 1 - 2 * m;
		const double L1 = D / 3.0, L2 = 2.0 * D / 3.0;//where the bend starts and ends
		const double g0 = I / 32.0;                    //32 px between contours on the gentle side, 4 on the steep
		const double a  = 7.0 * g0 / ( 2.0 * ( L2 - L1 ) );
		auto slopeAt    = [ & ]( double u ) { return u <= L1 ? g0 : u >= L2 ? 8.0 * g0 : g0 + 2.0 * a * ( u - L1 ); };
		auto curveAt    = [ & ]( double u ) { return u <= L1 || u >= L2 ? 0.0 : 2.0 * a; };
		auto hOf        = [ & ]( double u ) {
			const double b1 = std::min( u, L1 ), b2 = std::clamp( u, L1, L2 ) - L1, b3 = std::max( u, L2 ) - L2;
			return 3.0 * I + g0 * b1 + ( g0 * b2 + a * b2 * b2 ) + 8.0 * g0 * b3;
		};
		std::vector< float > img = terrain( width, height, [ & ]( int x, int ) { return hOf( x - m ); } );
		std::vector< float > out;
		if( !renderTerrain( width, height, b, perturb, img, out ) )
			return failures + 1;

		const double hmax = hOf( width ) + 1.0;
		const std::vector< Run > runs = runsOf( inkRow( out, width, height / 2 ) );
		double worstRatio = 0.0, worstErr = 0.0, worstTol = 0.0, minSlope = 1e9, maxSlope = 0.0, minW = 1e9, maxW = 0.0;
		int measured = 0, indexSeen = 0;
		for( const Run& r : runs )
		{
			if( r.first < m || r.last > width - 1 - m )
				continue;
			//Which level: the elevation at the centroid, on the stated profile.
			const double u      = r.centroid - m;
			const int k         = static_cast< int >( std::lround( hOf( u ) / I ) );
			const bool isIndex  = ( ( k % every ) + every ) % every == 0;
			const double expect = isIndex ? wi : w;
			//The smoothing spreads the bend's curvature by its own radius.
			const double reach  = 0.5 * ( expect + 1.0 ) + 1.0 + static_cast< double >( statedWeights( stated::sigma( smooth ) ).size() - 1 );
			double kappa        = 0.0;
			for( double v = u - reach; v <= u + reach; v += 0.25 )
				kappa = std::max( kappa, curveAt( v ) / slopeAt( v ) );
			const double slope = slopeAt( u );
			const double tol   = 0.5 * kappa * ( expect + 1.0 ) * ( expect + 1.0 )
			                   + ( expect + 2.0 ) * distanceEps( hmax, slope, 0.5 * expect + 1.0 );
			const double err = std::fabs( r.mass - expect );
			if( err / tol > worstRatio )
			{
				worstRatio = err / tol;
				worstErr   = err;
				worstTol   = tol;
			}
			minSlope = std::min( minSlope, slope );
			maxSlope = std::max( maxSlope, slope );
			if( !isIndex )
			{
				minW = std::min( minW, r.mass );
				maxW = std::max( maxW, r.mass );
			}
			indexSeen += isIndex ? 1 : 0;
			++measured;
		}
		const bool spans = maxSlope >= 8.0 * minSlope * ( 1.0 - 1e-12 );
		failures += report( measured >= 10 && indexSeen >= 2 && spans && worstRatio <= 1.0, quiet,
		                    "width: Smooth %.2f: %d contours (%d index) on slopes %.2f:1, line widths %.4f..%.4f px (stated %.2f), worst error %.2e against its tol %.2e",
		                    smooth, measured, indexSeen, maxSlope / std::max( minSlope, 1e-12 ), minW, maxW, w, worstErr, worstTol );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --hillshade
//
// Planes h = c + a x + b y (x east, y north, in pixels), shaded alone: white
// paper, no tints, no lines, Hillshade 1, so every pixel IS the hillshade.
// The expectation is the cartographer's formula, stated here in its own
// form:
//
//     hs = cos( zenith ) cos( slope ) + sin( zenith ) sin( slope ) cos( azimuth - aspect )
//
// with zenith = 90 - altitude, the surface lifted by Z Factor times the
// frame height, slope = atan |grad z|, and aspect the compass bearing of the
// downhill direction, clockwise from north. Clamped at 0.
//
// Tolerance: the planes' elevations are rounded to float on the way in
// (half an ulp at the largest), so each gradient component is off by at
// most half an ulp, lifted by Z H into the slope; |d hs / d z_i| <= 1; plus
// a few ulps of normalize, dot and the light's uniform.
//---------------------------------------------------------------------------
double statedHillshade( double zx, double zy, double azimuthDeg, double altitudeDeg )
{
	const double zenith = ( 90.0 - altitudeDeg ) * kPi / 180.0;
	const double az     = azimuthDeg * kPi / 180.0;
	const double grad   = std::sqrt( zx * zx + zy * zy );
	const double slope  = std::atan( grad );
	const double aspect = grad > 0.0 ? std::atan2( -zx, -zy ) : 0.0;//compass bearing of ( east, north ) = -grad
	const double hs     = std::cos( zenith ) * std::cos( slope ) + std::sin( zenith ) * std::sin( slope ) * std::cos( az - aspect );
	return std::max( 0.0, hs );
}

int runHillshade( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	struct Case
	{
		double slopeDeg, aspectDeg;//of the plane, downhill bearing
		float azimuth, altitude, zFactor;
	};
	const Case cases[] = {
		{ 0.0, 0.0, 0.875f, 0.5f, 0.5f },     //flat: sin( 45 ) everywhere
		{ 30.0, 315.0, 0.875f, 0.5f, 0.5f },  //facing the light
		{ 30.0, 135.0, 0.875f, 0.5f, 0.5f },  //facing away
		{ 30.0, 45.0, 0.875f, 0.5f, 0.5f },   //side-lit
		{ 20.0, 0.0, 0.875f, 0.5f, 0.5f },    //facing north
		{ 20.0, 90.0, 0.875f, 0.5f, 0.5f },   //facing east
		{ 60.0, 135.0, 0.875f, 0.5f, 0.5f },  //steep, facing away: clamped at 0
		{ 25.0, 170.0, 0.5f, 0.3333333f, 0.5f },//light from the south at 30 degrees
		{ 25.0, 250.0, 0.125f, 0.6666667f, 0.5f },
		{ 10.0, 300.0, 0.875f, 0.5f, 0.75f },  //Z Factor 10^0.5
	};
	for( const Case& c : cases )
	{
		Baseline b;
		b.contours  = 0.0f;
		b.hillshade = 1.0f;
		b.azimuth   = c.azimuth;
		b.altitude  = c.altitude;
		b.zFactor   = c.zFactor;
		const double Z   = stated::zFactor( c.zFactor );
		const double lift = Z * height;
		//The plane's gradient in elevation per pixel, from the slope and the
		//downhill bearing: grad h points UPHILL, opposite the aspect.
		const double t  = std::tan( c.slopeDeg * kPi / 180.0 ) / lift;
		const double ga = -t * std::sin( c.aspectDeg * kPi / 180.0 );//east
		const double gb = -t * std::cos( c.aspectDeg * kPi / 180.0 );//north
		double lo = std::min( { 0.0, ga * ( width - 1 ), gb * ( height - 1 ), ga * ( width - 1 ) + gb * ( height - 1 ) } );
		const double c0 = 0.25 - lo;
		std::vector< float > img = terrain( width, height, [ & ]( int x, int y ) { return c0 + ga * x + gb * y; } );
		std::vector< float > out;
		if( !renderTerrain( width, height, b, perturb, img, out ) )
			return failures + 1;

		double hmax = c0 + std::fabs( ga ) * width + std::fabs( gb ) * height;
		const double expected = statedHillshade( lift * ga, lift * gb, stated::azimuth( c.azimuth ), stated::altitude( c.altitude ) );
		const double tol      = lift * std::sqrt( 2.0 ) * 0.5 * ulpf( hmax ) + 16.0 * std::ldexp( 1.0, -24 );
		double worst          = 0.0;
		for( int r = 1; r < height - 1; ++r )
			for( int x = 1; x < width - 1; ++x )
				for( int ch = 0; ch < 3; ++ch )
					worst = std::max( worst, std::fabs( out[ ( static_cast< size_t >( r ) * width + x ) * 4 + ch ] - expected ) );
		failures += report( worst <= tol, quiet, "hillshade: slope %4.1f facing %5.1f, light %5.1f at %4.1f, Z %.2f: stated %.6f, worst error %.2e (tol %.2e)",
		                    c.slopeDeg, c.aspectDeg, stated::azimuth( c.azimuth ), stated::altitude( c.altitude ), Z, expected, worst, tol );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --sea
//
// Water is h < sea level, exactly: a terrain of a hill, a basin and a
// plateau lying EXACTLY at sea level (which is land: not below), rendered
// with water black and paper white, so every pixel's red is 1 or 0 and must
// be 0 exactly where the stated elevation at that pixel is below the sea.
// No smoothing, so the elevation the shader sees is the float the harness
// wrote. No tolerance.
//
// The shoreline is the isoline at sea level: on a ramp along x, and one
// along y, with water and paper both white, the stroke is the only ink. Its
// centroid sits on the ramp's sea-level position to within the lattice's
// sawtooth bound for the stroke's width plus rounding -- the spec asks for
// half a pixel, and the derived bound is tighter, so that is what is used.
//---------------------------------------------------------------------------
int runSea( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	{
		Baseline b;
		b.contours       = 0.0f;
		b.seaLevel       = 0.4f;
		const float S    = static_cast< float >( stated::seaLevel( b.seaLevel, stated::scale( 0.5 ) ) );
		const double cx  = 0.35 * width, cy = 0.5 * height, R = 0.3 * height;
		auto hAt = [ & ]( int x, int y ) -> double {
			if( x > width * 3 / 4 )
				return S;//the plateau, at sea level exactly
			const double dx = x - cx, dy = y - cy;
			return 0.1 + 0.6 * std::exp( -( dx * dx + dy * dy ) / ( 2.0 * R * R ) ) + 0.2 * x / width;
		};
		std::vector< float > img = terrain( width, height, hAt );
		std::vector< float > out;
		if( !renderTerrain( width, height, b, perturb, img, out ) )
			return failures + 1;
		int wrong = 0, water = 0, land = 0, onLevel = 0;
		for( int r = 0; r < height; ++r )
			for( int x = 0; x < width; ++x )
			{
				const float h     = img[ ( static_cast< size_t >( r ) * width + x ) * 4 ];
				const bool below  = h < S;
				const float red   = out[ ( static_cast< size_t >( r ) * width + x ) * 4 ];
				const bool isSea  = red == 0.0f;
				const bool isLand = red == 1.0f;
				if( below ? !isSea : !isLand )
					++wrong;
				water += below ? 1 : 0;
				land += below ? 0 : 1;
				onLevel += h == S ? 1 : 0;
			}
		failures += report( wrong == 0 && water > 0 && land > 0 && onLevel > 0, quiet,
		                    "sea: %d water and %d land pixels (%d exactly at sea level, land): %d misclassified", water, land, onLevel, wrong );
	}

	for( const float shore : { 0.3f, 0.5f } )
		for( const bool alongX : { true, false } )
			for( const double phase : { 0.0, 0.37 } )
			{
				Baseline b;
				b.contours   = 0.0f;
				b.seaLevel   = 0.5f;
				b.shoreWidth = shore;
				b.water[ 0 ] = b.water[ 1 ] = b.water[ 2 ] = 1.0f;
				const double S  = static_cast< float >( stated::seaLevel( b.seaLevel, stated::scale( 0.5 ) ) );
				const double ws = stated::shoreWidth( shore );
				const int n     = alongX ? width : height;
				const double g  = 1.0 / 64.0;
				//The sea-level isoline at pixel centre n/2 + phase (counted
				//from the west, or from the south).
				const double xs = std::floor( 0.5 * n ) + phase;
				std::vector< float > img = terrain( width, height, [ & ]( int x, int y ) { return S + g * ( ( alongX ? x : y ) - xs ); } );
				std::vector< float > out;
				if( !renderTerrain( width, height, b, perturb, img, out ) )
					return failures + 1;
				//The stroke's colour is 0.4 of the water's, so ink = ( 1 - v ) / 0.6.
				std::vector< double > profile( static_cast< size_t >( n ) );
				for( int i = 0; i < n; ++i )
				{
					const int x = alongX ? i : width / 2;
					const int r = alongX ? height / 2 : height - 1 - i;
					profile[ static_cast< size_t >( i ) ] = ( 1.0 - out[ ( static_cast< size_t >( r ) * width + x ) * 4 ] ) / 0.6;
				}
				const std::vector< Run > runs = runsOf( profile );
				const double eps = distanceEps( S + g * n, g, 0.5 * ws + 1.0 ) + 4.0 * ulpf( 1.0 ) / 0.6;
				const double tol = latticeCentroidBound( ws ) + floatCentroidBound( ws, eps );
				const bool one   = runs.size() == 1;
				const double off = one ? runs[ 0 ].centroid - xs : 1e9;
				failures += report( one && std::fabs( off ) <= tol && tol <= 0.5, quiet,
				                    "sea: shoreline %.1f px, isoline running %s at +%.2f px: stroke centred %+.2e px off (tol %.2e; spec 0.5)",
				                    ws, alongX ? "north-south" : "east-west ", phase, off, tol );
			}
	return failures;
}

//---------------------------------------------------------------------------
// --index
//
// A ramp at 10 px a contour, index strokes 3 px against 1.2: the heavy
// strokes are exactly the levels k = 0 mod N, for N = 2, 3, 5 and 10, and
// nothing else is heavy. Classified by each stroke's mass against the
// midpoint of the two widths, which on a straight ramp are exact. Integer
// counts, no tolerance.
//---------------------------------------------------------------------------
int runIndex( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( const int every : { 2, 3, 5, 10 } )
	{
		Baseline b;
		b.indexEvery = static_cast< float >( every );
		b.indexWidth = 0.5f;//3 px
		const double I  = stated::interval( b.interval );
		const double w  = stated::lineWidth( b.lineWidth );
		const double wi = stated::indexWidth( b.indexWidth );
		const double s  = 10.0;
		const double g  = I / s;
		const double x0 = 4.3;//level k = 1 at x0
		std::vector< float > img = terrain( width, height, [ & ]( int x, int ) { return I + g * ( x - x0 ); } );
		std::vector< float > out;
		if( !renderTerrain( width, height, b, perturb, img, out ) )
			return failures + 1;
		std::set< int > heavy, stated;
		int strokes = 0, expectedStrokes = 0;
		for( const Run& r : runsOf( inkRow( out, width, height / 2 ) ) )
		{
			if( r.first == 0 || r.last == width - 1 )
				continue;
			const int k = 1 + static_cast< int >( std::lround( ( r.centroid - x0 ) / s ) );
			++strokes;
			if( r.mass > 0.5 * ( w + wi ) )
				heavy.insert( k );
		}
		//A stroke of width wk puts ink on pixel x iff | x - xk | < ( wk + 1 ) / 2;
		//one that reaches the first or last pixel is cut, and not counted on
		//either side.
		for( int k = 1;; ++k )
		{
			const double xk = x0 + ( k - 1 ) * s;
			const double wk = k % every == 0 ? wi : w;
			if( xk > width - 1 )
				break;
			if( xk < 0.5 * ( wk + 1.0 ) || ( width - 1 ) - xk < 0.5 * ( wk + 1.0 ) )
				continue;
			++expectedStrokes;
			if( k % every == 0 )
				stated.insert( k );
		}
		failures += report( heavy == stated && strokes == expectedStrokes && !stated.empty(), quiet,
		                    "index: every %2d: %zu heavy of %d strokes, stated %zu of %d, the same levels: %s", every, heavy.size(), strokes,
		                    stated.size(), expectedStrokes, heavy == stated ? "yes" : "NO" );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --flat
//
// The guard, proved. The left of the frame is a plateau lying EXACTLY on a
// contour level (h = 8 I) with a noise floor on it; every zero-crossing of
// the noise is an isoline of the model. The right is a ramp, so the frame
// is not simply blank. At the default Smooth the noise's amplitude a is
// chosen so that the smoothed gradient provably stays under the guard:
//
//     |d/dx ( G * n )| <= a * sum_k | w_{k+1} - w_{k-1} | / 2   per axis
//
// (the central difference of the blurred noise, each sample at most a; the
// y pass has unit gain), times sqrt 2 for the length -- with w the stated
// Gaussian. With that below the guard slope, the claim is exact: NO ink
// anywhere on the plateau. Without the guard the same plateau is a
// labyrinth, which is the negative control.
//
// A stronger noise floor (two code values) is also rendered and its ink
// reported, for the record: the guard bounds the width of what it lets
// through, it does not promise a blank page on any noise.
//---------------------------------------------------------------------------
/// A PCG output mix, exact in 32 bits: the same noise on every machine.
uint32_t hashInt( uint32_t v )
{
	uint32_t state = v * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

double noiseAt( int x, int y )
{
	return static_cast< double >( hashInt( static_cast< uint32_t >( x ) * 7919u + static_cast< uint32_t >( y ) * 104729u + 17u ) )
	           / 4294967295.0 * 2.0
	       - 1.0;
}

int runFlat( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	Baseline b;
	b.smooth = 0.35f;
	const double I     = stated::interval( b.interval );
	const double sigma = stated::sigma( b.smooth );
	const std::vector< double > wts = statedWeights( sigma );
	const int radius   = static_cast< int >( wts.size() ) - 1;
	double tv          = 0.0;
	for( int k = -radius - 1; k <= radius + 1; ++k )
	{
		auto at = [ & ]( int j ) { return std::abs( j ) <= radius ? wts[ static_cast< size_t >( std::abs( j ) ) ] : 0.0; };
		tv += std::fabs( at( k + 1 ) - at( k - 1 ) );
	}
	const double guard = stated::guardSlope( stated::scale( 0.5 ), sigma );
	const double level = 8.0 * I;
	const int edge     = width / 2;
	const int keep     = radius + 2;//the plateau's interior, clear of the ramp's blur

	for( const double codeValues : { 0.1, 2.0 } )
	{
		const double a     = codeValues / 255.0;
		const double bound = a * 0.5 * tv * std::sqrt( 2.0 );
		std::vector< float > img = terrain( width, height, [ & ]( int x, int y ) {
			if( x < edge )
				return level + a * noiseAt( x, y );
			return level + ( x - edge ) * I / 12.0;
		} );
		std::vector< float > out;
		if( !renderTerrain( width, height, b, perturb, img, out ) )
			return failures + 1;
		double ink = 0.0;
		int area   = 0;
		for( int r = keep; r < height - keep; ++r )
			for( int x = keep; x < edge - keep; ++x )
			{
				ink += 1.0 - out[ ( static_cast< size_t >( r ) * width + x ) * 4 ];
				++area;
			}
		const size_t rampRuns = runsOf( inkRow( out, width, height / 2 ) ).size();
		if( codeValues < 1.0 )
			failures += report( bound < guard && ink == 0.0 && rampRuns >= 3, quiet,
			                    "flat: a plateau ON a level, noise +-%.1f code values (gradient <= %.2e, guard %.2e): ink %.3g over %d px; the ramp beside it draws %zu",
			                    codeValues, bound, guard, ink, area, rampRuns );
		else if( !quiet )
			std::printf( "flat: for the record, noise +-%.1f code values (gradient up to %.2e, over the guard): ink %.1f px-equivalents, %.3f%% of the plateau\n",
			             codeValues, bound, ink, 100.0 * ink / std::max( area, 1 ) );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --audio
//
// The sea rising, read out of the picture: a ramp from 0 to 1 west to east,
// water black, sea level 0.2, Audio Rise 1, so the waterline's column is the
// effective sea level to within one column of ramp (1 / W of elevation --
// the lattice, and the whole tolerance).
//
//   primed      Onset, a loud constant spectrum from frame 0: the sea never
//               moves. An unprimed detector floods on frame 0.
//   onset       silence, then a step to loud at frame 30: the drive
//               saturates (an onset of more than a quarter of full scale
//               times the stated gain of 4), so the sea stands at 0.2 + 0.5
//               on that frame, then drains, monotonically, to within 0.05
//               of the base after one second (e^-1/0.3 = 0.036 of the rise).
//   level       Level, constant spectra v: the sea at 0.2 + 0.5 sqrt( v ).
//   band        loud bass bins only: Bass raises the sea, Treble does not.
//   scrub       loud and steady, then the clock jumps back 5 s: re-primed,
//               the sea does not move.
//---------------------------------------------------------------------------
int waterlineColumns( const std::vector< float >& out, int width, int row )
{
	int n = 0;
	for( int x = 0; x < width; ++x )
		n += out[ ( static_cast< size_t >( row ) * width + x ) * 4 ] == 0.0f ? 1 : 0;
	return n;
}

int runAudio( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	const double base = 0.2;
	auto ramp         = [ & ]( int x, int ) { return static_cast< double >( x ) / width; };
	const std::vector< float > img = terrain( width, height, ramp );
	const double cell = 1.0 / width;
	//The elevation the waterline says: n columns are water, so the sea is in
	//( h( n - 1 ), h( n ) ]; its midpoint is within half a cell.
	auto seaOf = [ & ]( int n ) { return ( n - 0.5 ) / width; };

	auto configure = [ & ]( Session& s, float mode, float band ) {
		Baseline b;
		b.contours  = 0.0f;
		b.seaLevel  = static_cast< float >( base );
		b.audioRise = 1.0f;
		b.audioMode = mode;
		b.audioBand = band;
		b.water[ 2 ] = 0.0f;
		apply( s.plugin, b );
		s.plugin.SetPerturbForTest( perturb );
	};
	std::vector< float > loud( 64, 0.64f ), silent( 64, 0.0f ), bass( 64, 0.0f );
	for( int i = 0; i < 8; ++i )
		bass[ static_cast< size_t >( i ) ] = 0.64f;

	//primed
	{
		Session s;
		configure( s, 1.0f, 0.0f );
		if( !s.begin( width, height ) )
			return failures + 1;
		s.spectrum( loud );
		double worst = 0.0;
		for( int f = 0; f < 60; ++f )
		{
			s.render( f, img );
			worst = std::max( worst, std::fabs( seaOf( waterlineColumns( s.readBackFloat(), width, height / 2 ) ) - base ) );
		}
		s.end();
		failures += report( worst <= cell, quiet, "audio: Onset, loud from the first frame: the sea stays at %.2f to %.2e over 60 frames (tol one column, %.2e)",
		                    base, worst, cell );
	}
	//onset
	{
		Session s;
		configure( s, 1.0f, 0.0f );
		if( !s.begin( width, height ) )
			return failures + 1;
		double at30 = 0.0, at90 = 0.0, before = 0.0, prev = 1e9;
		bool monotone = true;
		for( int f = 0; f <= 90; ++f )
		{
			s.spectrum( f < 30 ? silent : loud );
			s.render( f, img );
			const double sea = seaOf( waterlineColumns( s.readBackFloat(), width, height / 2 ) );
			if( f == 29 )
				before = sea;
			if( f == 30 )
				at30 = sea;
			if( f >= 30 )
			{
				monotone = monotone && sea <= prev + 1e-12;
				prev     = sea;
			}
			at90 = sea;
		}
		s.end();
		const double peak = base + stated::kRiseRange;
		//The surge holds while four times the rise above the baseline is at
		//least 1: t_h = tau_b ln( 4 x ), x = 0.8. Then it releases with tau_r.
		const double hold  = stated::kBaselineTime * std::log( 4.0 * 0.8 );
		const double drain = stated::kRiseRange * std::exp( -( 1.0 - hold ) / stated::kOnsetRelease );
		failures += report( std::fabs( before - base ) <= cell && std::fabs( at30 - peak ) <= cell && monotone && at90 - base <= drain + cell, quiet,
		                    "audio: Onset, a step at frame 30: %.3f before, %.3f on it (stated %.3f), draining monotonically to %.4f a second later (stated <= %.4f)",
		                    before, at30, peak, at90, base + drain );
	}
	//level
	for( const float v : { 0.04f, 0.25f, 0.64f } )
	{
		Session s;
		configure( s, 0.0f, 0.0f );
		if( !s.begin( width, height ) )
			return failures + 1;
		s.spectrum( std::vector< float >( 64, v ) );
		double sea = 0.0;
		for( int f = 0; f < 3; ++f )
		{
			s.render( f, img );
			sea = seaOf( waterlineColumns( s.readBackFloat(), width, height / 2 ) );
		}
		s.end();
		const double stated = base + stated::kRiseRange * std::sqrt( static_cast< double >( v ) );
		failures += report( std::fabs( sea - stated ) <= cell, quiet, "audio: Level, every bin %.2f: the sea at %.4f, stated %.4f (tol %.2e)", v, sea, stated, cell );
	}
	//band
	{
		double seaBass = 0.0, seaTreble = 0.0;
		for( const float band : { 1.0f, 3.0f } )
		{
			Session s;
			configure( s, 0.0f, band );
			if( !s.begin( width, height ) )
				return failures + 1;
			s.spectrum( bass );
			s.render( 0, img );
			( band == 1.0f ? seaBass : seaTreble ) = seaOf( waterlineColumns( s.readBackFloat(), width, height / 2 ) );
			s.end();
		}
		failures += report( std::fabs( seaBass - ( base + 0.4 ) ) <= cell && std::fabs( seaTreble - base ) <= cell, quiet,
		                    "audio: bass bins only: Bass band raises the sea to %.4f (stated %.4f), Treble leaves it at %.4f", seaBass, base + 0.4, seaTreble );
	}
	//scrub
	{
		Session s;
		configure( s, 1.0f, 0.0f );
		if( !s.begin( width, height ) )
			return failures + 1;
		s.spectrum( loud );
		double worst = 0.0;
		for( int f = 0; f < 40; ++f )
		{
			s.upload( img );
			s.renderAtTime( f < 20 ? 10.0 + f / 60.0 : 5.0 + f / 60.0 );
			worst = std::max( worst, std::fabs( seaOf( waterlineColumns( s.readBackFloat(), width, height / 2 ) ) - base ) );
		}
		s.end();
		failures += report( worst <= cell, quiet, "audio: Onset, loud and steady across a scrub back 5 s: the sea stays within %.2e (tol %.2e)", worst, cell );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//
// The same instance handed a differently sized clip mid-run, as a host does:
//   - the audio drive is carried through it (Onset, loud and steady: the sea
//     never moves), and
//   - the first frame after it is byte for byte a fresh instance's render of
//     the same picture at the new size, with Smooth on -- no buffer of the
//     old size survives into it.
//---------------------------------------------------------------------------
std::vector< float > hills( int width, int height )
{
	return terrain( width, height, [ & ]( int x, int y ) {
		const double u = static_cast< double >( x ) / width, v = static_cast< double >( y ) / height;
		return 0.5 + 0.3 * std::sin( 7.0 * u + 2.0 * v ) * std::cos( 5.0 * v - u ) + 0.1 * u;
	} );
}

int runResize( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	Baseline b;
	b.smooth     = 0.35f;
	b.seaLevel   = 0.4f;
	b.audioRise  = 1.0f;
	b.audioMode  = 1.0f;
	b.shoreWidth = 0.3f;
	b.hillshade  = 0.5f;
	std::vector< float > loud( 64, 0.64f );
	const int w2 = width * 3 / 2 + 1, h2 = height * 3 / 4 + 1;

	Session s;
	apply( s.plugin, b );
	s.plugin.SetPerturbForTest( perturb );
	if( !s.begin( width, height ) )
		return 1;
	s.spectrum( loud );
	const std::vector< float > first = hills( width, height );
	float worstDrive                 = 0.0f;
	for( int f = 0; f < 20; ++f )
	{
		s.render( f, first );
		worstDrive = std::max( worstDrive, s.plugin.DriveForTest() );
	}
	s.resize( w2, h2 );
	const std::vector< float > second = hills( w2, h2 );
	std::vector< float > after;
	for( int f = 20; f < 40; ++f )
	{
		s.render( f, second );
		if( f == 20 )
			after = s.readBackFloat();
		worstDrive = std::max( worstDrive, s.plugin.DriveForTest() );
	}
	s.end();

	Session fresh;
	apply( fresh.plugin, b );
	if( !fresh.begin( w2, h2 ) )
		return 1;
	fresh.spectrum( loud );
	fresh.render( 0, second );
	const std::vector< float > expect = fresh.readBackFloat();
	fresh.end();

	size_t differ = after.size() == expect.size() ? 0 : after.size();
	for( size_t i = 0; i < std::min( after.size(), expect.size() ); ++i )
		differ += after[ i ] != expect[ i ] ? 1 : 0;
	failures += report( worstDrive == 0.0f, quiet, "resize: %dx%d -> %dx%d mid-run, Onset loud and steady: the drive stays %.3g", width, height, w2, h2,
	                    static_cast< double >( worstDrive ) );
	failures += report( differ == 0, quiet, "resize: the first frame after it against a fresh instance at %dx%d: %zu of %zu values differ", w2, h2, differ,
	                    expect.size() );
	return failures;
}

//---------------------------------------------------------------------------
// Offline: no GL.
//---------------------------------------------------------------------------
int runControls( bool quiet = false )
{
	using namespace contour::controls;
	int failures = 0;
	double worst = 0.0;
	for( int i = 0; i <= 20; ++i )
	{
		const float p = static_cast< float >( i ) / 20.0f;
		const double q = static_cast< double >( p );
		const double pairs[][ 2 ] = {
			{ HeightScaleFromParam( p ), stated::scale( q ) },       { SmoothSigmaFromParam( p ), stated::sigma( q ) },
			{ IntervalFromParam( p ), stated::interval( q ) },       { LineWidthFromParam( p ), stated::lineWidth( q ) },
			{ IndexWidthFromParam( p ), stated::indexWidth( q ) },   { ShoreWidthFromParam( p ), stated::shoreWidth( q ) },
			{ AzimuthDegreesFromParam( p ), stated::azimuth( q ) }, { AltitudeDegreesFromParam( p ), stated::altitude( q ) },
			{ ZFactorFromParam( p ), stated::zFactor( q ) },         { SeaLevelFromParam( p, 1.5 ), stated::seaLevel( q, 1.5 ) },
		};
		for( const auto& pr : pairs )
			worst = std::max( worst, std::fabs( pr[ 0 ] - pr[ 1 ] ) / std::max( 1.0, std::fabs( pr[ 1 ] ) ) );
	}
	failures += report( worst <= 1e-12, quiet, "controls: ten mappings at 21 points each against the README's statement: worst relative %.1e", worst );
	double guardWorst = 0.0;
	for( const double sg : { 0.0, 0.5, 1.0, 1.96, 7.3, 16.0 } )
		for( const double sc : { 0.25, 1.0, 2.0 } )
			guardWorst = std::max( guardWorst, std::fabs( GuardSlope( sc, sg ) - stated::guardSlope( sc, sg ) ) / stated::guardSlope( sc, sg ) );
	failures += report( guardWorst <= 1e-15 && kAudioRiseRange == stated::kRiseRange, quiet,
	                    "controls: the guard is one code value over 8 px, over max( 1, sigma ) (worst relative %.1e); the rise half the range", guardWorst );
	//The defaults an operator meets.
	Contour plugin;
	const double az  = AzimuthDegreesFromParam( plugin.GetFloatParameter( Contour::PT_AZIMUTH ) );
	const double alt = AltitudeDegreesFromParam( plugin.GetFloatParameter( Contour::PT_ALTITUDE ) );
	const double z   = ZFactorFromParam( plugin.GetFloatParameter( Contour::PT_Z_FACTOR ) );
	failures += report( az == 315.0 && alt == 45.0 && z == 1.0, quiet, "controls: default light %.1f at %.1f, Z Factor %.3f (spec: 315 and 45)", az, alt, z );
	return failures;
}

int runDetector( int perturb = 0, bool quiet = false )
{
	using contour::SeaDrive;
	int failures = 0;
	std::vector< float > loud( 64, 0.64f ), silent( 64, 0.0f );
	const int onset = contour::controls::kAudioOnset, level = contour::controls::kAudioLevel;

	{
		SeaDrive d;
		float worst = 0.0f;
		for( int f = 0; f < 120; ++f )
			worst = std::max( worst, d.Update( loud.data(), 64, 0, onset, f / 60.0, false, perturb ) );
		failures += report( worst == 0.0f, quiet, "detector: Onset, loud from the first frame: drive %.3g over two seconds (primed)", static_cast< double >( worst ) );
	}
	{
		//A step from silence saturates the envelope; well after it, when the
		//onset term has died, successive frames fall by e^( -dt / tau_r ):
		//the release is measured from the output, not assumed.
		SeaDrive d;
		double peak = 0.0, v90 = 0.0, v91 = 0.0;
		for( int f = 0; f <= 91; ++f )
		{
			const double v = d.Update( f < 30 ? silent.data() : loud.data(), 64, 0, onset, f / 60.0, false, perturb );
			if( f == 30 )
				peak = v;
			if( f == 90 )
				v90 = v;
			if( f == 91 )
				v91 = v;
		}
		const double lr  = std::log( v91 / v90 );
		const double tau = -( 1.0 / 60.0 ) / lr;
		//The drive is handed out as a float: each value is off by half an
		//ulp, their ratio by one ulp relative, so tau by tau 2^-23 / |ln r|.
		const double tauTol = stated::kOnsetRelease * std::ldexp( 1.0, -23 ) / std::fabs( lr );
		failures += report( peak == 1.0 && std::fabs( tau - stated::kOnsetRelease ) <= tauTol, quiet,
		                    "detector: Onset, a step from silence: saturates to %.3f; a second later it releases with tau %.9f s (stated %.3f)", peak,
		                    tau, stated::kOnsetRelease );
	}
	{
		SeaDrive d;
		double at = 0.0;
		for( int f = 0; f <= 45; ++f )
			at = d.Update( f < 30 ? loud.data() : silent.data(), 64, 0, level, f / 60.0, false, perturb );
		const double x      = std::sqrt( static_cast< double >( 0.64f ) );
		//Frames 30..45 are sixteen silent frames, each releasing by e^( -dt / tau ).
		const double stated = x * std::exp( -( 16.0 / 60.0 ) / stated::kLevelRelease );
		failures += report( std::fabs( at - stated ) <= 1e-6, quiet, "detector: Level, loud then sixteen silent frames: %.6f (stated 0.8 e^-16/15 = %.6f)", at, stated );
	}
	{
		SeaDrive d;
		double worst = 0.0;
		for( int f = 0; f < 60; ++f )
			worst = std::max< double >( worst, d.Update( loud.data(), 64, 0, onset, f < 30 ? 50.0 + f / 60.0 : 2.0 + f / 60.0, f == 30, perturb ) );
		failures += report( worst == 0.0, quiet, "detector: Onset across a jump (re-primed): drive %.3g", worst );
	}
	{
		//A dt of zero is a frame with no release, not a snap.
		SeaDrive d;
		d.Update( loud.data(), 64, 0, level, 1.0, false, perturb );
		double v = 0.0;
		for( int i = 0; i < 5; ++i )
			v = d.Update( silent.data(), 64, 0, level, 1.0, false, perturb );
		const double x = std::sqrt( static_cast< double >( 0.64f ) );
		failures += report( std::fabs( v - x ) <= 1e-7, quiet, "detector: Level, five frames at the same instant after loud: %.7f, unmoved (%.7f)", v, x );
	}
	return failures;
}

int runClock( int perturb = 0, bool quiet = false )
{
	using contour::Clock;
	using contour::SeaDrive;
	int failures = 0;
	Clock seconds, millis;
	seconds.SetScaleForTest( 1.0 );
	millis.SetScaleForTest( 0.001 );
	millis.SetFloatForTest( ( perturb & pb::kClockFloat ) != 0 );
	SeaDrive a, b;
	const double origin = 499000000.0 + 6.0 * 86400.0 * 1000.0;//ms: Resolume's measured clock, plus six days
	double worst        = 0.0;
	std::vector< float > bins( 64 );
	for( int k = 0; k < 3000; ++k )
	{
		const float v = 0.3f + 0.3f * static_cast< float >( std::sin( k * 0.37 ) ) * ( ( k / 23 ) % 2 == 0 ? 1.0f : 0.2f );
		std::fill( bins.begin(), bins.end(), v * v );
		seconds.Update( k / 60.0 );
		millis.Update( origin + k * 1000.0 / 60.0 );
		const double da = a.Update( bins.data(), 64, 0, contour::controls::kAudioOnset, seconds.Now(), seconds.Jumped(), 0 );
		const double db = b.Update( bins.data(), 64, 0, contour::controls::kAudioOnset, millis.Now(), millis.Jumped(), 0 );
		worst           = std::max( worst, std::fabs( da - db ) );
	}
	//A double at 1.02e9 s resolves 1.2e-7 s, so each frame's dt is off by
	//at most 2.4e-7 s, moving each release factor by 2.4e-7 / tau_min =
	//2.4e-6 relative; compounded over the stimulus's longest run (23 frames,
	//and the envelope's releases chain across a few) that stays under 3e-4.
	//A float clock at that origin resolves 64 s.
	failures += report( worst <= 1e-3, quiet, "clock: a six-day millisecond clock and a fresh seconds clock drive the sea alike over 3000 frames: worst %.2e (tol 1e-3)",
	                    worst );
	return failures;
}

int runNames( bool quiet = false )
{
	Contour plugin;
	std::set< std::string > seen;
	int bad = 0;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.name.size() > 16 || !seen.insert( p.name ).second )
			++bad;
	}
	const std::string name = "SW Contour";
	return report( bad == 0 && name.size() <= 16, quiet, "names: %zu parameters, all unique and within 16 characters; the plugin is '%s' (%zu)",
	               seen.size(), name.c_str(), name.size() );
}

//---------------------------------------------------------------------------
// --negative: every check above can fail.
//---------------------------------------------------------------------------
struct NegativeControl
{
	const char* what;
	int failuresSeen;
};

int summariseNegatives( const std::vector< NegativeControl >& controls )
{
	int failures = 0;
	for( const NegativeControl& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		++g_checks;
		std::printf( "negative %-58s %s  %s\n", c.what, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
		{
			++failures;
			++g_failures;
		}
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed model is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

template< typename F >
int caught( F&& check )
{
	const int checks = g_checks, failures = g_failures;
	const int seen   = check();
	g_checks         = checks;
	g_failures       = failures;
	return seen;
}

int runNegativeOffline()
{
	return summariseNegatives( {
		{ "detector: onset baseline starts from silence", caught( [] { return runDetector( pb::kUnprimed, true ); } ) },
		{ "clock: kept in float", caught( [] { return runClock( pb::kClockFloat, true ); } ) },
	} );
}

int runNegative( int width, int height )
{
	return summariseNegatives( {
		{ "count: levels at ( k + 1/2 ) interval", caught( [ & ] { return runCount( width, height, pb::kLevelsHalfStep, true ); } ) },
		{ "count: interval 3% wide", caught( [ & ] { return runCount( width, height, pb::kIntervalDetune, true ); } ) },
		{ "spacing: interval 3% wide", caught( [ & ] { return runSpacing( width, height, pb::kIntervalDetune, true ); } ) },
		{ "spacing: levels at ( k + 1/2 ) interval", caught( [ & ] { return runSpacing( width, height, pb::kLevelsHalfStep, true ); } ) },
		{ "width: no division by |grad h| (the spec's)", caught( [ & ] { return runWidth( width, height, pb::kNoSlopeDivide, true ); } ) },
		{ "hillshade: the light from the south", caught( [ & ] { return runHillshade( width, height, pb::kLightFromSouth, true ); } ) },
		{ "hillshade: Z Factor ignored", caught( [ & ] { return runHillshade( width, height, pb::kNoZFactor, true ); } ) },
		{ "sea: water a quarter interval low", caught( [ & ] { return runSea( width, height, pb::kSeaLow, true ); } ) },
		{ "sea: shoreline half a pixel uphill", caught( [ & ] { return runSea( width, height, pb::kShoreOffset, true ); } ) },
		{ "index: index on k = -1 mod N", caught( [ & ] { return runIndex( width, height, pb::kIndexOffByOne, true ); } ) },
		{ "flat: no guard", caught( [ & ] { return runFlat( width, height, pb::kNoGuard, true ); } ) },
		{ "audio: onset detector unprimed", caught( [ & ] { return runAudio( width, height, pb::kUnprimed, true ); } ) },
		{ "resize: the detector reset, unprimed", caught( [ & ] { return runResize( width, height, pb::kDriveOnResize, true ); } ) },
		{ "resize: the old height buffers kept", caught( [ & ] { return runResize( width, height, pb::kResizeStale, true ); } ) },
	} );
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep, the bench and a default --pipe:
// soft hills drifting over a tilted plane, a noisy patch (the generalising
// question), a hard-edged block, and a colour gradient so every Height
// Source reads different ground. It moves, and it carries a spectrum that
// pulses on the bass every half second, so the sea's audio controls live.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t = static_cast< double >( frame ) / 60.0;
	struct Hill
	{
		double x, y, r, a;
	} const hillsAt[] = {
		{ 0.30 + 0.05 * std::sin( 0.7 * t ), 0.40, 0.22, 0.55 },
		{ 0.68, 0.55 + 0.06 * std::cos( 0.5 * t ), 0.16, 0.45 },
		{ 0.52 + 0.08 * std::cos( 0.9 * t ), 0.78, 0.10, -0.30 },
	};
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			double e = 0.15 + 0.25 * u;
			for( const Hill& h : hillsAt )
			{
				const double dx = ( u - h.x ) * width / height, dy = v - h.y;
				e += h.a * std::exp( -( dx * dx + dy * dy ) / ( 2.0 * h.r * h.r ) );
			}
			if( u > 0.05 && u < 0.25 && v > 0.70 && v < 0.95 )
				e += 0.04 * noiseAt( x, y + static_cast< int >( frame ) );
			if( u > 0.80 && u < 0.92 && v > 0.12 && v < 0.30 )
				e += 0.2;
			e = std::clamp( e, 0.0, 1.0 );
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = static_cast< unsigned char >( std::lround( 255.0 * e ) );
			px[ 1 ] = static_cast< unsigned char >( std::lround( 255.0 * std::clamp( 0.8 * e + 0.2 * v, 0.0, 1.0 ) ) );
			px[ 2 ] = static_cast< unsigned char >( std::lround( 255.0 * std::clamp( 1.0 - e * u, 0.0, 1.0 ) ) );
			px[ 3 ] = 255;
		}
	return img;
}

std::vector< float > cardSpectrum( int64_t frame )
{
	std::vector< float > bins( 64, 0.0f );
	const double phase = static_cast< double >( frame % 30 ) / 60.0;
	const float kick   = static_cast< float >( 0.7 * std::exp( -phase / 0.08 ) );
	for( int i = 0; i < 64; ++i )
		bins[ static_cast< size_t >( i ) ] = i < 8 ? kick : 0.02f + 0.01f * static_cast< float >( ( frame + i ) % 3 );
	return bins;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( session.plugin, setting, error );
	}
	if( !session.begin( width, height ) )
		return -1.0;

	std::vector< std::vector< unsigned char > > loop;
	for( int i = 0; i < 4; ++i )
		loop.push_back( buildCard( width, height, i * 7 ) );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, loop[ static_cast< size_t >( frame ) % loop.size() ] );
	glFinish();

	//Best of three: the GPU is shared with other builds on this machine.
	double best   = 1e9;
	int64_t frame = warmup;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < frames; ++i, ++frame )
			session.renderAt( frame );
		glFinish();
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		best                 = std::min( best, seconds * 1000.0 / frames );
	}
	session.end();
	return best;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720  ", 1280, 720 }, { "1920x1080 ", 1920, 1080 }, { "3840x2160 ", 3840, 2160 } };
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   %% of a 60fps frame   Smooth 1 (sigma 16)\n" );
	std::vector< std::string > heavy = settings;
	heavy.push_back( "Smooth=1" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( settings, size.width, size.height, frames );
		const double hv = benchAt( heavy, size.width, size.height, frames );
		std::printf( "%s    %7.3f        %5.1f%%             %7.3f\n", size.name, ms, ms / 16.667 * 100.0, hv );
	}
	std::printf( "\nThree passes at the defaults (height, two blur axes of radius 6, the map).\n"
	             "Smooth at its maximum makes the blur 97 taps an axis.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace sh = contour::shaders;
	const std::pair< const char*, const char* > files[] = {
		{ "vertex.vert", sh::kVertex },
		{ "height.frag", sh::kHeight },
		{ "blur.frag", sh::kBlur },
		{ "map.frag", sh::kMap },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"cntest -- render and measure the Contour survey map\n"
		"\n"
		"  --out PATH          render the moving terrain card through the plugin (default /tmp/contour.png)\n"
		"  --size WxH          raster (default 1280x720). Not --width: that is a check\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             synthetic frame rate driving the clock (default 60)\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --count             contours crossed along a ramp: an exact integer\n"
		"  --spacing           isolines interval / g apart, whole-pixel and fractional\n"
		"  --width             one pen width on slopes spanning 8:1\n"
		"  --hillshade         planes shade to the closed-form cartographic hillshade\n"
		"  --sea               water exactly below sea level; the shoreline centred on its isoline\n"
		"  --index             every Nth contour, and only those, heavy\n"
		"  --flat              a noisy plateau lying on a level draws nothing (the guard)\n"
		"  --audio             the music raises the sea; the onset detector is primed\n"
		"  --resize            a resize mid-run keeps the drive and leaves no stale buffer\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Controls.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --controls          every control mapping against the README's statement\n"
		"  --detector          the sea's audio detector: primed, its gain and releases, jumps\n"
		"  --clock             a six-day millisecond clock drives the sea as a fresh one does\n"
		"  --names             nothing the host will silently truncate\n"
		"  --offline           all four, and their negative controls; says loudly what it skipped. For CI.\n"
		"  --allow-no-gl       with the rendering checks: SKIP loudly, not FAIL, when no GL 4.1 context exists\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/contour.png";
	std::string scriptPath;
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--count", "--spacing", "--width", "--hillshade", "--sea", "--index",
		                                       "--flat", "--audio", "--resize", "--negative" };
	const std::set< std::string > offline  = { "--controls", "--detector", "--clock", "--names", "--negative-offline" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );//run the checks verbosely against a perturbed model (Controls.h)
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
			for( const char* m : { "--controls", "--detector", "--clock", "--names", "--negative-offline" } )
				checks.push_back( m );
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width < 3 || height < 3 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width and height must be at least 3, frames and fps positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Contour plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		//The checks with no GL first; a context only if a rendering one asks.
		bool needGL     = false;
		bool offlineRan = false;
		for( const std::string& check : checks )
		{
			if( check == "--controls" )
				runControls();
			else if( check == "--detector" )
				runDetector( perturb );
			else if( check == "--clock" )
				runClock( perturb );
			else if( check == "--names" )
				runNames();
			else if( check == "--negative-offline" )
			{
				runNegativeOffline();
				offlineRan = true;
			}
			else
			{
				needGL = true;
				continue;
			}
			std::printf( "\n" );
		}
		if( offlineRan )
			std::printf( "   OFFLINE: --count, --spacing, --width, --hillshade, --sea, --index, --flat, --audio,\n"
			             "   --resize and their negative controls were NOT run. Nothing here drew a pixel\n"
			             "   through a GL driver; the shaders were not exercised, only (in CI) compiled by glslc.\n\n" );

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--count" )
						runCount( width, height, perturb );
					else if( check == "--spacing" )
						runSpacing( width, height, perturb );
					else if( check == "--width" )
						runWidth( width, height, perturb );
					else if( check == "--hillshade" )
						runHillshade( width, height, perturb );
					else if( check == "--sea" )
						runSea( width, height, perturb );
					else if( check == "--index" )
						runIndex( width, height, perturb );
					else if( check == "--flat" )
						runFlat( width, height, perturb );
					else if( check == "--audio" )
						runAudio( width, height, perturb );
					else if( check == "--resize" )
						runResize( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, frames < 40 ? 60 : frames ) );

	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider would.
			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			//Frame n is clocked at n / fps, never the wall clock.
			const bool rendered = index != failRender && session.render( index, frame );
			if( !rendered )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
	{
		session.spectrum( cardSpectrum( frame ) );
		if( !session.render( frame, buildCard( width, height, frame ) ) )
			return finish( 1 );
	}

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
