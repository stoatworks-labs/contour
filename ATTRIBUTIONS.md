# Attributions

Contour is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The off-screen buffer wrapper is tinsel's, by way of rebate and standards; the lesson that a line drawn from a field must be divided by the field's gradient to keep one width on the page is tinsel's and intaglio's, applied here to isolines.

### Harness shape, --pipe contract and the host clock — Stoatworks standards

<https://github.com/stoatworks-labs/standards>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness shape, the --pipe contract, --offline, check-shaders.sh, the host clock (readout's unit voting, kept in double) and the negative-control pattern come from standards, pitch and slowscan.

### Audio declaration and band split — Stoatworks regauss

<https://github.com/stoatworks-labs/regauss>  
Licence: MIT  
Copyright: Stoatworks Labs

The audio declaration and the three-way band split over 64 bins are regauss's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Topographic survey maps

Contour lines at a fixed vertical interval with heavier index contours, the standard analytical hillshade (Lambertian reflectance under a light at 315 degrees and 45 degrees, in the zenith/slope/aspect form GIS packages document), hypsometric and bathymetric tints, and generalisation by smoothing. Built from textbook cartography; no agency's symbology, colour table or name is used.

## Standards and published specifications

What the implementation is measured against.

- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — The pcg_hash output mix the harness uses for its noise, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
