#include "Contour.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace contour;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Contour >,// Create method
	"CN01",                  // Plugin unique ID of maximum length 4.
	"SW Contour",            // Plugin name
	2,                       // API major version number
	1,                       // API minor version number
	0,                       // Plugin major version number
	1,                       // Plugin minor version number
	FF_EFFECT,               // Plugin type
	"Reads the picture's brightness as ground height and draws it as a survey map: contour lines at a fixed interval with every Nth heavier, hill-shading from the north-west, hypsometric tints, and a sea below a chosen level.\n\nThe contours crowd on steep ground and open out on gentle ground, because their spacing is the slope; each keeps one pen width everywhere, because it is drawn from its distance on the page. Noise makes an unreadable map until Smooth generalises the height. The shoreline is the isoline at sea level, and the audio input raises the sea.\n\nStart with something that has big, soft shapes in it.",// Plugin description
	"Contour FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kSourceNames[]    = { "Luma", "Red", "Green", "Blue", "Alpha" };
const char* const kTintNames[]      = { "Classic", "Mono", "Off" };
const char* const kAudioModeNames[] = { "Level", "Onset" };
const char* const kBandNames[]      = { "Full Range", "Bass", "Mid", "Treble" };

/// Elevation is a continuous quantity differenced across one pixel, so it
/// is held in full float: a half-float store would put 2^-11 steps into the
/// ground, and on a gentle slope that is a staircase with a contour on
/// every riser.
constexpr GLint kHeightFormat = GL_R32F;

constexpr double kPi = 3.14159265358979323846;
} // namespace

