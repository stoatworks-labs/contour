#pragma once

/**
	The passes. Every read is `texelFetch` at an integer coordinate computed
	from `gl_FragCoord`, so nothing here depends on where a rasteriser's
	interpolated uv lands or on a texture unit's filtering precision: the only
	floating-point work a driver can do differently is the arithmetic itself,
	which GLSL 4.10 section 8.2 bounds.

	  height    the host picture -> elevation (R32F, the picture's size):
	            the chosen channel, inverted or not, times Height Scale
	  blur      elevation -> elevation, one axis of a separable Gaussian whose
	            weights the CPU computed in double. Run twice, or skipped at
	            Smooth 0 (the map then reads the height buffer directly)
	  map       elevation -> the host's framebuffer: gradient, contours,
	            hillshade, tints, sea and shoreline, paper, mix

	Rows stay in GL's order throughout (row 0 is the bottom of the picture),
	so north -- up on the page -- is +y everywhere, the light's direction
	included. Only the harness, which builds its pictures top first, flips.
*/
namespace contour::shaders
{

extern const char* const kVertex;
extern const char* const kHeight;
extern const char* const kBlur;
extern const char* const kMap;

} // namespace contour::shaders
