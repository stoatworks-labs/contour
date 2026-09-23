/**
 * Contour — browser demo.
 *
 * The picture's brightness read as ground height and drawn the way a survey
 * map draws it. The one idea, from `source/Contour.h`: treat luma as
 * elevation; draw isolines at a fixed interval with every Nth heavier; shade
 * the relief from the north-west; tint the elevation bands; flood everything
 * below sea level. Contours crowd on steep ground because their spacing IS
 * interval / |grad h|, and keep one pen width everywhere because each is drawn
 * from its distance on the page, |h - level| / |grad h|.
 *
 * Unlike clamp and standards this plugin really is shaders and arithmetic:
 *
 *   The shaders are the plugin's. `VERTEX`, `HEIGHT`, `BLUR` and `MAP` below
 *   are `kVertex`, `kHeight`, `kBlur` and `kMap` from `source/Shaders.cpp`,
 *   copied across unedited. `demo/tools/check_shaders.py` compares them
 *   character for character and `tools/verify.sh` runs it.
 *
 *   The arithmetic around them is ported: every conversion in `Controls.cpp`,
 *   the Gaussian weight table from `Contour::uploadWeights`, the light vector
 *   and the uniform values from `Contour::ProcessOpenGL`. `SeaDrive` from
 *   `Audio.cpp` is ported too, and here it always reads silence.
 *
 * ------------------------------------------------------- what is missing
 *
 * **Audio.** The sea rises with the audio input in the plugin: Resolume hands
 * it 64 FFT bins through an FF_TYPE_BUFFER parameter, and `SeaDrive` turns them
 * into a drive. A browser page has no Resolume audio, so the drive is 0 on
 * every frame — exactly what the plugin does with no audio routed — and Audio
 * Rise, Audio Mode and Audio Band are present, faithful, and do nothing. The
 * FFT buffer itself is not a control a host draws, so it has no row.
 *
 * **Index Every is a dropdown.** It is FF_TYPE_INTEGER, 2 to 10, and the demo
 * kit has no integer control, so it is a dropdown of its own values, as
 * galvo's integers are.
 *
 * **The About block is absent**, as on every page in this suite.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++ arithmetic, not the plugin. No Resolume, no composition, no
 * FFGL, and GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const HEIGHT = `#version 410 core

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
`;

const BLUR = `#version 410 core

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
`;

const MAP = `#version 410 core

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

	//--- the ground, and its slope in elevation per pixel, north up: a
	//central difference, one-sided on the frame's edge columns and rows (a
	//clamped central difference there would halve the slope and double the
	//width of any line crossing the edge).
	float h   = heightAt( p );
	ivec2 lo  = max( p - ivec2( 1 ), ivec2( 0 ) );
	ivec2 hi  = min( p + ivec2( 1 ), ivec2( HWidth - 1, HHeight - 1 ) );
	vec2 span = vec2( max( hi - lo, ivec2( 1 ) ) );
	vec2 g    = vec2( heightAt( ivec2( hi.x, p.y ) ) - heightAt( ivec2( lo.x, p.y ) ),
	                  heightAt( ivec2( p.x, hi.y ) ) - heightAt( ivec2( p.x, lo.y ) ) ) / span;
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
`;

//===========================================================================
// Controls.cpp, ported. The units: elevation (the channel times Height
// Scale), pixels (Smooth and the three widths), degrees (Azimuth, Altitude).
//===========================================================================

const unit = (v) => Math.min(1, Math.max(0, v));
const optionIndex = (value, count) => Math.min(count - 1, Math.max(0, Math.round(value)));

const heightScaleFromParam = (v) => 2.0 * unit(v);
const smoothSigmaFromParam = (v) => 16.0 * unit(v) * unit(v);
const K_GUARD_SLOPE = 1.0 / (255.0 * 8.0);
const guardSlope = (heightScale, sigma) => (K_GUARD_SLOPE * heightScale) / Math.max(1.0, sigma);
const K_MIN_INTERVAL = 1.0 / 512.0;
const intervalFromParam = (v) => Math.max(K_MIN_INTERVAL, 0.25 * unit(v) * unit(v));
const lineWidthFromParam = (v) => 4.0 * unit(v);
const indexWidthFromParam = (v) => 6.0 * unit(v);
const shoreWidthFromParam = (v) => 6.0 * unit(v);
const azimuthDegreesFromParam = (v) => 360.0 * unit(v);
const altitudeDegreesFromParam = (v) => 90.0 * unit(v);
const zFactorFromParam = (v) => Math.pow(10.0, 2.0 * unit(v) - 1.0);
const seaLevelFromParam = (v, heightScale) => unit(v) * heightScale;
const K_AUDIO_RISE_RANGE = 0.5;
const K_MAX_BLUR_RADIUS = 48;

const SOURCE_COUNT = 5;
const TINT_COUNT = 3;
const AUDIO_ONSET = 1;
const AUDIO_MODE_COUNT = 2;
const BAND_COUNT = 4;
const K_AUDIO_BINS = 64;
const BAND_RANGE = [[0, 63], [0, 7], [8, 27], [28, 63]];

const K_INDEX_EVERY_MIN = 2;
const K_INDEX_EVERY_MAX = 10;

//===========================================================================
// Audio.cpp, ported: the sea's audio drive. On this page `bins` is always
// null — no audio is routed — which the plugin reads as silence: the drive is
// 0 and Audio Rise does nothing. Ported rather than replaced by a constant so
// that the page does what the plugin does, not what it would probably do.
//===========================================================================

class SeaDrive {
  constructor() {
    this.primed = false;
    this.last = 0;
    this.level = 0;
    this.baseline = 0;
    this.envelope = 0;
    this.drive = 0;
  }

  update(bins, binCount, band, mode, now, jumped) {
    const b = Math.min(BAND_COUNT - 1, Math.max(0, band));
    const from = BAND_RANGE[b][0];
    const to = Math.min(BAND_RANGE[b][1], binCount - 1);
    let sum = 0;
    let counted = 0;
    for (let i = from; bins !== null && i <= to; i += 1, counted += 1) sum += Math.sqrt(Math.max(0, bins[i]));
    const x = counted > 0 ? Math.min(1, Math.max(0, sum / counted)) : 0;

    if (!this.primed || jumped) {
      this.level = x;
      this.baseline = x;
      this.envelope = 0;
      this.last = now;
      this.primed = true;
    }

    const dt = Math.max(0, now - this.last);
    this.last = now;

    if (x >= this.level) this.level = x;
    else this.level += (x - this.level) * (1 - Math.exp(-dt / SeaDrive.kLevelRelease));

    const flux = Math.max(0, x - this.baseline);
    this.baseline += (x - this.baseline) * (1 - Math.exp(-dt / SeaDrive.kBaselineTime));
    this.envelope = Math.max(Math.min(1, flux * SeaDrive.kOnsetGain), this.envelope * Math.exp(-dt / SeaDrive.kOnsetRelease));

    this.drive = mode === AUDIO_ONSET ? this.envelope : Math.min(1, Math.max(0, this.level));
    return this.drive;
  }
}
SeaDrive.kLevelRelease = 0.25;
SeaDrive.kBaselineTime = 0.1;
SeaDrive.kOnsetRelease = 0.3;
SeaDrive.kOnsetGain = 4.0;

//===========================================================================
// Clock.cpp, ported, with the unit declared as seconds (SetScaleForTest(1),
// as cntest does). Only the sea's drive reads it.
//===========================================================================

class Clock {
  constructor() {
    this.started = false;
    this.lastScaled = -1;
    this.anchor = 0;
    this.offset = 0;
    this.now = 0;
    this.jumped = false;
  }

  update(scaled) {
    this.jumped = false;
    if (!this.started) {
      this.started = true;
      this.anchor = scaled;
      this.offset = 0;
      this.now = 0;
      this.lastScaled = scaled;
      return;
    }
    const delta = scaled - this.lastScaled;
    if (delta < 0 || delta > Clock.kMaxFrameSeconds) {
      this.jumped = true;
      this.offset = this.now + Clock.kNominalFrameSeconds;
      this.anchor = scaled;
      this.now = this.offset;
    } else {
      this.now = this.offset + (scaled - this.anchor);
    }
    this.lastScaled = scaled;
  }
}
Clock.kMaxFrameSeconds = 0.5;
Clock.kNominalFrameSeconds = 1.0 / 60.0;

//===========================================================================
// The renderer: Contour::ProcessOpenGL, in its order.
//
//   1. height   the picture read as ground, R32F
//   2. blur     a separable Gaussian, across then down, R32F
//   3. map      the survey drawing, onto the canvas
//===========================================================================

function createRenderer(gl, quad) {
  const heightShader = new Program(gl, VERTEX, HEIGHT, 'height');
  const blurShader = new Program(gl, VERTEX, BLUR, 'blur');
  const mapShader = new Program(gl, VERTEX, MAP, 'map');

  // Elevation is differenced across one pixel, so it is held in full float,
  // as the plugin holds it: a half-float store would put 2^-11 steps into the
  // ground, a staircase with a contour on every riser.
  const HEIGHT_FORMAT = gl.R32F;
  const height = new PassBuffer(gl, { filter: 'nearest' });
  const across = new PassBuffer(gl, { filter: 'nearest' });
  const smooth = new PassBuffer(gl, { filter: 'nearest' });

  const weights = new Float32Array(49);
  let radius = 0;
  let weightsSigma = -1;

  const seaDrive = new SeaDrive();
  const clock = new Clock();

  /// Contour::uploadWeights: radius ceil( 3 sigma ), renormalised over what
  /// is kept, so a plateau stays exactly level.
  const uploadWeights = (sigma) => {
    if (sigma === weightsSigma) return;
    weightsSigma = sigma;
    radius = sigma > 0 ? Math.min(K_MAX_BLUR_RADIUS, Math.ceil(3.0 * sigma)) : 0;
    const w = new Float64Array(49);
    let total = 0;
    for (let k = 0; k <= radius; k += 1) {
      w[k] = radius === 0 ? 1.0 : Math.exp((-0.5 * k * k) / (sigma * sigma));
      total += k === 0 ? w[k] : 2.0 * w[k];
    }
    for (let k = 0; k < 49; k += 1) weights[k] = k <= radius ? w[k] / total : 0;
  };

  const blurPass = (from, to, dirX, dirY) => {
    to.bind();
    blurShader.use();
    bindTexture(gl, 0, from.texture);
    blurShader.setSampler('Source', 0);
    blurShader.setInt('Width', from.width);
    blurShader.setInt('Height', from.height);
    blurShader.setInt('DirX', dirX);
    blurShader.setInt('DirY', dirY);
    blurShader.setInt('Radius', radius);
    blurShader.setArray('Weights', weights);
    quad.draw();
    bindTexture(gl, 0, null);
  };

  return {
    render({ input, params, width: vpW, height: vpH, time }) {
      clock.update(time);
      const p = (id) => params.get(id);
      const picture = input;
      const width = picture.width;
      const heightPx = picture.height;
      gl.disable(gl.BLEND);

      // The sea's drive. No audio reaches a browser page: no bins, silence.
      const drive = seaDrive.update(
        null,
        0,
        optionIndex(p('audioBand'), BAND_COUNT),
        optionIndex(p('audioMode'), AUDIO_MODE_COUNT),
        clock.now,
        clock.jumped,
      );

      // Units. Every conversion in double, handed over as float uniforms.
      const scale = heightScaleFromParam(p('heightScale'));
      const sigma = smoothSigmaFromParam(p('smooth'));
      const interval = intervalFromParam(p('interval'));
      const every = Math.min(K_INDEX_EVERY_MAX, Math.max(K_INDEX_EVERY_MIN, indexEveryValue(p('indexEvery'))));
      const az = (azimuthDegreesFromParam(p('azimuth')) * Math.PI) / 180.0;
      const alt = (altitudeDegreesFromParam(p('altitude')) * Math.PI) / 180.0;
      const zf = zFactorFromParam(p('zFactor'));
      const lightNorth = Math.cos(alt) * Math.cos(az);
      const sea = seaLevelFromParam(p('seaLevel'), scale) + p('audioRise') * drive * K_AUDIO_RISE_RANGE * scale;

      uploadWeights(sigma);
      const blurring = radius > 0;

      height.ensure(width, heightPx, HEIGHT_FORMAT);
      if (blurring) {
        across.ensure(width, heightPx, HEIGHT_FORMAT);
        smooth.ensure(width, heightPx, HEIGHT_FORMAT);
      }

      // 1. The picture as ground.
      height.bind();
      heightShader.use();
      bindTexture(gl, 0, picture.texture);
      heightShader.setSampler('InputTexture', 0);
      heightShader.setInt('Source', optionIndex(p('heightSource'), SOURCE_COUNT));
      heightShader.setInt('Invert', p('invert') >= 0.5 ? 1 : 0);
      heightShader.set('Scale', scale);
      quad.draw();
      bindTexture(gl, 0, null);

      // 2. Generalised.
      let ground = height;
      if (blurring) {
        blurPass(height, across, 1, 0);
        blurPass(across, smooth, 0, 1);
        ground = smooth;
      }

      // 3. The map, onto the canvas.
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);

      mapShader.use();
      bindTexture(gl, 0, ground.texture);
      bindTexture(gl, 1, picture.texture);
      mapShader.setSampler('Height', 0);
      mapShader.setSampler('InputTexture', 1);
      mapShader.setInt('HWidth', ground.width);
      mapShader.setInt('HHeight', ground.height);
      mapShader.setInt('VpX', 0);
      mapShader.setInt('VpY', 0);
      mapShader.setInt('VpW', vpW);
      mapShader.setInt('VpH', vpH);

      mapShader.set('Interval', interval);
      mapShader.setInt('IndexEvery', every);
      mapShader.set('HalfLine', 0.5 * lineWidthFromParam(p('lineWidth')));
      mapShader.set('HalfIndex', 0.5 * indexWidthFromParam(p('indexWidth')));
      mapShader.set('HalfShore', 0.5 * shoreWidthFromParam(p('shoreWidth')));
      mapShader.setInt('ContoursOn', p('contoursOn') >= 0.5 ? 1 : 0);
      mapShader.set('LineColour', p('lineR'), p('lineG'), p('lineB'));

      mapShader.set('GuardSlope', guardSlope(Math.max(scale, 1e-6), sigma));
      mapShader.set('RefSlope', interval / 16.0);

      mapShader.set('Light', Math.cos(alt) * Math.sin(az), lightNorth, Math.sin(alt));
      // The surface is lifted by Z Factor times the frame height, so a ramp
      // across the whole frame is the same hill at any raster.
      mapShader.set('ZScale', zf * ground.height);
      mapShader.set('Hillshade', p('hillshade'));

      mapShader.setInt('TintMode', optionIndex(p('tintMode'), TINT_COUNT));
      mapShader.set('TintStrength', p('tintStrength'));
      mapShader.set('TopOfRange', scale);

      mapShader.set('SeaLevel', sea);
      mapShader.set('WaterColour', p('waterR'), p('waterG'), p('waterB'));
      mapShader.set('PaperColour', p('paperR'), p('paperG'), p('paperB'));
      mapShader.set('MixAmount', p('mix'));
      mapShader.setInt('Perturb', 0);
      quad.draw();
      bindTexture(gl, 0, null);
      bindTexture(gl, 1, null);
      gl.activeTexture(gl.TEXTURE0);
    },
  };
}

//===========================================================================
// The controls, read out of Contour::Contour(). Same names, same groups, same
// order, same defaults, same dropdown elements. Absent: the About block, and
// the Audio FFT buffer, which is not a control a host draws.
//===========================================================================

/// Index Every is FF_TYPE_INTEGER, 2..10, default 5: exempt from the 0..1
/// clamp, so the plugin stores the integer itself. The kit has no integer
/// control, so it is a dropdown of its own values and the index converts.
const INDEX_EVERY_ELEMENTS = [];
for (let v = K_INDEX_EVERY_MIN; v <= K_INDEX_EVERY_MAX; v += 1) INDEX_EVERY_ELEMENTS.push(String(v));
function indexEveryValue(index) {
  return K_INDEX_EVERY_MIN + optionIndex(index, INDEX_EVERY_ELEMENTS.length);
}
const indexEveryIndex = (value) => value - K_INDEX_EVERY_MIN;

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });
const colour = (id, name, def, group, hint) => ({ id, name, type: 'colour', default: def, group, hint });

const px = (v) => `${v.toFixed(2)} px`;
const NO_AUDIO = ' There is no audio in the browser, so on this page it does nothing.';

mountDemo({
  name: 'Contour',
  pluginId: 'CN01',
  tagline:
    'The picture’s brightness read as ground height and drawn as a survey map: contour lines at a fixed interval with every Nth heavier, hill-shading from the north-west, hypsometric tints, and a sea below a chosen level. The contours crowd on steep ground and open out on gentle ground, because their spacing is the slope; each keeps one pen width everywhere, because it is drawn from its distance on the page. Noise makes an unreadable map until Smooth generalises the height.',
  repo: 'https://github.com/stoatworks-labs/contour',

  // The stock wording would say "same parameters, same maths" with nothing
  // about the sea, which in the plugin is raised by the music.
  blurb:
    'It is Contour’s own GLSL, ported from the repository to WebGL2 and running on generated clips in this page — same parameters, same maths, no install. The plugin raises its sea with Resolume’s audio input; a browser page has none, so here the sea stays at Sea Level.',

  // The ground is R32F, as in the plugin; rendering to it needs a float target.
  needFloat: true,

  params: [
    opt('heightSource', 'Height Source', ['Luma', 'Red', 'Green', 'Blue', 'Alpha'], 0, 'Terrain',
      'Which channel is read as ground height. Luma is Rec. 709.'),
    std('heightScale', 'Height Scale', 0.5, 'Terrain', {
      display: (v) => `×${heightScaleFromParam(v).toFixed(2)}`,
      hint: 'Elevation per unit of the source channel, 0 to 2. Interval and Sea Level are measured in elevation, so this changes how many contours the picture reaches.',
    }),
    bool('invert', 'Invert', 0, 'Terrain', 'Dark is high ground instead of low.'),
    std('smooth', 'Smooth', 0.5, 'Terrain', {
      display: (v) => `σ ${px(smoothSigmaFromParam(v))}`,
      hint: 'Generalises the height with a Gaussian of this sigma before anything is drawn. Noise makes an unreadable map; an 8-bit gentle slope reads as terraces through the hillshade below about 4 px.',
    }),

    std('interval', 'Interval', 0.45, 'Contours', {
      display: (v) => intervalFromParam(v).toFixed(4),
      hint: 'Elevation between contours. Their spacing on the page is interval / slope, so they crowd on steep ground.',
    }),
    opt('indexEvery', 'Index Every', INDEX_EVERY_ELEMENTS, indexEveryIndex(5), 'Contours',
      'Every Nth contour is an index contour, drawn heavier. An integer in the plugin, 2 to 10.'),
    std('lineWidth', 'Line Width', 0.3, 'Contours', {
      display: (v) => px(lineWidthFromParam(v)),
      hint: 'The ordinary contour’s full stroke width, in pixels on the page — kept on a cliff and on a plain alike.',
    }),
    std('indexWidth', 'Index Width', 0.4, 'Contours', {
      display: (v) => px(indexWidthFromParam(v)),
      hint: 'The index contour’s full stroke width, in pixels.',
    }),
    colour('lineR', 'Line Colour', 0.45, 'Contours'),
    colour('lineG', 'Line_Green', 0.28, 'Contours'),
    colour('lineB', 'Line_Blue', 0.15, 'Contours'),
    bool('contoursOn', 'Contours On', 1, 'Contours'),

    std('hillshade', 'Hillshade', 0.6, 'Relief', {
      display: (v) => `${Math.round(v * 100)}%`,
      hint: 'Lambert shading of the surface from the light below.',
    }),
    std('azimuth', 'Azimuth', 0.875, 'Relief', {
      display: (v) => `${azimuthDegreesFromParam(v).toFixed(0)}°`,
      hint: 'Where the light comes from, clockwise from north (up). 315° is the cartographer’s north-west.',
    }),
    std('altitude', 'Altitude', 0.5, 'Relief', {
      display: (v) => `${altitudeDegreesFromParam(v).toFixed(1)}°`,
      hint: 'The light’s height above the horizon.',
    }),
    std('zFactor', 'Z Factor', 0.5, 'Relief', {
      display: (v) => `×${zFactorFromParam(v).toFixed(2)}`,
      hint: 'Vertical exaggeration for the hillshade, 0.1 to 10, relative to the frame height so a ramp is the same hill at any raster.',
    }),

    opt('tintMode', 'Tint Mode', ['Classic', 'Mono', 'Off'], 0, 'Tints',
      'Hypsometric tints by elevation band: a survey sheet’s greens to browns to white, a grey ramp, or plain paper.'),
    std('tintStrength', 'Tint Strength', 0.6, 'Tints'),

    std('seaLevel', 'Sea Level', 0.05, 'Sea', {
      display: (v) => `${(unit(v) * 100).toFixed(0)}% of the range`,
      hint: 'Everything below this is sea, as a fraction of the range the picture can reach. The shoreline is the isoline at sea level.',
    }),
    std('audioRise', 'Audio Rise', 0.5, 'Sea', {
      display: (v) => `up to ${(unit(v) * K_AUDIO_RISE_RANGE * 100).toFixed(0)}%`,
      hint: `How far the audio drive raises the sea, up to half the height range.${NO_AUDIO}`,
    }),
    colour('waterR', 'Water Colour', 0.55, 'Sea'),
    colour('waterG', 'Water_Green', 0.74, 'Sea'),
    colour('waterB', 'Water_Blue', 0.88, 'Sea'),
    std('shoreWidth', 'Shore Width', 0.25, 'Sea', {
      display: (v) => px(shoreWidthFromParam(v)),
      hint: 'The shoreline stroke’s full width, in pixels.',
    }),

    opt('audioMode', 'Audio Mode', ['Level', 'Onset'], 1, 'Audio',
      `Level: the sea breathes with the band’s level. Onset: it surges on a hit and drains after it.${NO_AUDIO}`),
    opt('audioBand', 'Audio Band', ['Full Range', 'Bass', 'Mid', 'Treble'], 1, 'Audio',
      `Which part of the spectrum drives the sea.${NO_AUDIO}`),

    colour('paperR', 'Paper Colour', 0.96, 'Output'),
    colour('paperG', 'Paper_Green', 0.94, 'Output'),
    colour('paperB', 'Paper_Blue', 0.88, 'Output'),
    std('mix', 'Mix', 1.0, 'Output'),
  ],

  // Big, soft shapes make the best map. The scene's hills and sky, the ramps'
  // even slopes (evenly spaced contours), the lights' round hills on a plain.
  sources: ['scene', 'ramp', 'spot', 'alpha', 'grid', 'bars', 'detail'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'Contours only (no tints, no relief)': { tintMode: 2, hillshade: 0 },
    'Dense contours': { interval: 0.28, indexEvery: indexEveryIndex(4) },
    'Raw ground (no smoothing)': { smooth: 0 },
    'Deep relief': { zFactor: 0.8, hillshade: 1, altitude: 0.35 },
    'High water': { seaLevel: 0.35 },
    'Mono sheet': { tintMode: 1, lineR: 0.15, lineG: 0.15, lineB: 0.15 },
    'Inverted: dark is high': { invert: 1 },
    'Map over the clip': { mix: 0.6 },
  },

  differences: [
    'No audio reaches a browser page. In the plugin the sea rises with Resolume’s audio input — 64 FFT bins through an FF_TYPE_BUFFER parameter, turned into a level or an onset envelope. Here no bins arrive, which the ported detector reads exactly as the plugin reads an unrouted input: silence, a drive of 0, and a sea that stays at Sea Level. Audio Rise, Audio Mode and Audio Band are the plugin’s own controls and are present, but on this page they do nothing. The FFT buffer is not a control a host draws, so it has no row here.',
    'Index Every is FF_TYPE_INTEGER in the plugin, 2 to 10. The demo kit has no integer control, so it is a dropdown of the same nine values.',
    'The arithmetic around the shaders is a port: every conversion in Controls.cpp, the Gaussian weight table and the light vector from Contour.cpp, and the audio detector from Audio.cpp. Nothing checks a port but a reader. The shaders are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the four drifts. The ground is R32F, as in the plugin.',
    'The plugin’s numerical proof — contour widths exact to their pen width on a cliff and on a plain, spacing inversely proportional to slope, the hillshade against Lambert, the flat-ground guard, the detector’s priming — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});