//---------------------------------------------------------------------------
Contour::Contour()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Only the audio drive reads the clock; it needs the host's time so an
	//export's release matches the preview's.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults: a USGS-looking sheet. Brown contours every 1/20 of the
	// range with every fifth heavier, a half-strength hillshade from the
	// north-west at 45 degrees, classic tints over cream paper, and a sea
	// over the lowest fifth that surges on the bass.
	//---------------------------------------------------------------------
	params[ PT_HEIGHT_SOURCE ] = static_cast< float >( controls::kSourceLuma );
	params[ PT_HEIGHT_SCALE ]  = 0.5f; //1
	params[ PT_INVERT ]        = 0.0f;
	params[ PT_SMOOTH ]        = 0.5f; //sigma 4 px: an 8-bit gentle slope reads as terraces through the hillshade below that

	params[ PT_INTERVAL ]    = 0.45f;//0.0506
	params[ PT_INDEX_EVERY ] = 5.0f;
	params[ PT_LINE_WIDTH ]  = 0.3f; //1.2 px
	params[ PT_INDEX_WIDTH ] = 0.4f; //2.4 px
	params[ PT_LINE_R ]      = 0.45f;
	params[ PT_LINE_G ]      = 0.28f;
	params[ PT_LINE_B ]      = 0.15f;
	params[ PT_CONTOURS_ON ] = 1.0f;

	params[ PT_HILLSHADE ] = 0.6f;
	params[ PT_AZIMUTH ]   = 0.875f;//315 degrees
	params[ PT_ALTITUDE ]  = 0.5f;  //45 degrees
	params[ PT_Z_FACTOR ]  = 0.5f;  //1

	params[ PT_TINT_MODE ]     = static_cast< float >( controls::kTintClassic );
	params[ PT_TINT_STRENGTH ] = 0.6f;

	params[ PT_SEA_LEVEL ]   = 0.2f;
	params[ PT_AUDIO_RISE ]  = 0.5f;
	params[ PT_WATER_R ]     = 0.55f;
	params[ PT_WATER_G ]     = 0.74f;
	params[ PT_WATER_B ]     = 0.88f;
	params[ PT_SHORE_WIDTH ] = 0.25f;//1.5 px

	params[ PT_AUDIO_MODE ] = static_cast< float >( controls::kAudioOnset );
	params[ PT_AUDIO_BAND ] = static_cast< float >( controls::kBandBass );

	params[ PT_PAPER_R ] = 0.96f;
	params[ PT_PAPER_G ] = 0.94f;
	params[ PT_PAPER_B ] = 0.88f;
	params[ PT_MIX ]     = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Option lists are in their natural order: Height Source
	// is luma then the channels in RGBA order, Tint Mode strongest first,
	// Audio Band low to high.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, const char* const* names, int count ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};

	declareOptions( PT_HEIGHT_SOURCE, "Height Source", kSourceNames, controls::kSourceCount );
	SetParamInfof( PT_HEIGHT_SCALE, "Height Scale", FF_TYPE_STANDARD );
	SetParamInfo( PT_INVERT, "Invert", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_SMOOTH, "Smooth", FF_TYPE_STANDARD );

	SetParamInfof( PT_INTERVAL, "Interval", FF_TYPE_STANDARD );
	//Only FF_TYPE_STANDARD has its default clamped into 0..1, so a real
	//integer can be declared with its real default and range.
	SetParamInfo( PT_INDEX_EVERY, "Index Every", FF_TYPE_INTEGER, params[ PT_INDEX_EVERY ] );
	SetParamRange( PT_INDEX_EVERY, static_cast< float >( kIndexEveryMin ), static_cast< float >( kIndexEveryMax ) );
	SetParamInfof( PT_LINE_WIDTH, "Line Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_INDEX_WIDTH, "Index Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_LINE_R, "Line Colour", FF_TYPE_RED );
	SetParamInfof( PT_LINE_G, "Line_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_LINE_B, "Line_Blue", FF_TYPE_BLUE );
	SetParamInfo( PT_CONTOURS_ON, "Contours On", FF_TYPE_BOOLEAN, true );

	SetParamInfof( PT_HILLSHADE, "Hillshade", FF_TYPE_STANDARD );
	SetParamInfof( PT_AZIMUTH, "Azimuth", FF_TYPE_STANDARD );
	SetParamInfof( PT_ALTITUDE, "Altitude", FF_TYPE_STANDARD );
	SetParamInfof( PT_Z_FACTOR, "Z Factor", FF_TYPE_STANDARD );

	declareOptions( PT_TINT_MODE, "Tint Mode", kTintNames, controls::kTintCount );
	SetParamInfof( PT_TINT_STRENGTH, "Tint Strength", FF_TYPE_STANDARD );

	SetParamInfof( PT_SEA_LEVEL, "Sea Level", FF_TYPE_STANDARD );
	SetParamInfof( PT_AUDIO_RISE, "Audio Rise", FF_TYPE_STANDARD );
	SetParamInfof( PT_WATER_R, "Water Colour", FF_TYPE_RED );
	SetParamInfof( PT_WATER_G, "Water_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_WATER_B, "Water_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_SHORE_WIDTH, "Shore Width", FF_TYPE_STANDARD );

	// The spectrum. Declared with a real element list so the host knows how
	// many bins to fill. With no audio routed every bin stays 0, the drive is
	// 0 and Audio Rise does nothing -- no phantom tide.
	SetBufferParamInfo( PT_AUDIO_FFT, "Audio", controls::kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < controls::kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO_FFT, static_cast< unsigned int >( i ), "", 0.0f );
	declareOptions( PT_AUDIO_MODE, "Audio Mode", kAudioModeNames, controls::kAudioModeCount );
	declareOptions( PT_AUDIO_BAND, "Audio Band", kBandNames, controls::kBandCount );

	SetParamInfof( PT_PAPER_R, "Paper Colour", FF_TYPE_RED );
	SetParamInfof( PT_PAPER_G, "Paper_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_PAPER_B, "Paper_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	struct Group
	{
		FFUInt32 first, last;
		const char* name;
	} const groups[] = {
		{ PT_HEIGHT_SOURCE, PT_SMOOTH, "Terrain" }, { PT_INTERVAL, PT_CONTOURS_ON, "Contours" },
		{ PT_HILLSHADE, PT_Z_FACTOR, "Relief" },    { PT_TINT_MODE, PT_TINT_STRENGTH, "Tints" },
		{ PT_SEA_LEVEL, PT_SHORE_WIDTH, "Sea" },    { PT_AUDIO_FFT, PT_AUDIO_BAND, "Audio" },
		{ PT_PAPER_R, PT_MIX, "Output" },
	};
	for( const Group& g : groups )
		for( FFUInt32 i = g.first; i <= g.last; ++i )
			SetParamGroup( i, g.name );

	// The About block. Declared inline: SetParamInfo is protected on
	// CFFGLPlugin and nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Contour effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Contour::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &heightShader, shaders::kHeight, "height" },
		{ &blurShader, shaders::kBlur, "blur" },
		{ &mapShader, shaders::kMap, "map" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertex, stage.fragment ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Contour: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	clock.Reset();
	seaDrive.Reset();
	weightsSigma = -1.0;

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool Contour::ensureBuffers( int width, int heightPx, bool blurring )
{
	if( !height.Ensure( width, heightPx, kHeightFormat, PassBuffer::Sampling::Nearest ) )
		return false;
	if( !blurring )
		return true;
	return across.Ensure( width, heightPx, kHeightFormat, PassBuffer::Sampling::Nearest )
	       && smooth.Ensure( width, heightPx, kHeightFormat, PassBuffer::Sampling::Nearest );
}

void Contour::uploadWeights( double sigma )
{
	if( sigma == weightsSigma )
		return;
	weightsSigma = sigma;

	//Radius ceil( 3 sigma ): the tail past it is 0.3% of the mass, and the
	//weights are renormalised over what is kept, so a plateau stays exactly
	//level -- a blur that lost mass at the edge of its kernel would tilt
	//flat ground and draw contours across it.
	radius = sigma > 0.0 ? std::min( controls::kMaxBlurRadius, static_cast< int >( std::ceil( 3.0 * sigma ) ) ) : 0;
	double w[ 49 ] = {};
	double total   = 0.0;
	for( int k = 0; k <= radius; ++k )
	{
		w[ k ] = radius == 0 ? 1.0 : std::exp( -0.5 * k * k / ( sigma * sigma ) );
		total += k == 0 ? w[ k ] : 2.0 * w[ k ];
	}
	for( int k = 0; k < 49; ++k )
		weights[ k ] = k <= radius ? static_cast< float >( w[ k ] / total ) : 0.0f;
}

void Contour::heightPass( const FFGLTextureStruct& picture )
{
	ScopedFBOBinding fbo( height.GetGLID(), ScopedFBOBinding::RB_REVERT );
	height.ResizeViewPort();
	ScopedShaderBinding shader( heightShader.GetGLID() );
	ScopedSamplerActivation sampler( 0 );
	Scoped2DTextureBinding texture( picture.Handle );
	heightShader.Set( "InputTexture", 0 );
	heightShader.Set( "Source", controls::OptionIndex( params[ PT_HEIGHT_SOURCE ], controls::kSourceCount ) );
	heightShader.Set( "Invert", params[ PT_INVERT ] >= 0.5f ? 1 : 0 );
	heightShader.Set( "Scale", static_cast< float >( controls::HeightScaleFromParam( params[ PT_HEIGHT_SCALE ] ) ) );
	quad.Draw();
}

void Contour::blurPass( PassBuffer& from, PassBuffer& to, int dirX, int dirY )
{
	ScopedFBOBinding fbo( to.GetGLID(), ScopedFBOBinding::RB_REVERT );
	to.ResizeViewPort();
	ScopedShaderBinding shader( blurShader.GetGLID() );
	ScopedSamplerActivation sampler( 0 );
	Scoped2DTextureBinding texture( from.TextureID() );
	blurShader.Set( "Source", 0 );
	blurShader.Set( "Width", static_cast< int >( from.GetWidth() ) );
	blurShader.Set( "Height", static_cast< int >( from.GetHeight() ) );
	blurShader.Set( "DirX", dirX );
	blurShader.Set( "DirY", dirY );
	blurShader.Set( "Radius", radius );
	//FFGLShader::Set has no array overload.
	glUniform1fv( glGetUniformLocation( blurShader.GetGLID(), "Weights" ), 49, weights );
	quad.Draw();
}

//---------------------------------------------------------------------------
FFResult Contour::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	clock.SetFloatForTest( ( perturb & perturb::kClockFloat ) != 0 );
	clock.Update( hostTime );
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale="
		            + std::to_string( clock.ClockScale() ) + " seconds=" + std::to_string( clock.Now() ) );

	const int width    = static_cast< int >( picture.Width );
	const int heightPx = static_cast< int >( picture.Height );
	const bool resized = height.IsValid() && ( static_cast< int >( height.GetWidth() ) != width
	                                           || static_cast< int >( height.GetHeight() ) != heightPx );

	//---------------------------------------------------------------------
	// The sea's drive. CPU state, carried across a resize untouched.
	//---------------------------------------------------------------------
	int drivePerturb = perturb;
	if( resized && ( perturb & perturb::kDriveOnResize ) )
	{
		seaDrive.Reset();
		drivePerturb |= perturb::kUnprimed;
	}
	float bins[ controls::kAudioBins ] = {};
	int binCount                       = 0;
	if( const ParamInfo* info = FindParamInfo( PT_AUDIO_FFT ) )
	{
		binCount = static_cast< int >( std::min< size_t >( info->elements.size(), controls::kAudioBins ) );
		for( int i = 0; i < binCount; ++i )
			bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
	}
	const float drive = seaDrive.Update( bins, binCount, controls::OptionIndex( params[ PT_AUDIO_BAND ], controls::kBandCount ),
	                                     controls::OptionIndex( params[ PT_AUDIO_MODE ], controls::kAudioModeCount ), clock.Now(),
	                                     clock.Jumped(), drivePerturb );

	//---------------------------------------------------------------------
	// Units. Every conversion in double, handed over as float uniforms.
	//---------------------------------------------------------------------
	const double scale    = controls::HeightScaleFromParam( params[ PT_HEIGHT_SCALE ] );
	const double sigma    = controls::SmoothSigmaFromParam( params[ PT_SMOOTH ] );
	const double interval = controls::IntervalFromParam( params[ PT_INTERVAL ] );
	const int every = std::clamp( static_cast< int >( std::lround( params[ PT_INDEX_EVERY ] ) ), kIndexEveryMin, kIndexEveryMax );
	const double az  = controls::AzimuthDegreesFromParam( params[ PT_AZIMUTH ] ) * kPi / 180.0;
	const double alt = controls::AltitudeDegreesFromParam( params[ PT_ALTITUDE ] ) * kPi / 180.0;
	const double zf  = ( perturb & perturb::kNoZFactor ) ? 1.0 : controls::ZFactorFromParam( params[ PT_Z_FACTOR ] );
	const double lightNorth = ( perturb & perturb::kLightFromSouth ) ? -std::cos( alt ) * std::cos( az ) : std::cos( alt ) * std::cos( az );
	const double sea = controls::SeaLevelFromParam( params[ PT_SEA_LEVEL ], scale )
	                   + static_cast< double >( params[ PT_AUDIO_RISE ] ) * drive * controls::kAudioRiseRange * scale;

	uploadWeights( sigma );
	const bool blurring = radius > 0;

	//---------------------------------------------------------------------
	// Buffers. Every allocation happens here, before anything binds a
	// texture: FFGLFBO::Initialise sizes its colour texture under a scoped
	// binding, and every ffglex Scoped* binding CLEARS to 0 on exit. Nothing
	// in them outlives a frame, so a resize simply reallocates.
	//---------------------------------------------------------------------
	if( !( resized && ( perturb & perturb::kResizeStale ) ) && !ensureBuffers( width, heightPx, blurring ) )
	{
		diag::error( "could not allocate the height buffers at " + std::to_string( width ) + "x" + std::to_string( heightPx ) );
		return FF_FAIL;
	}
	if( blurring && !smooth.IsValid() && !ensureBuffers( width, heightPx, true ) )
		return FF_FAIL;

	heightPass( picture );
	PassBuffer* ground = &height;
	if( blurring )
	{
		blurPass( height, across, 1, 0 );
		blurPass( across, smooth, 0, 1 );
		ground = &smooth;
	}

	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( mapShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding groundTexture( ground->TextureID() );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding input( picture.Handle );

		const int hw = static_cast< int >( ground->GetWidth() );
		const int hh = static_cast< int >( ground->GetHeight() );
		mapShader.Set( "Height", 0 );
		mapShader.Set( "InputTexture", 1 );
		mapShader.Set( "HWidth", hw );
		mapShader.Set( "HHeight", hh );
		mapShader.Set( "VpX", hostViewport[ 0 ] );
		mapShader.Set( "VpY", hostViewport[ 1 ] );
		mapShader.Set( "VpW", hostViewport[ 2 ] );
		mapShader.Set( "VpH", hostViewport[ 3 ] );

		mapShader.Set( "Interval", static_cast< float >( interval ) );
		mapShader.Set( "IndexEvery", every );
		mapShader.Set( "HalfLine", static_cast< float >( 0.5 * controls::LineWidthFromParam( params[ PT_LINE_WIDTH ] ) ) );
		mapShader.Set( "HalfIndex", static_cast< float >( 0.5 * controls::IndexWidthFromParam( params[ PT_INDEX_WIDTH ] ) ) );
		mapShader.Set( "HalfShore", static_cast< float >( 0.5 * controls::ShoreWidthFromParam( params[ PT_SHORE_WIDTH ] ) ) );
		mapShader.Set( "ContoursOn", params[ PT_CONTOURS_ON ] >= 0.5f ? 1 : 0 );
		mapShader.Set( "LineColour", params[ PT_LINE_R ], params[ PT_LINE_G ], params[ PT_LINE_B ] );

		mapShader.Set( "GuardSlope", static_cast< float >( controls::GuardSlope( std::max( scale, 1e-6 ), sigma ) ) );
		mapShader.Set( "RefSlope", static_cast< float >( interval / 16.0 ) );

		mapShader.Set( "Light", static_cast< float >( std::cos( alt ) * std::sin( az ) ), static_cast< float >( lightNorth ),
		               static_cast< float >( std::sin( alt ) ) );
		//The surface is lifted by Z Factor times the frame height, so a ramp
		//across the whole frame is the same hill at any raster.
		mapShader.Set( "ZScale", static_cast< float >( zf * hh ) );
		mapShader.Set( "Hillshade", params[ PT_HILLSHADE ] );

		mapShader.Set( "TintMode", controls::OptionIndex( params[ PT_TINT_MODE ], controls::kTintCount ) );
		mapShader.Set( "TintStrength", params[ PT_TINT_STRENGTH ] );
		mapShader.Set( "TopOfRange", static_cast< float >( scale ) );

		mapShader.Set( "SeaLevel", static_cast< float >( sea ) );
		mapShader.Set( "WaterColour", params[ PT_WATER_R ], params[ PT_WATER_G ], params[ PT_WATER_B ] );
		mapShader.Set( "PaperColour", params[ PT_PAPER_R ], params[ PT_PAPER_G ], params[ PT_PAPER_B ] );
		mapShader.Set( "MixAmount", params[ PT_MIX ] );
		mapShader.Set( "Perturb", perturb );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Contour::DeInitGL()
{
	heightShader.FreeGLResources();
	blurShader.FreeGLResources();
	mapShader.FreeGLResources();
	quad.Release();

	height.Destroy();
	across.Destroy();
	smooth.Destroy();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Contour::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Contour::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Contour::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Contour::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Contour::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}
