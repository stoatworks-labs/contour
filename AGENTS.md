# AGENTS.md — Contour

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

Luma read as elevation and drawn as a topographic map, as an FFGL 2.1 effect (`CN01`,
shown as `SW Contour`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake,
universal macOS `.bundle` and a Windows `.dll`. MIT, intended home
`github.com/stoatworks-labs/contour`. **Local only at v0.1.0** — no remote, no tag,
not registered anywhere, never loaded into Resolume.

Built 2026-09-23 in one session from the fleet's templates and
`specs/SPEC-contour.md` (with `BRIEF.md` and `BRIEF-ADDENDUM.md`): standards for the
harness, `--pipe`, verify, `--offline` and CI; tinsel for `PassBuffer`, the trap list
and the lesson that a line drawn from a field must be divided by its gradient;
intaglio for the discipline of tolerances and an estimator that must not drift with
raster; regauss for the audio declaration and bands; graticule for the notes.

### Not contourtonist, not toolpath

**`contourtonist`** (`~/Projects/audio/contourtonist`) is an equal-loudness EQ plugin
— ISO 226 loudness *contours*, JUCE, audio. It shares a word and nothing else.

**`toolpath`** (`~/dev/toolpath`, being built alongside this) also draws offset
curves, but of a different field for a different reason: it thresholds the picture
into a *region*, computes the region's *distance field* by jump flooding, and draws
level sets of distance from the boundary — the passes of a CNC tool, all parallel to
one outline, with the fillets and scallops of a round cutter, and a machine that cuts
over time. Contour never makes a region and never measures distance to an edge: its
field is the brightness itself, its isolines are **not** parallel to each other, and
their spacing varying with the slope is the whole point. If the two ever converge on
code, it is the stroke coverage (`stroke()` here), which is generic.

---

## The one idea

**Treat the picture's brightness as ground height, and draw it the way a survey map
does**: isolines at a fixed interval, every Nth heavier, hill-shading from the
north-west, hypsometric tints, and a sea that the music can raise. Nothing is drawn
that the cartographer's mechanism does not produce:

| the mechanism | what falls out |
| --- | --- |
| isolines of h at levels k × Interval | **contours crowd on steep ground** — their spacing is Interval / \|∇h\| |
| each line drawn from its distance on the page, \|h − L\| / \|∇h\| | **one pen width everywhere**, cliff or plain |
| a Gaussian of Smooth px on the height first | **noise is unreadable until generalised** |
| water where h < Sea Level, the shoreline the isoline at it | **the flood follows the terrain**, islands appear |
| a Lambert hillshade of the surface lifted by Z × frame height | the relief reads the same at any raster |

### The arithmetic, in one place

    h        = Height Scale × channel (1 − channel inverted), generalised by G_sigma
    g        = ( dh/dx, dh/dy ) per pixel, central difference; one-sided on the edges
    L        = round( h / I ) × I                       the nearest level
    d        = | h − L | / max( |g|, G )               pixels from the isoline
    coverage = | [d − ½, d + ½] ∩ [−w/2, w/2] |         the stroke box ⊛ the pixel box
    fade     = smoothstep( G, 2G, |g| )                 the guard
    hs       = max( 0, n · light ),  n ∝ ( −Z H g, 1 ),  light from Azimuth, Altitude
    sea      = h < S + Audio Rise × drive × ½ × Height Scale

The coverage is exact for a straight isoline: the stroke's box convolved with the
pixel's box, sampled at unit spacing, sums to w wherever the line falls (the unit box
comb sums to one). That identity is what `--width`, `--spacing` and `--sea` lean on.

### The guard

The spec asks for "a stated guard where |∇h| → 0 (flat ground and saddles), so line
width cannot blow up". The obvious guard — `max( |g|, ε )` in the denominator — is
wrong in a way worth writing down: on a plateau that sits exactly on a level,
|h − L| is 0 and the guarded d is 0, so **the whole plateau inks**. A guard on the
denominator alone turns a divide-by-zero into a blob.

What is here instead has two halves:

- **Fade.** A line's coverage is multiplied by `smoothstep( G, 2G, |g| )`, so below G
  there is no line at all and above 2G the line is exactly the model's.
