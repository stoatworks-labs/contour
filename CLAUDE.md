# contour

Luma read as elevation and drawn as a topographic map — contours, index contours,
hillshade, hypsometric tints and an audio-raised sea — as an FFGL **effect** for
Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows
`.dll`. MIT.

Read `AGENTS.md` before changing the distance-on-the-page arithmetic, the guard, the
edge slope or the audio detector.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel 4`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/cntest --out /tmp/f.png --size 1920x1080` (the moving
  terrain card, with its own bass-pulse spectrum so the sea's audio controls are live)
- Set anything by name: `--set "Smooth=0.5" --set "Tint Mode=1" --set "Index Every=4"`
  (0..1 for sliders, the element index for options, the real integer for Index Every)
- List parameters, kinds, defaults and ranges: `./build/cntest --list`
- The exact GLSL the plugin compiles: `./build/cntest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps, default 60; only the
  audio drive reads the clock) and an optional `--script` of `frame Parameter Name value`
  cues, linearly interpolated between a name's cues and held before the first and after
  the last — an option index interpolated passes through the options between, so key a
  cut two cues a frame apart. A cue naming no parameter exits 2 before any frame; a
  partial frame at the end of stdin ends the stream with exit 0; a failed render or a
  closed stdout exits 1 (SIGPIPE is ignored so a closed stdout is a failed write, not a
  141). `--pipe` feeds no spectrum:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/cntest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`
- There is no `--width N`/`--height N` raster alias (standards has one): `--width` is a check.

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + a reserved-word scan +
  the offline checks + every rendered check at 320x180 AND 1280x720 + the --pipe
  contract + the sweep + the bundle, ~20 s)
- Contours crossed along a ramp, exactly: `./build/cntest --count`
- Isolines interval / g apart, whole-pixel and fractional: `./build/cntest --spacing`
- One pen width on slopes spanning 8:1 (the headline): `./build/cntest --width`
- Planes shade to the closed-form hillshade: `./build/cntest --hillshade`
- Water exactly below sea level; shoreline on its isoline; luma weights, Invert:
  `./build/cntest --sea`
- Every Nth contour heavy: `./build/cntest --index`
- A noisy plateau on a level draws nothing (the guard): `./build/cntest --flat`
- The sea rises with the music; onset primed: `./build/cntest --audio`
- A resize keeps the drive, leaves no stale buffer: `./build/cntest --resize`
- The checks can fail: `./build/cntest --negative`; one perturbation verbosely:
  `./build/cntest --width --perturb 1` (bits in `Controls.h`, `contour::perturb`)
- No GL (what CI runs): `./build/cntest --offline` = `--controls --detector --clock --names`
  and their negative controls
- Every rendered check takes `--size WxH`; CI runs them at 320x180 with `--allow-no-gl`
- Shaders through glslc: `tools/check-shaders.sh build/cntest`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/cntest --bench` (best of three, defaults and Smooth 1)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Contour.bundle`

## Notes
- **A line is drawn from its distance on the page**, `|h - level| / |grad h|` in pixels,
  and its coverage is the stroke box convolved with the pixel box. Never draw from
  `|h - level|` alone; `--width` fails by 8× if you do.
- **The guard** (`controls::GuardSlope`): one code value over 8 px, divided by
  `max( 1, sigma )`. Below it lines fade (smoothstep from guard to twice it) and the
  denominator stops there. It is what keeps a noisy plateau from inking wholesale.
- **The slope is a central difference, one-sided on the edge columns and rows.** A
  clamped central difference there halves the slope and doubles a line's width.
- **Every read is `texelFetch` at an integer coordinate**; every buffer is R32F Nearest.
- **North is up and +y in GL**; the light is `( cos alt sin az, cos alt cos az, sin alt )`
  with x east, y north. The harness builds pictures top row first and flips on upload.
- **The harness never re-types the plugin.** Control mappings, the hillshade (in its
  zenith/slope/aspect form) and the audio laws are stated in `cntest` from the README.
- **The only state across frames is `SeaDrive`** (CPU): primed on the first frame and on
  every clock jump; carried across a resize untouched.
- **`Perturb` bits are test hooks**, always 0 in the plugin.
- **Parameter names must be unique and ≤ 16 characters** — `--set`, the sweep and hosts
  find them by name. The colour triples are `Line Colour`/`Line_Green`/`Line_Blue` etc.
- `SetParamInfo` clamps a STANDARD default into 0..1; options are mapped by index in
  `Controls.cpp` (an option's range reads back 0..1); Index Every is a real
  `FF_TYPE_INTEGER` with range 2..10.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `contour_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `CN01`, display name `SW Contour`.

## Not done yet
- **Never loaded into Resolume.** Everything numeric is measured offline on macOS,
  plus an `oxbow` load. The Windows build is CI-only and has never run.
- Never seen on real footage, only on synthetic terrain.
- No user guide, no OpenFX port, no browser demo, no factory presets.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies with `guide=""`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/contour/contour.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\contour\logs\contour.YYYY-MM-DD.log   (Windows)
