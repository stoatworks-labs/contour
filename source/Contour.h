#pragma once

#include "Audio.h"
#include "Clock.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include <string>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Contour -- the picture's brightness read as ground height and drawn the
	way a survey map draws it, as an FFGL effect.

	**The one idea.** Treat luma as elevation. Draw isolines at a fixed
	interval with every Nth heavier, shade the relief from the north-west,
	tint the elevation bands, and flood everything below sea level -- a sea
	the music can raise. Nothing is drawn that the cartographer's mechanism
	does not produce: contours crowd on steep ground because their spacing IS
	interval / |grad h|; they keep one pen width everywhere because each is
	drawn from its distance on the page, |h - level| / |grad h|; noise makes an
	unreadable map until the height is generalised; the shoreline is only the
	isoline at sea level.

	**Three passes** (`Shaders.h`): the picture into an elevation buffer, a
	separable Gaussian to generalise it, and the map. The map is a pure
	function of the frame. The only state across frames is the sea's audio
	drive (`Audio.h`), on the CPU. See AGENTS.md for the guard, the traps and
	what is verified.
*/
class Contour : public CFFGLPlugin
{
public:
	Contour();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by cntest; the plugin's own operation never uses
	//--- them, and the perturbation is always 0 outside the harness.

	/// The harness DECLARES its clock unit rather than leaving the voting to
	/// infer one: it renders as fast as the GPU allows.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

	/// Negative-control hooks, a bitmask of `contour::perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}

	/// The audio drive the last frame used, 0..1.
	float DriveForTest() const
	{
		return seaDrive.Drive();
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	/// SetParamGroup collapses consecutive ids, so each group is a contiguous
	/// run, and the order is load-bearing: append only.
	enum ParamID : FFUInt32
	{
		//Terrain
		PT_HEIGHT_SOURCE,
		PT_HEIGHT_SCALE,
		PT_INVERT,
		PT_SMOOTH,

		//Contours
		PT_INTERVAL,
		PT_INDEX_EVERY,
		PT_LINE_WIDTH,
		PT_INDEX_WIDTH,
		PT_LINE_R,
		PT_LINE_G,
		PT_LINE_B,
		PT_CONTOURS_ON,

		//Relief
		PT_HILLSHADE,
		PT_AZIMUTH,
		PT_ALTITUDE,
		PT_Z_FACTOR,

		//Tints
		PT_TINT_MODE,
		PT_TINT_STRENGTH,

		//Sea
		PT_SEA_LEVEL,
		PT_AUDIO_RISE,
		PT_WATER_R,
		PT_WATER_G,
		PT_WATER_B,
		PT_SHORE_WIDTH,

		//Audio
		PT_AUDIO_FFT,
		PT_AUDIO_MODE,
		PT_AUDIO_BAND,

		//Output
		PT_PAPER_R,
		PT_PAPER_G,
		PT_PAPER_B,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// Index Every's real integer range.
	static constexpr int kIndexEveryMin = 2;
	static constexpr int kIndexEveryMax = 10;

private:
	bool ensureBuffers( int width, int height, bool blurring );
	void heightPass( const FFGLTextureStruct& picture );
	void blurPass( contour::PassBuffer& from, contour::PassBuffer& to, int dirX, int dirY );
	void uploadWeights( double sigma );

	ffglex::FFGLShader heightShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader mapShader;
	ffglex::FFGLScreenQuad quad;

	contour::PassBuffer height;///< the ground, before generalising
	contour::PassBuffer across;///< after the horizontal pass
	contour::PassBuffer smooth;///< after both

	/// Weights[ k ] at distance k, normalised over -radius..radius.
	float weights[ 49 ] = {};
	int radius          = 0;
	double weightsSigma = -1.0;

	contour::SeaDrive seaDrive;
	contour::Clock clock;
	double hostTime = -1.0;
	int clockFrames = 0;

	int perturb = 0;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