- **Floor.** The denominator is `max( |g|, G )`, so between G and 2G, where the line is
  fading in, its width is exact, and below G (where it is invisible anyway) nothing
  divides by zero or makes a NaN.

**G is one 8-bit code value of elevation over eight pixels, divided by max( 1, σ ).**
Below that slope an isoline's position is set by quantisation and noise, not by the
picture: the distance estimate is a ratio of two numbers that are both noise.
Smoothing averages the quantisation down in proportion to σ, so generalising more lets
flatter ground carry a contour — which is also what an operator does when lines are
missing from a gentle gradient. The effect: contours on ground flatter than about one
code value per 8σ px fade out. Saddles: at the exact saddle point |g| is 0, so the
fade takes the crossing out over a few pixels and the arms of the X are drawn — by
construction; no check puts a saddle on a level.

G is per pixel, so the look is raster-dependent: the same clip at 4K has half the
per-pixel slope it has at 1080p, and gentle ground that carries lines at 1080p can
fade at 4K. Smooth is in pixels too, so raising it compensates. See Open questions.

### What does not fall out, and is the honest limit

- **The ground is the brightness.** A bright shirt is a hill; nothing knows about depth
  or occlusion. Height Source can pick a channel or alpha, and that is all.
- **Index contours are keyed to absolute level numbers** (k = 0 at elevation 0), so
  moving Sea Level does not renumber them, as it would not on a real sheet.
- **Contours are drawn on land only.** Below sea level the bathymetric tint carries the
  depth; there are no depth contours.
- **The coast is hard.** Water is a per-pixel test, h < S, not antialiased; the
  shoreline stroke (1.5 px by default) is what hides the staircase. At Shore Width 0
  the coast is aliased.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Contour.{h,cpp}` | The plugin: parameters, buffers, the three passes, the uniforms, the test hooks. |
| `source/Shaders.{h,cpp}` | Four shaders: vertex, height, blur (one axis), map. |
| `source/Controls.{h,cpp}` | 0..1 host parameters to elevation, pixels and degrees; the guard; the perturbation bits. |
| `source/Audio.{h,cpp}` | `SeaDrive`: 64 bins to one number, Level or Onset, primed. No GL. |
| `source/Clock.{h,cpp}` | standards' clock: unit voting, origin + offset in double, jumps flagged. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/cntest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/check-shaders.sh` | glslc on the dumped shaders; verify.sh and CI both call it. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |

