# Attributions

Contour is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

It is PROVISIONAL: hand-written in the shape the `stoatworks-backend` sync
(`scripts/sync-attributions.py`) generates. Contour is not yet registered in that
script's lists, so the sync cannot produce this file yet. Once the registration is
finished the sync overwrites this file; edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives through the vcpkg manifest on Windows only. Not fetched on macOS.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls — listed because it is present in the checkout. The harness writes PNGs through the system zlib and not through this.

## Within the fleet

Not third-party, but owed a line. `PassBuffer` is **tinsel**'s, by way of **rebate**
and **standards**. The harness shape, the `--pipe` contract, `--offline`,
`check-shaders.sh`, the host clock (readout's unit voting, kept in double) and the
negative-control pattern come from **standards**, **pitch** and **slowscan**. The
audio declaration and the three-way band split over 64 bins are **regauss**'s. The
lesson that a line drawn from a field must be divided by the field's gradient to keep
one width on the page is **tinsel**'s ("a lamp measured in strip units is a streak")
and **intaglio**'s, applied here to isolines. The PCG output mix the harness uses for
its noise is the well-known `pcg_hash` construction, written out here rather than
copied from anyone's source.

## Science, not code

The drawing is the cartographer's, built from textbook definitions rather than from
anyone's implementation: contour lines (isolines of elevation) at a fixed vertical
interval with heavier index contours, the convention of US and UK survey sheets; the
standard analytical hillshade, Lambertian reflectance of the surface normal under a
light at an azimuth and altitude (315° and 45° by convention), in the zenith / slope /
aspect form GIS packages document; hypsometric tints (layer colouring by elevation
band) and bathymetric tints below sea level; and cartographic generalisation by
smoothing. The distance of a pixel from an isoline, |h − level| / |∇h|, is the
first-order estimate used throughout the rendering of implicit curves. No agency's
symbology, colour table or name is used.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
