# NanoVG offscreen pixel-regression tests

Deterministic, headless rendering regression suite for the NanoVG GL3 backend.
Each scene is rendered offscreen (EGL surfaceless context + FBO, no window
system needed) with the Mesa **llvmpipe** software rasterizer, read back with
`glReadPixels`, and compared **bit-exact** against committed golden images.

## Running

```sh
make test      # build runner + compare all scenes against golden/
make update    # build runner + (re)generate golden/ images
make clean     # remove binary and out/ diff artifacts
```

Toolchain variables can be overridden, e.g.:

```sh
make CC=/path/to/gcc \
     EGL_CFLAGS=-I/path/to/prefix/usr/include \
     EGL_LIBS="/path/to/libEGL.so.1 /path/to/libGL.so.1 -Wl,-rpath-link,/path/to" \
     test
```

Run a single scene by passing a name substring to the binary, e.g.
`./nvgtest_render test fill`.

## Scenes (400x300, white background)

| Scene             | Covers |
|-------------------|--------|
| `fill_basic`      | solid/rounded/circle/ellipse fills, path with hole, linear/radial/box gradients, translucent overlaps, bezier, self-intersecting star |
| `stroke_basic`    | line caps (butt/round/square), joins (miter/round/bevel), widths 1–9, stroked circle/rect/bezier, translucent stroke overlap |
| `text_basic`      | sizes, left/center/right alignment, letter spacing, blur, alpha, text box, multi-line |
| `image_basic`     | procedural RGBA images, scaled/rotated/alpha patterns, repeat wrapping, image-in-circle, image under transform |
| `clip_scissor`    | axis-aligned scissor, rotated scissor, `nvgIntersectScissor`, scissor under scale |
| `transform`       | translate/rotate/scale/skew, nested transforms, rotated text, `nvgResetTransform` |
| `composite_combo` | all of the above combined in one frame |

## Determinism & cross-machine reproducibility

- Software rendering is forced (`LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe`,
  also set inside the runner), so results do **not** depend on the host GPU.
- All scene geometry, colors, fonts (`example/Roboto-Regular.ttf`) and images
  (generated procedurally with fixed integer math) are constant — no time, no
  randomness, fixed `devicePixelRatio = 1`.
- Golden files (`golden/<scene>.nvg`) are raw top-down RGBA with a 16-byte
  header (`NVGGLD01` + width + height); comparison is exact, byte for byte.
- Goldens are reproducible across machines **as long as the Mesa/llvmpipe
  version matches**. A rasterizer upgrade may legitimately shift anti-aliasing
  by a pixel; that is a rasterizer change, not a NanoVG regression — verify
  with the diff artifacts, then regenerate with `make update`.

## Failure output

On mismatch the runner exits 1 and prints localization stats, e.g.:

```
FAIL fill_basic: 1/120000 pixels differ (0.001%), max channel delta 255, bbox (0,25)-(0,25)
  diff at (0,25): golden=ff00ff00 actual=ffffffff
  wrote out/fill_basic.{golden,actual,diff}.png
```

- diff pixel count, percentage, max per-channel delta, bounding box, and the
  first 5 differing pixels with exact golden/actual RGBA values;
- `out/<scene>.golden.png` / `.actual.png` / `.diff.png` for visual inspection
  (diff image: black where identical, amplified per-channel error elsewhere).

A missing golden also fails the scene (hint: `make update`).

## Adding a scene

Add a `static void scene_*(NVGcontext*, float, float)` draw function in
`nvgtest_render.c`, register it in `g_scenes[]`, then run `make update` and
commit the new `golden/<scene>.nvg` + `.png`.
