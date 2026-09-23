# contour

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The map is not asserted but
> measured: an offline harness drives the real plugin class in a headless GL context and
> reads each claim back out of the picture — a ramp crosses exactly the stated number of
> contours; adjacent isolines sit interval / slope apart, exactly on whole pixels and
> within the pixel lattice's own bound on fractional ones; every contour on terrain whose
> slope spans 8:1 is drawn the same width on the page, to a bound derived per stroke;
> planes of known slope and aspect shade to the cartographer's closed-form hillshade; the
> sea covers exactly the pixels below sea level and the shoreline sits on that isoline; a
> noisy plateau lying exactly on a contour level draws nothing — with sixteen negative
> controls that prove each check can fail, and one mutation of the shipped GLSL that two
> of them catch. It has **never been loaded into Resolume**. It is loaded by
> [oxbow](https://github.com/stoatworks-labs/oxbow), which is a real FFGL host and is not
> Resolume. See [Status](#status).

Luma read as elevation and drawn as a topographic map — an FFGL effect for
[Resolume](https://resolume.com) Arena and Avenue.

![A synthetic terrain drawn as a survey map: two hills ringed by brown contours, closer together on the steep flanks, a heavier index contour every fifth line, hill-shading lit from the upper left, green-to-sand elevation tints, a pale blue sea with a dark shoreline, and a round lake in a hollow](docs/hero.png)

<sub>One frame, rendered by `cntest`, the offline harness — not captured from Resolume.
The defaults with Sea Level raised to 0.2, on the harness's moving terrain
card.</sub>

## The one idea

Treat the picture's brightness as ground height, and draw it the way a survey map does:
**isolines at a fixed interval, heavier index contours, hill-shading from the
north-west, hypsometric tints, and a sea that the music can raise.**

The mechanism is the cartographer's, and so are the artefacts.

## What falls out

None of these is drawn on purpose:

- **Contours crowd on steep ground and open out on gentle ground.** The spacing of the
  lines is interval / |∇h|. Nothing places them; the spacing *is* the slope.
- **Every line is one pen width, on a cliff and on a plain.** Each line is drawn from
  its distance on the page, |h − level| / |∇h|, not from |h − level| alone — which would
  make it a hairline on a cliff and a smear on a plain. The harness proves the
  difference: take the division out and the check fails by a factor of eight.
- **Noise makes an unreadable map until the height is generalised.** Survey data has to
  be smoothed before it is contoured, and so does a picture. Smooth is that step.
- **The shoreline is simply the isoline at sea level**, so a flood follows the terrain:
  raise the sea and it pours into the low ground first and leaves the hills as islands.
- **Moving footage becomes a moving landscape.**

### The honest limit

Flat ground has no contour. Below a slope of one 8-bit code value over eight pixels
(less when Smooth is up), an isoline's position is decided by quantisation and noise
rather than by the picture, so contours fade out there instead of scribbling across a
sky or a wall. The flip side is that a very gentle gradient carries no lines at all
until you smooth more. And the ground is the picture's brightness: a bright shirt is a
hill whether or not it is in front of anything.

## Controls

| Group | |
| --- | --- |
| **Terrain** | Height Source (Luma, Red, Green, Blue, Alpha), Height Scale (0–2× the channel), Invert, Smooth (the generalising Gaussian, σ up to 16 px). |
| **Contours** | Interval (elevation between lines), Index Every (2–10), Line Width and Index Width (in pixels), Line Colour, Contours On. |
| **Relief** | Hillshade (how much), Azimuth (compass, 315° by default), Altitude (45° by default), Z Factor (vertical exaggeration, 0.1–10). |
| **Tints** | Tint Mode (Classic, Mono, Off), Tint Strength. Land is banded by elevation between contours; the sea darkens with depth. |
| **Sea** | Sea Level (a fraction of the height range), Audio Rise, Water Colour, Shore Width. |
| **Audio** | the FFT input, Audio Mode (Level: the sea breathes with the loudness; Onset: it surges on a hit and drains), Audio Band (Full Range, Bass, Mid, Treble). |
| **Output** | Paper Colour, Mix (over the clip). |

With no audio routed the sea stays where Sea Level puts it. The onset detector is primed
on the first frame and after every jump in the clock, so triggering a clip with the
music already playing does not flood the map.

## Status

**v0.1.0, and honestly early — 23 September 2026.**

### Measured offline, on macOS

`tools/verify.sh` passes on this machine (M4 Max, macOS 26.4.1) against a fresh
universal Release build, running every rendered check at **two rasters**, 320×180 and
1280×720; the same checks also pass at 333×187 and 1920×1080. What it establishes:

| check | result |
| --- | --- |
| `--count` | five ramps (20 to 150 contours, at Height Scale ×1, ×1.5 and ×0.5) cross **exactly** the stated number of contours; the boundary rule is exercised both ways |
| `--spacing` | whole-pixel spacings (8, 12, 16 px) land **exactly** (worst 5.7e-5 px); fractional ones (7.3, 10.6, 13.9 px) within 0.064 px of the lattice's own bound of 0.067; fitted spacing within 6e-4 px |
| `--width` | on terrain whose slope spans **8:1**, every one of 40 (320×180) and 175 (1280×720) contours is 1.20 px wide on the page, or 2.40 for an index contour, each within a curvature bound derived for that stroke — 1.193–1.232 px unsmoothed at 320×180, 1.195–1.203 at the default Smooth |
| `--hillshade` | ten planes of known slope and aspect, three lights and two Z Factors shade to the cartographer's zenith/slope/aspect formula within a few ulps of the elevation — worst 3.8e-5 inside the frame and 6.4e-5 on its border at 1280×720, each under its derived tolerance |
| `--sea` | every pixel below sea level is water and none above — **0** of 57,600 and 921,600 wrong, including 14,220 / 229,680 lying exactly at sea level; the shoreline centred on its isoline within 0.041 px (spec: half a pixel) and within 2e-7 px for an integer width; the Rec. 709 luma weights and Invert read back through the shoreline within 1e-5 px |
| `--index` | every Nth contour, and only those, heavy — N = 2, 3, 5 and 10 |
| `--flat` | a plateau lying **exactly on a contour level**, with a noise floor whose smoothed slope is provably under the guard, draws **no ink at all**; without the guard it is a labyrinth covering a fifth of the plateau |
| `--audio` | a constant loud spectrum from the first frame leaves the sea still (primed); an onset raises it by the stated half-range on that frame and drains it monotonically; Level puts it at 0.2 + 0.5 √v; the band routing and a 5 s scrub, each within one column of the ramp |
| `--resize` | a resize mid-run keeps the audio drive, and the first frame after it matches a fresh instance at the new size in every one of 4,157,044 values |
| offline | the ten control mappings against this README's statement; the detector's gain and its releases measured from its output; a six-day millisecond clock drives the sea as a fresh one does; names within 16 characters |
| `--negative` | sixteen perturbed models — no division by the slope, no guard, the light from the south, Z Factor ignored, levels half a step off, the interval 3% wide, index contours off by one, the sea too low, the shoreline half a pixel uphill, an unprimed detector, a detector reset on resize, stale buffers after a resize, a float clock — each **fails** its check |
| mutation | one character of the shipped GLSL (the stroke's coverage, `d - 0.5` → `d - 0.4`) was caught by `--width` and `--sea`, then reverted |
| `tools/sweep.py` | all **30** controls measurably change the picture |
| shaders | all 4, as the plugin compiles them, through `glslc` |
| `--pipe` | 2.5 frames in, exactly 2 out; an unknown cue refused (2); a failed render and a closed stdout each exit 1 |
| the bundle | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs; `oxbow` reports `SW Contour` / `CN01` / `effect` and renders 120 frames through `plugMain` |

Render cost at the defaults, best of three runs of 60 frames after a warm-up,
`glFinish` both sides, on a GPU shared with other work: **0.11 ms** at 720p,
**0.29 ms** at 1080p, **1.28 ms** at 4K. At the most Smooth (σ 16, 97 taps an axis):
**0.32 ms**, **0.73 ms**, **2.90 ms**. macOS figures only.

### Not established

It has **never been loaded into Resolume**, on either platform. Everything above was
compiled, rendered and measured offline against the real plugin class in a headless
CGL context, plus an `oxbow` load. How it looks on real footage, how 30 controls read
in Arena's inspector, and what Resolume's FFT bins actually carry are untested. The
Windows build is CI-only and has never run. No OpenFX port, no browser demo and no
factory presets, none in scope for 0.1.0. No user guide.

## Build

Needs CMake 3.15+, a C++17 compiler, and the FFGL SDK submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/contour
cd contour
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

macOS builds are universal (Apple Silicon + Intel) by default; add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster dev build. Windows needs GLEW via vcpkg.

## Building and testing

The offline harness renders the real plugin class headlessly:

```bash
./build/cntest --out /tmp/frame.png --size 1920x1080   # the moving terrain card
./build/cntest --list                                  # every control, kind and default
./build/cntest --count --spacing --width               # each claim, measured
./build/cntest --hillshade --sea --index --flat
./build/cntest --audio --resize
./build/cntest --negative                              # and the checks can fail
./build/cntest --offline                               # what needs no GL (CI)
./build/cntest --bench                                 # 720p, 1080p and 4K
python3 tools/sweep.py                                 # no control is silently dead
tools/verify.sh                                        # all of it, on a fresh universal build
```

Every check takes `--size`; run it at 320×180 as well as the raster you care about.
Footage goes through the real shaders with `--pipe`, in the fleet's frame format:

```bash
ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
  | ./build/cntest --pipe --size 1920x1080 --script cues.txt \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 60 -i - out.mov
```

See [`CLAUDE.md`](CLAUDE.md) for the full command reference and
[`AGENTS.md`](AGENTS.md) for the model and the traps.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).