Per frame: `SeaDrive` updates on the CPU; **height** writes the picture's elevation
(R32F, the picture's size); **blur** runs twice (x then y) unless σ is 0, with weights
the CPU normalised in double; **map** draws onto the host's framebuffer at the host's
viewport, which was captured before anything else ran.

---

## Traps

Roughly in the order they bit.

### ☠️ `--width` the check was eaten by `--width N` the raster

standards' harness accepts `--width N` / `--height N` as aliases for `--size`. The spec
names this plugin's headline check `--width`. The first full run printed no width
results at all and a count one short: `--width --hillshade` had set the raster width to
`atoi( "--hillshade" )` — zero — and swallowed the next check. The aliases are gone;
`--size` is the only way to set the raster, and the usage says why.

### ☠️ A clamped central difference halves the slope on the frame's edge

The plugin's first gradient was `½ ( h[x+1] − h[x−1] )` with the index clamped. On the
first and last column that spans one pixel and divides by two: the slope reads half,
and any contour crossing the edge is drawn twice as wide in that column. Found by
`--spacing` at **1920×1080**, where one stroke happened to land 1.1 px from the right
edge — at 320×180 and 1280×720 no stroke did, and the check passed. The difference now
divides by the span it actually has, and `--hillshade` holds the border pixels to a
full ulp.

### ☠️ The check's own edge rule was too narrow

The same failure exposed a defect in `--spacing` and `--index`: they excluded a run only
if it touched the edge pixel. A stroke whose reach includes the edge column but whose
ink there was lost still counted, and its centroid sat 0.085 px off against a 0.070
bound. Both checks now skip any stroke whose reach includes the first two or the last
two columns, on the measured side and on the stated side alike. It passed at the two
verify rasters by luck of where strokes fell, which is the addendum's point about
running a third raster by hand.

### The onset held for 0.6 s

The first detector compared the band's level to a 0.5 s baseline. Writing the onset
check's expected drain showed that a step to a sustained level keeps four times the
excess above 1 until the baseline has closed three quarters of the gap — **0.58 s** of
full flood on every sustained note. The baseline is 0.1 s: the surge holds
τ ln( 4 x ) = 0.12 s for a step to x = 0.8, then drains with the 0.3 s release.
`--audio` now states that hold and its drain from the law, and the detector's release
is measured from its output (`--detector`), not assumed.

### An 8-bit picture's gentle slopes are terraces through the hillshade

At σ ≈ 2 px (the first default Smooth) the harness's own card showed ripples a code
value apart across every gentle slope: Z Factor lifts the surface by the frame height,
so a one-code-value step 20 px wide is a visible facet. Default Smooth is 0.5, σ 4 px.
No check depends on the default.

### Defects found in my own checks, before any of them could pass for the wrong reason

- `--width` first tracked "the worst error" with a comparison that never updated, and
  reported 0 against a tolerance of 0; and its quadratic terrain's *measured* contours
  spanned only 5:1 at 320×180 and 6.9:1 at 1280×720, because a quadratic spends little
  distance on its gentle end. Now a plane at g0, a bend, and a plane at 8 g0: the
  measured slopes span exactly 8.00:1 at every raster.
- `--width`'s curvature bound first ignored that the blur spreads the bend's curvature
  by its own radius, so strokes on the plane beside the bend had a float-only tolerance.
  The window now includes the radius.
- `--detector` expected the Level release after 15 frames where 16 had elapsed, and
  held a float drive to a double's 1e-9. Both corrected from the law, not loosened.
- The mutation pass showed that **no check read the luma weights, Invert or Height
  Scale** — every check fed the red channel at scale 1. `--sea` now reads each Rec. 709
  weight and Invert back through the shoreline, `--count` has ramps at ×1.5 and ×0.5.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
restored before the map, with the host's FBO bound explicitly); every `ffglex::Scoped*`
clears to 0 on exit, so every `Ensure()` happens before anything binds a texture;
`FFGLFBO::Release()` leaks the colour texture, which is why `PassBuffer::Destroy()`
deletes it first; `SetParamInfo` clamps a STANDARD default into 0..1; an option's range
reads back 0..1 whatever its element count; the core is an **OBJECT** library;
`SetTextParameter` must return `FF_SUCCESS` for the About block; `FFGLShader::Set` has
no array overload (the blur's 49 weights go through `glUniform1fv`); Resolume's clock
overflows a float, so the audio clock is origin + offset in double and nothing absolute
reaches a shader; the onset detector is primed on the first frame and on every clock
jump; a resize must not disturb state across frames (here only `SeaDrive`, which is
CPU-side and untouched); `nm | grep -q` fails under pipefail when grep succeeds; GLSL
reserved words (`patch sample input output filter common active half layout flat`) are
scanned for by verify.sh without glslc; `--pipe` ignores SIGPIPE so a closed stdout is
exit 1, not 141, and verify.sh tests it.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at 320×180
and 1280×720 in `verify.sh`, and at 333×187 and 1920×1080 by hand.

What makes them rasteriser-proof by construction: **every coordinate is an integer**
(`gl_FragCoord` and the viewport, never an interpolated uv), **every read is
`texelFetch`** (no filtering precision), **every conversion is done on the CPU in
double** and handed over as a float uniform, and **the input is a float texture**, so
the elevation the shader sees is the float the harness wrote. What a driver can still
do differently is arithmetic: GLSL 4.10 §8.2 bounds it, and each tolerance carries its
share. Deliberately not relied on: `mix( a, b, 1 ) == b` (the hillshade tolerance
allows the x + a (y − x) form's rounding), exact cancellation of two computed
elevations, and `pow` anywhere in a check's path.

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--count` | inked runs along three rows of a linear ramp | **exact integer**: n = #{ k : h(0) < k I < h(W−1) }, the boundary rule; a precondition keeps every level a stroke and a pixel from both ends | the spacing is adjusted so the ramp spans N + ½ intervals at any width; the start fraction alternates ¼ / ¾ so the rule is exercised both ways at every raster |
| `--spacing` whole-pixel | stroke centroids, s = 8, 12, 16 px, every level on a pixel centre | **float only**: (w+2)(w+1)ε/2w, ε the per-pixel error of d (3 ulp of h over g, the slope's half-ulp, the division, the readback) — symmetric samples put the centroid on the centre exactly | positions are in pixels from x0 = 5; the float term grows with the elevation reached, so it is larger at wider rasters (4e-4 → 1.7e-3 px) |
| `--spacing` fractional | the same at 7.3, 10.6, 13.9 px, levels anywhere | **the lattice's sawtooth**: r(1 − r)/2w, r = frac(w) = 0.2 → 0.0667 px, plus the float term; the fitted spacing to tol × Σ\|k − k̄\| / Σ(k − k̄)² | none beyond the float term; measured 0.0617 against 0.0667 at every raster — the bound is tight, which is what a derived bound should be |
| `--width` | Σ coverage of every stroke on a plane / bend / plane terrain spanning 8:1 | per stroke: κ(w+1)²/2 (1-Lipschitz coverage, at most four samples on the two one-pixel ramps, κ = \|h''\|/h' over the stroke's reach + 1 px + the blur radius) plus (w+2)ε; on the planes κ = 0 | the terrain spans the frame at any width; κ scales as 1/width, so the bound tightens at larger rasters (0.18 → 0.048 px unsmoothed) |
| `--hillshade` | every pixel of ten planes against the zenith/slope/aspect formula | Z H √2 · ½ ulp(h_max) (each gradient component's rounding, lifted into the slope; \|∂hs/∂z_i\| ≤ 1) + 16 × 2⁻²⁴ (normalize, dot, the light's uniform, the mix); border: a full ulp | the lift is Z × frame height, so the tolerance scales with H (4.8e-6 → 1.2e-4 inside); measured worst stays under it at every raster |
| `--sea` classification | every pixel's water/land against h < S | **exact**: h is the float written, S the float uniform; strict `<`, with 14,220 / 229,680 pixels exactly at S | none |
| `--sea` shoreline | the stroke's centroid on ramps along x and y, 1.8 and 3 px, at 0 and 0.37 px phase | the sawtooth bound (0.045 px at 1.8, **zero** at 3) plus float; the spec's half pixel is looser and is not what is used | none: positions are in pixels from the middle |
| `--sea` sources | the shoreline on a one-channel ramp against Rec. 709's weight, and on an inverted ramp | float only at an integer width, with ε taken at the stroke's local elevation (6.4e-5 px); a fourth-figure weight error moves it 2e-3 to 0.2 px | none |
| `--index` | the heavy strokes' level numbers | **exact set equality**, strokes classified by mass against the midpoint of 1.2 and 3 px (errors are ~1e-5) | strokes whose reach includes the first or last two columns are excluded on both sides |
| `--flat` | ink on a noisy plateau lying on a level | **exactly 0**: the noise (±0.1 code value) is chosen so the smoothed slope is provably under G — ½ a Σ\|w_{k+1} − w_{k−1}\| √2 from the stated kernel — and smoothstep is exactly 0 below its edge | per pixel, so raster-free; the plateau is the frame's left half |
| `--audio` | the waterline's column on a 0..1 ramp | **one column** (1/W of elevation): the midpoint of the column interval is within half of it | 1/W: 3.1e-3 at 320, 7.8e-4 at 1280 |
| `--resize` | the drive through a resize; the frame after it against a fresh instance | drive exactly 0; **every value equal** | resizes to 1.5 W + 1 × 0.75 H + 1, a different aspect, at any raster |
| `--controls` | ten mappings at 21 points, the guard, the defaults | 1e-12 relative (two statements of one definition in double); the defaults exact | none (no GL) |
| `--detector` | SeaDrive: primed, saturation, the releases | the Onset release τ from two late frames' ratio, to τ 2⁻²³ / \|ln r\| (the drive is a float); Level to 1e-6 | none (no GL) |
| `--clock` | a six-day millisecond clock against a fresh seconds one, 3,000 frames | 1e-3: a double at 1.02e9 s resolves 1.2e-7 s, 2.4e-6 relative per release factor, compounded over the stimulus's runs; a float clock resolves 64 s and misses by 0.31 | none (no GL) |

What might still differ on another rasteriser: a driver whose `smoothstep` is not
exactly 0 below its first edge would put ink on the flat plateau (the spec's formula
clamps to 0 before the polynomial, so it would be non-conforming). A software context
is a different compiler for the same GLSL; `check-shaders.sh` covers syntax on the
GL-less runner and the rendered checks run with `--allow-no-gl` so a runner without a
context skips loudly.

### The negative controls

`cntest --negative` runs fourteen against rendered checks and `--offline` two against
the model; `--perturb BITS` runs any check verbosely against one. Each perturbs the
*plugin's* model — a bit in `contour::perturb`, zero in the shipped plugin — never the
harness's expectation.

| perturbation | what fails, measured at 320×180 |
| --- | --- |
| no division by \|∇h\| (the spec's) | `--width`: widths 0.10 to 2.40 px against 1.20, and the steep side's strokes merge |
| no guard | `--flat`: 4,650 px of ink on a 23,616 px plateau that should be blank |
| the light's north component negated | `--hillshade`: errors of 0.34 to 0.72 on every sloped plane |
| Z Factor ignored | `--hillshade`: the Z 3.16 plane off by 0.071 |
| levels at ( k + ½ ) I | `--count`: 33 against 34, 27 against 26, 19 against 20; `--spacing`: every centroid half a spacing off |
| interval 3% wide | `--count`: wrong on four of the five ramps (33 against 34, 19 against 20, …); `--spacing`: the step off by 0.24 px and more |
| index on k = −1 mod N | `--index`: the wrong levels heavy (11 against 10 at N = 3) |
| water a quarter interval low | `--sea`: 1,356 pixels misclassified; `--audio`: the waterline 0.017 low |
| shoreline half a pixel uphill | `--sea`: every shoreline 0.49–0.50 px off |
| onset detector unprimed | `--audio`: the sea floods to 0.70 on frame 0 and after the scrub |
| detector reset, unprimed, on a resize | `--resize`: drive 1 after the resize |
| height buffers kept at the old size | `--resize`: 172,023 of 261,664 values differ |
| offline: unprimed | `--detector`: drive 1 from a constant spectrum, and after a jump |
| offline: the clock kept in float | `--clock`: drives differ by 0.31 |

Read what each failed on, not only that it did (standards' lesson): the list above is
from `--perturb` runs, and every one fails for the reason it was built for.

### The mutation

One character of the shipped GLSL, on a clean committed tree (2c694be): in `stroke()`,
`max( d - 0.5, -hw )` → `max( d - 0.4, -hw )` — the pixel's footprint made 0.9 px
wide. Caught at both rasters by `--width` (all four cases: widths 0.90–1.01 px against
1.20) and by `--sea`'s fractional shorelines (four cases: 0.057 px off against 0.045,
and 0.020 against 5.6e-4 at 3 px). `--spacing` passed, correctly: the mutated stroke is
still symmetric about its centre, so centroids do not move. `--count`, `--index`,
`--hillshade`, `--flat`, `--audio` and `--resize` passed, correctly. Reverted with
`git checkout source/Shaders.cpp`; the tree was clean before and after.

A second probe, while adding the source checks: `0.7152` → `0.7153` in the height
shader's luma is caught by `--sea` (the green-only shoreline 2.2e-3 px off against
6.4e-5). Honestly recorded: that tree was not clean — the harness's tolerance edit was
uncommitted beside it, and was committed next as 833133c. Shaders.cpp was reverted with
`git checkout` the same way.

---

## Decisions taken without asking

- **Units.** Elevation is the channel × Height Scale (0..2); Interval and Sea Level are
  in it. Widths and Smooth are pixels, as the spec says, so the pen is the same on any
  raster and the map is not. The hillshade lifts the surface by Z × **frame height**, so
  relief reads the same at any raster.
- **Mappings** are linear or plain powers so a harness can pick exact values: Interval
  p²/4 (0.5 → 1/16), Smooth σ = 16 p², Line Width 4p, Index Width 6p, Shore Width 6p,
  Azimuth 360p, Altitude 90p, Z Factor 10^(2p−1), Height Scale 2p, Sea Level p × scale.
- **The guard** as above: one code value over 8 px, ÷ max( 1, σ ); fade over [G, 2G].
- **The slope** is a central difference, one-sided on the edges; the blur clamps (the
  ground carries on level past the frame).
- **Coverage** is the stroke box convolved with the pixel box across the line, not a
  smoothstep: it makes the width identity exact and the lattice bounds derivable.
- **Index contours** are k ≡ 0 mod N from elevation 0, and use the same colour.
- **Contours on land only**; the sea has a bathymetric tint by depth band (up to 55%
  darker at full Tint Strength), and no depth contours. **Hillshade on land only.**
- **The coast is a hard per-pixel test**, h < S; the shoreline stroke is the
  antialiasing. The stroke's colour is 0.4 × Water Colour.
- **Tints are banded between contours** (the band's mid-elevation picks the colour), as
  layer colouring is; Classic runs green → pale green → sand → brown → snow from the
  sea to the top of the range, Mono is a grey ramp.
- **Audio**: Level and Onset over a band's mean of √bin, regauss's bands (Full, 0–7,
  8–27, 28–63); Level releases in 0.25 s; Onset is 4 × the rise above a 0.1 s baseline,
  released in 0.3 s; the rise is Audio Rise × drive × half the height range. Defaults
  Onset on Bass — a surge on the kick. The FFT bins are not assumed linear in anything.
- **Defaults**: Smooth 0.5 (σ 4), Interval 0.45 (0.0506, about twenty lines over the
  range), Index Every 5, lines 1.2 and 2.4 px in brown, Hillshade 0.6 from 315° at 45°,
  Z 1, Classic tints at 0.6 on cream paper, Sea Level 0.2, Shore 1.5 px.
- **Paper and Mix**: the map is opaque on paper; Mix < 1 blends it over the clip.
- **No factory presets, no OpenFX, no browser demo** — not in the spec for 0.1.0.
- **`--pipe` feeds no spectrum**; `--out` and the sweep feed the card's own.
- **The FFGL submodule is dissociated** (repacked, alternates removed) rather than
  borrowing tinsel's object store through `--reference`.
- **Provisional About and attributions** (`StoatworksAbout.h`, `ATTRIBUTIONS.md`) are
  hand copies adapted from standards' with `guide=""`; the button count, and so the
  parameter count, does not change when the fleet's sync regenerates them.
- **Commit trailers name the model that did the work** (`Claude Opus 5.5`), as the
  session's instructions said, not the brief's `Claude Fable 5.1`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1 (2026-09-23)

Every number is `tools/verify.sh` on this machine against a fresh universal Release
build, at 320×180 and 1280×720, with the same checks passing at 333×187 and 1920×1080
by hand.

- **Count.** Five ramps, 20 to 150 contours, at Height Scale ×1, ×1.5 and ×0.5: exact.
- **Spacing.** Whole-pixel exact to 5.7e-5 px; fractional within 0.0634 px of the
  0.0667 lattice bound; the fitted spacing within 6e-4 px.
- **Width.** 40 and 175 contours on slopes spanning 8.00:1, each within its own bound:
  1.193–1.232 px unsmoothed at 320×180 (worst 0.032 against 0.18), 1.199–1.208 at
  1280×720; 1.195–1.203 at the default Smooth.
- **Hillshade.** Ten planes, worst 6.0e-6 inside the frame and 1.2e-5 on its border at
  320×180, 3.8e-5 and 6.4e-5 at 1280×720, each under its derived tolerance.
- **Sea.** 0 of 57,600 and 921,600 pixels misclassified; the shoreline within 0.041 px
  at 1.8 px (bound 0.045) and 2e-7 at 3 px; Rec. 709 weights and Invert within 1e-5 px.
- **Index.** N = 2, 3, 5, 10 exact.
- **Flat.** 0 ink on the plateau; ±2 code values (over the guard) leaves 8% ink, for
  the record.
- **Audio.** Primed, the onset's peak and drain, Level at √v, the bands and a scrub,
  each within one column.
- **Resize.** The drive carried; 0 of 4,157,044 values differ from a fresh instance.
- **Offline.** Mappings to 0 relative; the detector's releases measured; the six-day
  clock within 6e-8.
- **Negative controls.** All sixteen fail their check.
- **Mutations.** Both caught (above).
- **No dead controls**, all 30, at 320×180 and at CI's 160×90.
- **Every shader compiles** through `glslc`; no reserved word declared as a name.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown cue
  with 2, and exits 1 on a failed render and on a closed stdout.
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.contour` and version 0.1.0 in all three places, ad-hoc signs, and
  `oxbow` reports `SW Contour` / `CN01` / `effect` and renders 120 frames through
  `plugMain`.
- **Render cost**, best of three runs of 60 frames after a warm-up, `glFinish` both
  sides, on a shared GPU:

  | | ms/frame | % of a 60fps frame | Smooth 1 (σ 16) |
  | --- | --- | --- | --- |
  | 1280×720 | 0.13 | 0.8% | 0.32 |
  | 1920×1080 | 0.29 | 1.7% | 0.73 |
  | 3840×2160 | 1.33 | 8.0% | 3.02 |

### Assumed, or not done

- ☠️ **Never loaded into Resolume**, on either platform. Everything was compiled,
  rendered and measured offline against the real plugin class in a headless CGL
  context, plus an `oxbow` load.
- **Never seen on real footage.** Every picture so far is synthetic terrain. Whether a
  face or a crowd reads as a landscape, and what Smooth it needs, is unjudged.
- **Resolume's FFT bins are unmeasured** (fleet-wide). The Level law √bin and the band
  split are regauss's choices, not measurements; `--audio` proves the plumbing, not the
  musicality.
- **The guard's constant is a judgement** — "one code value over eight pixels" is
  argued, not measured on real 8-bit footage; and it is per pixel, so the look changes
  with raster (see the guard).
- **The Windows build is CI-only** and CI cannot run yet.
- **Not verified at 4K**, only benchmarked there.
- **No OpenFX port and no browser demo.** Not required for 0.1.0.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies** with
  `guide=""`; register the project and re-run the syncs before the first release.
- **Nothing has been through a show.**

---

## Open questions

- **Should the guard and Smooth be in fractions of the frame?** Pixels make the pen the
  same at any raster (the spec's choice) but make the generalisation, and so which
  gentle ground carries lines, change with raster. A clip moved from a 1080p to a 4K
  composition loses lines on its gentlest slopes.
- **Should the coast be antialiased** rather than hidden under the stroke? A coverage
  test on (S − h) / |∇h| would do it and would need `--sea`'s classification half to
  become a coverage check.
- **Depth contours under the sea?** Charts have them; this has only the tint.
- **Should Onset be relative to the band's recent level** (a ratio, not a difference)?
  A quiet track and a loud one surge differently at the same Audio Rise.
- **The terraces of 8-bit input.** σ 4 hides them on the card; real footage with a
  shallow gradient (a sky) may need more, and a dither or a smarter reconstruction of
  the quantised height could replace brute smoothing.

---

## Siblings

- **standards** — the harness, `--pipe` (with SIGPIPE ignored), `--offline`, verify, CI,
  the clock, and the negative-control pattern.
- **tinsel** — `PassBuffer`, `sweep.py`, the fleet's trap list, and "a lamp measured in
  strip units is a streak": the same division by a field's gradient that keeps a
  contour one pen wide.
- **intaglio** — every tolerance with a derivation, and an estimator that must not drift
  with raster.
- **regauss** — the FFT declaration, the √bin law and the three-way band split.
- **toolpath** — the other level-set plugin of this wave; see above for the difference.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
