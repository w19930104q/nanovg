/*
 * nvgtest_render - offscreen deterministic pixel-regression runner for NanoVG.
 *
 * Renders a fixed set of scenes (fill, stroke, text, image, scissor/clip,
 * transforms, and a combined scene) into an offscreen FBO using the NanoVG
 * GL3 backend on a headless EGL (surfaceless) context, reads the pixels back,
 * and either:
 *
 *   update  - writes golden images (tests/golden/<scene>.nvg + .png preview)
 *   test    - compares rendered pixels bit-exact against the goldens
 *
 * On mismatch it prints localization stats (diff count, max channel delta,
 * bounding box, first differing pixels) and writes
 * <out>/<scene>.{golden,actual,diff}.png for visual inspection.
 *
 * Determinism: rendering is done with Mesa llvmpipe (software GL), forced via
 * LIBGL_ALWAYS_SOFTWARE=1 (set both here and in the Makefile). All scene
 * geometry, colors, fonts and images are fixed; nothing depends on time,
 * randomness, or the host GPU. Goldens are reproducible across machines as
 * long as the software rasterizer (Mesa/llvmpipe) version matches; a
 * rasterizer upgrade may legitimately shift anti-aliasing by a pixel, in
 * which case goldens must be regenerated with `make update`.
 *
 * Usage:
 *   ./nvgtest_render test|update [--golden DIR] [--out DIR] [--font FILE] [filter]
 *
 * Exit codes: 0 = all pass / update ok, 1 = at least one scene differs, 2 = setup error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/glcorearb.h>

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "nanovg.h"
#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg_gl.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define SCENE_W 400
#define SCENE_H 300

#define GOLDEN_MAGIC "NVGGLD01"
#define GOLDEN_MAGIC_LEN 8

static NVGcontext* g_vg = NULL;
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;

static int g_imgPattern = 0;   /* procedural 64x64 test image */
static int g_imgChecker = 0;   /* procedural 32x32 repeatable checker */

static void die(const char* msg) {
	fprintf(stderr, "error: %s\n", msg);
	exit(2);
}

static void mkdir_if_needed(const char* path) {
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "error: could not create directory '%s'\n", path);
		exit(2);
	}
}

/* ------------------------------------------------------------------ */
/* EGL / GL setup                                                      */
/* ------------------------------------------------------------------ */

static void egl_init(void) {
	EGLConfig cfg;
	EGLint n = 0;

	/* Force software rendering so results do not depend on the host GPU. */
	setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
	setenv("GALLIUM_DRIVER", "llvmpipe", 0);

	g_dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if (g_dpy == EGL_NO_DISPLAY) die("eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA) failed");
	if (!eglInitialize(g_dpy, NULL, NULL)) die("eglInitialize failed");
	if (!eglBindAPI(EGL_OPENGL_API)) die("eglBindAPI(EGL_OPENGL_API) failed");

	{
		EGLint cfgAttrs[] = {
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
			EGL_NONE
		};
		eglChooseConfig(g_dpy, cfgAttrs, &cfg, 1, &n);
	}
	{
		EGLint ctxAttrs[] = {
			EGL_CONTEXT_MAJOR_VERSION, 3,
			EGL_CONTEXT_MINOR_VERSION, 3,
			EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
			EGL_NONE
		};
		g_ctx = eglCreateContext(g_dpy, n ? cfg : NULL, EGL_NO_CONTEXT, ctxAttrs);
		if (g_ctx == EGL_NO_CONTEXT) /* fall back to whatever the driver gives us */
			g_ctx = eglCreateContext(g_dpy, n ? cfg : NULL, EGL_NO_CONTEXT, NULL);
	}
	if (g_ctx == EGL_NO_CONTEXT) die("eglCreateContext failed");
	if (!eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_ctx))
		die("eglMakeCurrent (surfaceless) failed");

	printf("GL_VENDOR:   %s\n", (const char*)glGetString(GL_VENDOR));
	printf("GL_RENDERER: %s\n", (const char*)glGetString(GL_RENDERER));
	printf("GL_VERSION:  %s\n", (const char*)glGetString(GL_VERSION));
}

/* ------------------------------------------------------------------ */
/* Test assets (font + procedural images, all fully deterministic)     */
/* ------------------------------------------------------------------ */

static void create_assets(const char* fontPath) {
	int x, y;
	static unsigned char pattern[64 * 64 * 4];
	static unsigned char checker[32 * 32 * 4];

	if (nvgCreateFont(g_vg, "sans", fontPath) == -1) {
		fprintf(stderr, "error: could not load font '%s'\n", fontPath);
		exit(2);
	}

	/* 64x64: xy-gradient + 8px checker + one half-transparent quadrant. */
	for (y = 0; y < 64; y++) {
		for (x = 0; x < 64; x++) {
			unsigned char* p = &pattern[(y * 64 + x) * 4];
			int check = ((x / 8) + (y / 8)) & 1;
			p[0] = (unsigned char)(x * 4);
			p[1] = (unsigned char)(y * 4);
			p[2] = (unsigned char)(check ? 255 : 32);
			p[3] = (unsigned char)((x >= 32 && y >= 32) ? 128 : 255);
		}
	}
	g_imgPattern = nvgCreateImageRGBA(g_vg, 64, 64, 0, pattern);
	if (g_imgPattern == 0) die("nvgCreateImageRGBA(pattern) failed");

	/* 32x32 two-color checker, repeat-wrapped. */
	for (y = 0; y < 32; y++) {
		for (x = 0; x < 32; x++) {
			unsigned char* p = &checker[(y * 32 + x) * 4];
			int check = ((x / 8) + (y / 8)) & 1;
			p[0] = check ? 255 : 16;
			p[1] = check ? 160 : 80;
			p[2] = check ? 0 : 160;
			p[3] = 255;
		}
	}
	g_imgChecker = nvgCreateImageRGBA(g_vg, 32, 32, NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY, checker);
	if (g_imgChecker == 0) die("nvgCreateImageRGBA(checker) failed");
}

/* ------------------------------------------------------------------ */
/* Scenes: 400x300, fixed coordinates, white background                */
/* ------------------------------------------------------------------ */

static void scene_fill(NVGcontext* vg, float w, float h) {
	NVGpaint p;
	(void)w; (void)h;

	/* Solid fills: rect, rounded rect, circle, ellipse. */
	nvgBeginPath(vg);
	nvgRect(vg, 10, 10, 60, 40);
	nvgFillColor(vg, nvgRGBf(0.2f, 0.4f, 0.8f));
	nvgFill(vg);

	nvgBeginPath(vg);
	nvgRoundedRect(vg, 80, 10, 60, 40, 8);
	nvgFillColor(vg, nvgRGBf(0.9f, 0.5f, 0.1f));
	nvgFill(vg);

	nvgBeginPath(vg);
	nvgCircle(vg, 180, 30, 22);
	nvgFillColor(vg, nvgRGBf(0.2f, 0.7f, 0.3f));
	nvgFill(vg);

	nvgBeginPath(vg);
	nvgEllipse(vg, 250, 30, 30, 18);
	nvgFillColor(vg, nvgRGBf(0.6f, 0.3f, 0.7f));
	nvgFill(vg);

	/* Path with a hole (even-odd style via winding). */
	nvgBeginPath(vg);
	nvgRect(vg, 300, 8, 80, 44);
	nvgCircle(vg, 340, 30, 14);
	nvgPathWinding(vg, NVG_HOLE);
	nvgFillColor(vg, nvgRGBf(0.8f, 0.2f, 0.2f));
	nvgFill(vg);

	/* Linear / radial / box gradients. */
	p = nvgLinearGradient(vg, 10, 75, 130, 75, nvgRGBf(0, 0, 1), nvgRGBAf(0, 0, 1, 0));
	nvgBeginPath(vg);
	nvgRect(vg, 10, 60, 120, 30);
	nvgFillPaint(vg, p);
	nvgFill(vg);

	p = nvgRadialGradient(vg, 200, 75, 5, 35, nvgRGBf(1, 1, 1), nvgRGBf(0, 0.5f, 0));
	nvgBeginPath(vg);
	nvgCircle(vg, 200, 75, 35);
	nvgFillPaint(vg, p);
	nvgFill(vg);

	p = nvgBoxGradient(vg, 260, 60, 80, 40, 10, 12, nvgRGBAf(0, 0, 0, 0.6f), nvgRGBAf(0, 0, 0, 0));
	nvgBeginPath(vg);
	nvgRoundedRect(vg, 260, 60, 80, 40, 10);
	nvgFillPaint(vg, p);
	nvgFill(vg);

	/* Overlapping translucent fills. */
	nvgFillColor(vg, nvgRGBAf(1, 0, 0, 0.5f));
	nvgBeginPath(vg);
	nvgCircle(vg, 60, 150, 25);
	nvgFill(vg);
	nvgFillColor(vg, nvgRGBAf(0, 1, 0, 0.5f));
	nvgBeginPath(vg);
	nvgCircle(vg, 85, 150, 25);
	nvgFill(vg);
	nvgFillColor(vg, nvgRGBAf(0, 0, 1, 0.5f));
	nvgBeginPath(vg);
	nvgCircle(vg, 72, 170, 25);
	nvgFill(vg);

	/* Bezier blob. */
	nvgBeginPath(vg);
	nvgMoveTo(vg, 200, 140);
	nvgBezierTo(vg, 250, 120, 280, 180, 330, 150);
	nvgBezierTo(vg, 340, 190, 260, 200, 220, 185);
	nvgClosePath(vg);
	nvgFillColor(vg, nvgRGBf(0.95f, 0.8f, 0.2f));
	nvgFill(vg);

	/* Star (self-intersecting, nonzero winding). */
	nvgBeginPath(vg);
	nvgMoveTo(vg, 60, 220);
	nvgLineTo(vg, 74, 256);
	nvgLineTo(vg, 112, 232);
	nvgLineTo(vg, 8, 232);
	nvgLineTo(vg, 46, 256);
	nvgClosePath(vg);
	nvgFillColor(vg, nvgRGBf(0.3f, 0.3f, 0.35f));
	nvgFill(vg);
}

static void scene_stroke(NVGcontext* vg, float w, float h) {
	int i;
	(void)w; (void)h;

	/* Line caps: butt / round / square. */
	nvgStrokeColor(vg, nvgRGBf(0.1f, 0.1f, 0.1f));
	nvgStrokeWidth(vg, 12);
	nvgLineCap(vg, NVG_BUTT);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 30, 24);
	nvgLineTo(vg, 140, 24);
	nvgStroke(vg);
	nvgLineCap(vg, NVG_ROUND);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 30, 48);
	nvgLineTo(vg, 140, 48);
	nvgStroke(vg);
	nvgLineCap(vg, NVG_SQUARE);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 30, 72);
	nvgLineTo(vg, 140, 72);
	nvgStroke(vg);
	nvgLineCap(vg, NVG_BUTT);

	/* Line joins: miter / round / bevel. */
	nvgStrokeWidth(vg, 10);
	nvgStrokeColor(vg, nvgRGBf(0.7f, 0.2f, 0.2f));
	nvgLineJoin(vg, NVG_MITER);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 180, 70);
	nvgLineTo(vg, 210, 20);
	nvgLineTo(vg, 240, 70);
	nvgStroke(vg);
	nvgLineJoin(vg, NVG_ROUND);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 250, 70);
	nvgLineTo(vg, 280, 20);
	nvgLineTo(vg, 310, 70);
	nvgStroke(vg);
	nvgLineJoin(vg, NVG_BEVEL);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 320, 70);
	nvgLineTo(vg, 350, 20);
	nvgLineTo(vg, 380, 70);
	nvgStroke(vg);
	nvgLineJoin(vg, NVG_MITER);

	/* Stroke widths 1 / 2.5 / 5 / 9. */
	nvgStrokeColor(vg, nvgRGBf(0.2f, 0.3f, 0.6f));
	for (i = 0; i < 4; i++) {
		static const float widths[4] = { 1.0f, 2.5f, 5.0f, 9.0f };
		nvgStrokeWidth(vg, widths[i]);
		nvgBeginPath(vg);
		nvgMoveTo(vg, 20, 100.0f + i * 22.0f);
		nvgLineTo(vg, 160, 100.0f + i * 22.0f);
		nvgStroke(vg);
	}

	/* Stroked circle, rounded rect and open bezier. */
	nvgStrokeWidth(vg, 4);
	nvgStrokeColor(vg, nvgRGBf(0.1f, 0.5f, 0.4f));
	nvgBeginPath(vg);
	nvgCircle(vg, 230, 130, 30);
	nvgStroke(vg);

	nvgStrokeColor(vg, nvgRGBf(0.5f, 0.2f, 0.6f));
	nvgBeginPath(vg);
	nvgRoundedRect(vg, 290, 105, 80, 50, 12);
	nvgStroke(vg);

	nvgStrokeWidth(vg, 6);
	nvgStrokeColor(vg, nvgRGBf(0.8f, 0.4f, 0.1f));
	nvgLineCap(vg, NVG_ROUND);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 30, 230);
	nvgBezierTo(vg, 90, 190, 150, 270, 210, 230);
	nvgStroke(vg);
	nvgLineCap(vg, NVG_BUTT);

	/* Translucent overlapping strokes. */
	nvgStrokeWidth(vg, 14);
	nvgStrokeColor(vg, nvgRGBAf(1, 0, 0, 0.4f));
	nvgBeginPath(vg);
	nvgMoveTo(vg, 250, 200);
	nvgLineTo(vg, 370, 260);
	nvgStroke(vg);
	nvgStrokeColor(vg, nvgRGBAf(0, 0, 1, 0.4f));
	nvgBeginPath(vg);
	nvgMoveTo(vg, 250, 260);
	nvgLineTo(vg, 370, 200);
	nvgStroke(vg);
}

static void scene_text(NVGcontext* vg, float w, float h) {
	(void)w; (void)h;

	nvgFontFace(vg, "sans");
	nvgFillColor(vg, nvgRGBf(0.05f, 0.05f, 0.05f));

	/* Baselines for alignment reference. */
	nvgStrokeColor(vg, nvgRGBf(0.85f, 0.85f, 0.85f));
	nvgStrokeWidth(vg, 1);
	nvgBeginPath(vg);
	nvgMoveTo(vg, 0, 60);
	nvgLineTo(vg, 400, 60);
	nvgMoveTo(vg, 0, 110);
	nvgLineTo(vg, 400, 110);
	nvgMoveTo(vg, 0, 140);
	nvgLineTo(vg, 400, 140);
	nvgStroke(vg);

	nvgFontSize(vg, 48.0f);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
	nvgText(vg, 10, 60, "NanoVG", NULL);

	nvgFontSize(vg, 18.0f);
	nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
	nvgText(vg, 200, 110, "center aligned", NULL);

	nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
	nvgText(vg, 390, 140, "right aligned", NULL);

	/* Letter spacing. */
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
	nvgTextLetterSpacing(vg, 4.0f);
	nvgText(vg, 10, 180, "spaced out", NULL);
	nvgTextLetterSpacing(vg, 0.0f);

	/* Blurred text. */
	nvgFontBlur(vg, 2.0f);
	nvgFillColor(vg, nvgRGBf(0.2f, 0.3f, 0.6f));
	nvgText(vg, 200, 180, "soft", NULL);
	nvgFontBlur(vg, 0.0f);

	/* Colored + translucent text. */
	nvgFillColor(vg, nvgRGBAf(0.8f, 0.2f, 0.2f, 0.7f));
	nvgFontSize(vg, 24.0f);
	nvgText(vg, 10, 215, "translucent 0.7", NULL);

	/* Boxed paragraph. */
	nvgFillColor(vg, nvgRGBf(0.15f, 0.15f, 0.15f));
	nvgFontSize(vg, 13.0f);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
	nvgTextBox(vg, 10, 235, 220, "The quick brown fox jumps over the lazy dog. 0123456789", NULL);

	/* Multi-line with explicit line height. */
	nvgTextLineHeight(vg, 1.4f);
	nvgTextBox(vg, 250, 235, 140, "line one\nline two\nline three", NULL);
	nvgTextLineHeight(vg, 1.0f);
}

static void scene_image(NVGcontext* vg, float w, float h) {
	NVGpaint p;
	(void)w; (void)h;

	/* Plain draw, natural size. */
	p = nvgImagePattern(vg, 10, 10, 64, 64, 0, g_imgPattern, 1.0f);
	nvgBeginPath(vg);
	nvgRect(vg, 10, 10, 64, 64);
	nvgFillPaint(vg, p);
	nvgFill(vg);

	/* Scaled up (stretched). */
	p = nvgImagePattern(vg, 90, 10, 128, 64, 0, g_imgPattern, 1.0f);
	nvgBeginPath(vg);
	nvgRect(vg, 90, 10, 128, 64);
	nvgFillPaint(vg, p);
	nvgFill(vg);

	/* Rotated pattern. */
	p = nvgImagePattern(vg, 280, 40, 64, 64, 0.5f, g_imgPattern, 1.0f);
	nvgBeginPath(vg);
	nvgRect(vg, 240, 10, 80, 80);
	nvgFillPaint(vg, p);
	nvgFill(vg);

	/* Half-alpha image over a colored backdrop. */
	nvgBeginPath(vg);
	nvgRect(vg, 10, 100, 64, 64);
	nvgFillColor(vg, nvgRGBf(0, 0.6f, 0.6f));
	nvgFill(vg);
	p = nvgImagePattern(vg, 10, 100, 64, 64, 0, g_imgPattern, 0.5f);
	nvgBeginPath(vg);
	nvgRect(vg, 10, 100, 64, 64);
	nvgFillPaint(vg, p);
	nvgFill(vg);

	/* Repeating checker pattern over a large rounded rect. */
	p = nvgImagePattern(vg, 100, 110, 32, 32, 0, g_imgChecker, 1.0f);
	nvgBeginPath(vg);
	nvgRoundedRect(vg, 100, 110, 220, 70, 10);
	nvgFillPaint(vg, p);
	nvgFill(vg);

	/* Image filled into a circle path. */
	p = nvgImagePattern(vg, 340, 100, 64, 64, 0, g_imgPattern, 1.0f);
	nvgBeginPath(vg);
	nvgCircle(vg, 372, 132, 32);
	nvgFillPaint(vg, p);
	nvgFill(vg);

	/* Image under a transform. */
	nvgSave(vg);
	nvgTranslate(vg, 60, 240);
	nvgRotate(vg, 0.35f);
	nvgScale(vg, 1.5f, 0.8f);
	p = nvgImagePattern(vg, -32, -32, 64, 64, 0, g_imgPattern, 0.9f);
	nvgBeginPath(vg);
	nvgRect(vg, -32, -32, 64, 64);
	nvgFillPaint(vg, p);
	nvgFill(vg);
	nvgRestore(vg);
}

static void scene_clip(NVGcontext* vg, float w, float h) {
	int i, j;
	NVGpaint p;
	(void)w; (void)h;

	/* Backdrop so clip boundaries are visible. */
	p = nvgLinearGradient(vg, 0, 0, 400, 300, nvgRGBf(0.9f, 0.9f, 0.95f), nvgRGBf(0.7f, 0.75f, 0.8f));
	nvgBeginPath(vg);
	nvgRect(vg, 0, 0, 400, 300);
	nvgFillPaint(vg, p);
	nvgFill(vg);

	/* Axis-aligned scissor: only circles inside (40,40,150,100) survive. */
	nvgSave(vg);
	nvgScissor(vg, 40, 40, 150, 100);
	for (j = 0; j < 5; j++)
		for (i = 0; i < 8; i++) {
			nvgBeginPath(vg);
			nvgCircle(vg, 20.0f + i * 30.0f, 30.0f + j * 30.0f, 12);
			nvgFillColor(vg, nvgRGBf(0.2f + 0.1f * i, 0.3f, 0.8f - 0.1f * j));
			nvgFill(vg);
		}
	nvgRestore(vg);

	/* Rotated scissor with diagonal stripes. */
	nvgSave(vg);
	nvgTranslate(vg, 290, 90);
	nvgRotate(vg, 0.4f);
	nvgScissor(vg, -60, -40, 120, 80);
	nvgStrokeWidth(vg, 8);
	nvgStrokeColor(vg, nvgRGBf(0.7f, 0.2f, 0.5f));
	for (i = -8; i < 9; i++) {
		nvgBeginPath(vg);
		nvgMoveTo(vg, -100.0f + i * 16.0f, -80);
		nvgLineTo(vg, -60.0f + i * 16.0f, 80);
		nvgStroke(vg);
	}
	nvgRestore(vg);

	/* Intersected scissor: rect AND rotated rect. */
	nvgSave(vg);
	nvgScissor(vg, 60, 180, 200, 90);
	nvgSave(vg);
	nvgTranslate(vg, 160, 225);
	nvgRotate(vg, -0.3f);
	nvgIntersectScissor(vg, -80, -30, 160, 60);
	nvgRestore(vg);
	p = nvgRadialGradient(vg, 160, 225, 10, 120, nvgRGBf(1, 1, 0.4f), nvgRGBf(0.8f, 0.1f, 0.1f));
	nvgBeginPath(vg);
	nvgRect(vg, 0, 150, 400, 150);
	nvgFillPaint(vg, p);
	nvgFill(vg);
	nvgRestore(vg);

	/* Scissor under scale transform. */
	nvgSave(vg);
	nvgTranslate(vg, 330, 220);
	nvgScale(vg, 1.2f, 0.7f);
	nvgScissor(vg, -40, -30, 80, 60);
	nvgBeginPath(vg);
	nvgCircle(vg, 0, 0, 45);
	nvgFillColor(vg, nvgRGBf(0.1f, 0.5f, 0.3f));
	nvgFill(vg);
	nvgRestore(vg);
}

static void scene_transform(NVGcontext* vg, float w, float h) {
	int i;
	(void)w; (void)h;

	/* Row of increasingly rotated squares around their centers. */
	for (i = 0; i < 5; i++) {
		nvgSave(vg);
		nvgTranslate(vg, 50.0f + i * 75.0f, 50);
		nvgRotate(vg, i * 0.3f);
		nvgBeginPath(vg);
		nvgRect(vg, -18, -18, 36, 36);
		nvgFillColor(vg, nvgRGBf(0.15f * i, 0.2f, 0.9f - 0.15f * i));
		nvgFill(vg);
		nvgRestore(vg);
	}

	/* Non-uniform scale applied to a circle. */
	nvgSave(vg);
	nvgTranslate(vg, 70, 140);
	nvgScale(vg, 2.0f, 0.6f);
	nvgBeginPath(vg);
	nvgCircle(vg, 0, 0, 25);
	nvgFillColor(vg, nvgRGBf(0.8f, 0.5f, 0.1f));
	nvgFill(vg);
	nvgRestore(vg);

	/* Skew. */
	nvgSave(vg);
	nvgTranslate(vg, 200, 120);
	nvgSkewX(vg, 0.4f);
	nvgBeginPath(vg);
	nvgRect(vg, -25, -20, 50, 40);
	nvgFillColor(vg, nvgRGBf(0.3f, 0.6f, 0.8f));
	nvgFill(vg);
	nvgRestore(vg);

	nvgSave(vg);
	nvgTranslate(vg, 300, 130);
	nvgSkewY(vg, -0.3f);
	nvgBeginPath(vg);
	nvgRect(vg, -25, -20, 50, 40);
	nvgFillColor(vg, nvgRGBf(0.5f, 0.8f, 0.3f));
	nvgFill(vg);
	nvgRestore(vg);

	/* Nested transforms: translate -> rotate -> scale, stroked path. */
	nvgSave(vg);
	nvgTranslate(vg, 100, 230);
	nvgRotate(vg, 0.6f);
	nvgScale(vg, 1.3f, 1.3f);
	nvgStrokeWidth(vg, 3);
	nvgStrokeColor(vg, nvgRGBf(0.6f, 0.1f, 0.3f));
	nvgBeginPath(vg);
	nvgMoveTo(vg, -30, 0);
	nvgLineTo(vg, 0, -25);
	nvgLineTo(vg, 30, 0);
	nvgLineTo(vg, 0, 25);
	nvgClosePath(vg);
	nvgStroke(vg);
	nvgRestore(vg);

	/* Text under rotation. */
	nvgSave(vg);
	nvgTranslate(vg, 250, 240);
	nvgRotate(vg, -0.25f);
	nvgFontFace(vg, "sans");
	nvgFontSize(vg, 22.0f);
	nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, nvgRGBf(0.1f, 0.1f, 0.2f));
	nvgText(vg, 0, 0, "rotated", NULL);
	nvgRestore(vg);

	/* Reset transform: rect must land at absolute coords regardless. */
	nvgSave(vg);
	nvgTranslate(vg, 500, 500);
	nvgRotate(vg, 1.0f);
	nvgResetTransform(vg);
	nvgBeginPath(vg);
	nvgRect(vg, 350, 220, 30, 30);
	nvgFillColor(vg, nvgRGBf(0.2f, 0.2f, 0.2f));
	nvgFill(vg);
	nvgRestore(vg);
}

static void scene_combo(NVGcontext* vg, float w, float h) {
	NVGpaint p;
	(void)w; (void)h;

	/* Card panel with box-gradient background. */
	p = nvgBoxGradient(vg, 40, 30, 320, 230, 12, 16, nvgRGBf(0.95f, 0.95f, 0.97f), nvgRGBf(0.75f, 0.78f, 0.85f));
	nvgBeginPath(vg);
	nvgRoundedRect(vg, 40, 30, 320, 230, 12);
	nvgFillPaint(vg, p);
	nvgFill(vg);
	nvgStrokeWidth(vg, 2);
	nvgStrokeColor(vg, nvgRGBf(0.3f, 0.32f, 0.4f));
	nvgStroke(vg);

	/* Clipped image thumbnail in the corner. */
	nvgSave(vg);
	nvgScissor(vg, 56, 46, 72, 72);
	p = nvgImagePattern(vg, 56, 46, 72, 72, 0.2f, g_imgPattern, 1.0f);
	nvgBeginPath(vg);
	nvgRect(vg, 56, 46, 72, 72);
	nvgFillPaint(vg, p);
	nvgFill(vg);
	nvgRestore(vg);
	nvgBeginPath(vg);
	nvgRoundedRect(vg, 56, 46, 72, 72, 6);
	nvgStrokeWidth(vg, 1.5f);
	nvgStrokeColor(vg, nvgRGBf(0.2f, 0.2f, 0.25f));
	nvgStroke(vg);

	/* Title + body text. */
	nvgFontFace(vg, "sans");
	nvgFillColor(vg, nvgRGBf(0.1f, 0.1f, 0.15f));
	nvgFontSize(vg, 20.0f);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
	nvgText(vg, 140, 70, "Combined scene", NULL);
	nvgFontSize(vg, 12.0f);
	nvgFillColor(vg, nvgRGBf(0.3f, 0.3f, 0.35f));
	nvgTextBox(vg, 140, 84, 200, "fill + stroke + text + image + clip + transform in one frame.", NULL);

	/* Gradient button with stroked border. */
	p = nvgLinearGradient(vg, 56, 150, 56, 190, nvgRGBf(0.3f, 0.6f, 0.95f), nvgRGBf(0.15f, 0.35f, 0.75f));
	nvgBeginPath(vg);
	nvgRoundedRect(vg, 56, 150, 110, 36, 18);
	nvgFillPaint(vg, p);
	nvgFill(vg);
	nvgStrokeWidth(vg, 1);
	nvgStrokeColor(vg, nvgRGBf(0.1f, 0.2f, 0.5f));
	nvgStroke(vg);
	nvgFillColor(vg, nvgRGBf(1, 1, 1));
	nvgFontSize(vg, 14.0f);
	nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
	nvgText(vg, 111, 169, "Button", NULL);

	/* Translucent transformed group. */
	nvgSave(vg);
	nvgTranslate(vg, 280, 190);
	nvgRotate(vg, 0.3f);
	nvgGlobalAlpha(vg, 0.6f);
	nvgBeginPath(vg);
	nvgRect(vg, -40, -20, 80, 40);
	nvgFillColor(vg, nvgRGBf(0.9f, 0.3f, 0.3f));
	nvgFill(vg);
	nvgBeginPath(vg);
	nvgCircle(vg, 30, 0, 22);
	nvgFillColor(vg, nvgRGBf(0.2f, 0.7f, 0.4f));
	nvgFill(vg);
	nvgRestore(vg);
	nvgGlobalAlpha(vg, 1.0f);
}

/* ------------------------------------------------------------------ */
/* Rendering + golden compare                                          */
/* ------------------------------------------------------------------ */

struct Scene {
	const char* name;
	void (*draw)(NVGcontext* vg, float w, float h);
};

static const struct Scene g_scenes[] = {
	{ "fill_basic",      scene_fill },
	{ "stroke_basic",    scene_stroke },
	{ "text_basic",      scene_text },
	{ "image_basic",     scene_image },
	{ "clip_scissor",    scene_clip },
	{ "transform",       scene_transform },
	{ "composite_combo", scene_combo },
};

/* Render one scene into a fresh FBO and read back top-down RGBA pixels. */
static void render_scene(const struct Scene* s, int w, int h, unsigned char* img) {
	GLuint fbo = 0, rbColor = 0, rbDS = 0;
	int stride = w * 4;
	int y;

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	glGenRenderbuffers(1, &rbColor);
	glBindRenderbuffer(GL_RENDERBUFFER, rbColor);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbColor);

	glGenRenderbuffers(1, &rbDS);
	glBindRenderbuffer(GL_RENDERBUFFER, rbDS);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbDS);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		die("framebuffer incomplete");

	glViewport(0, 0, w, h);
	glDisable(GL_DITHER);
	glClearColor(1, 1, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	nvgBeginFrame(g_vg, (float)w, (float)h, 1.0f);
	s->draw(g_vg, (float)w, (float)h);
	nvgEndFrame(g_vg);

	glFinish();
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glDeleteRenderbuffers(1, &rbColor);
	glDeleteRenderbuffers(1, &rbDS);

	/* GL reads bottom-up; store goldens top-down. */
	for (y = 0; y < h / 2; y++) {
		unsigned char* a = img + y * stride;
		unsigned char* b = img + (h - 1 - y) * stride;
		int x;
		for (x = 0; x < stride; x++) {
			unsigned char t = a[x];
			a[x] = b[x];
			b[x] = t;
		}
	}
}

static int write_golden(const char* path, int w, int h, const unsigned char* img) {
	FILE* f = fopen(path, "wb");
	uint32_t dims[2];
	if (!f) return -1;
	dims[0] = (uint32_t)w;
	dims[1] = (uint32_t)h;
	if (fwrite(GOLDEN_MAGIC, 1, GOLDEN_MAGIC_LEN, f) != GOLDEN_MAGIC_LEN ||
		fwrite(dims, sizeof(uint32_t), 2, f) != 2 ||
		fwrite(img, 1, (size_t)w * h * 4, f) != (size_t)w * h * 4) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

/* Returns 0 on success, -1 on IO/format error. */
static int read_golden(const char* path, int w, int h, unsigned char* img) {
	FILE* f = fopen(path, "rb");
	char magic[GOLDEN_MAGIC_LEN];
	uint32_t dims[2];
	if (!f) return -1;
	if (fread(magic, 1, GOLDEN_MAGIC_LEN, f) != GOLDEN_MAGIC_LEN ||
		memcmp(magic, GOLDEN_MAGIC, GOLDEN_MAGIC_LEN) != 0 ||
		fread(dims, sizeof(uint32_t), 2, f) != 2 ||
		dims[0] != (uint32_t)w || dims[1] != (uint32_t)h ||
		fread(img, 1, (size_t)w * h * 4, f) != (size_t)w * h * 4) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static void write_png(const char* path, int w, int h, const unsigned char* img) {
	if (!stbi_write_png(path, w, h, 4, img, w * 4))
		fprintf(stderr, "warning: could not write %s\n", path);
}

/* Compare golden vs actual, print localization stats, write diff artifacts. */
static int compare_scene(const char* name, int w, int h,
						 const unsigned char* golden, const unsigned char* actual,
						 const char* outDir) {
	size_t npix = (size_t)w * h;
	size_t i;
	uint64_t ndiff = 0;
	int maxdelta = 0;
	int minx = w, miny = h, maxx = -1, maxy = -1;
	unsigned char* diff = (unsigned char*)malloc(npix * 4);
	int printed = 0;

	for (i = 0; i < npix; i++) {
		const unsigned char* g = golden + i * 4;
		const unsigned char* a = actual + i * 4;
		int d0 = abs((int)g[0] - (int)a[0]);
		int d1 = abs((int)g[1] - (int)a[1]);
		int d2 = abs((int)g[2] - (int)a[2]);
		int d3 = abs((int)g[3] - (int)a[3]);
		int dmax = d0 > d1 ? d0 : d1;
		if (d2 > dmax) dmax = d2;
		if (d3 > dmax) dmax = d3;
		if (dmax > 0) {
			int x = (int)(i % (size_t)w);
			int y = (int)(i / (size_t)w);
			ndiff++;
			if (dmax > maxdelta) maxdelta = dmax;
			if (x < minx) minx = x;
			if (x > maxx) maxx = x;
			if (y < miny) miny = y;
			if (y > maxy) maxy = y;
			if (printed < 5) {
				printf("  diff at (%d,%d): golden=%02x%02x%02x%02x actual=%02x%02x%02x%02x\n",
					   x, y, g[0], g[1], g[2], g[3], a[0], a[1], a[2], a[3]);
				printed++;
			}
			/* Amplified per-channel diff on black, floor so 1-off is visible. */
			diff[i * 4 + 0] = (unsigned char)(d0 ? (d0 * 4 > 255 ? 255 : (d0 * 4 < 64 ? 64 : d0 * 4)) : 0);
			diff[i * 4 + 1] = (unsigned char)(d1 ? (d1 * 4 > 255 ? 255 : (d1 * 4 < 64 ? 64 : d1 * 4)) : 0);
			diff[i * 4 + 2] = (unsigned char)(d2 ? (d2 * 4 > 255 ? 255 : (d2 * 4 < 64 ? 64 : d2 * 4)) : 0);
			diff[i * 4 + 3] = 255;
		} else {
			diff[i * 4 + 0] = diff[i * 4 + 1] = diff[i * 4 + 2] = 0;
			diff[i * 4 + 3] = 255;
		}
	}

	if (ndiff == 0) {
		free(diff);
		return 0;
	}

	printf("FAIL %s: %llu/%zu pixels differ (%.3f%%), max channel delta %d, bbox (%d,%d)-(%d,%d)\n",
		   name, (unsigned long long)ndiff, npix, 100.0 * (double)ndiff / (double)npix,
		   maxdelta, minx, miny, maxx, maxy);

	if (outDir) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s.golden.png", outDir, name);
		write_png(path, w, h, golden);
		snprintf(path, sizeof(path), "%s/%s.actual.png", outDir, name);
		write_png(path, w, h, actual);
		snprintf(path, sizeof(path), "%s/%s.diff.png", outDir, name);
		write_png(path, w, h, diff);
		printf("  wrote %s/%s.{golden,actual,diff}.png\n", outDir, name);
	}
	free(diff);
	return 1;
}

static void usage(const char* argv0) {
	fprintf(stderr,
		"usage: %s test|update [--golden DIR] [--out DIR] [--font FILE] [filter]\n"
		"  test    compare rendered scenes against goldens (exit 1 on any diff)\n"
		"  update  regenerate goldens and .png previews\n"
		"  filter  optional substring to run only matching scenes\n", argv0);
	exit(2);
}

int main(int argc, char** argv) {
	const char* mode = NULL;
	const char* goldenDir = "golden";
	const char* outDir = "out";
	const char* fontPath = "../example/Roboto-Regular.ttf";
	const char* filter = NULL;
	int i;
	int npass = 0, nfail = 0, nrun = 0;
	unsigned char* actual = NULL;
	unsigned char* golden = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "test") == 0 || strcmp(argv[i], "update") == 0) {
			if (mode) usage(argv[0]);
			mode = argv[i];
		} else if (strcmp(argv[i], "--golden") == 0 && i + 1 < argc) {
			goldenDir = argv[++i];
		} else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
			outDir = argv[++i];
		} else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
			fontPath = argv[++i];
		} else if (argv[i][0] != '-') {
			filter = argv[i];
		} else {
			usage(argv[0]);
		}
	}
	if (!mode) usage(argv[0]);

	mkdir_if_needed(goldenDir);
	mkdir_if_needed(outDir);

	egl_init();
	g_vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
	if (!g_vg) die("nvgCreateGL3 failed");
	create_assets(fontPath);

	actual = (unsigned char*)malloc(SCENE_W * SCENE_H * 4);
	golden = (unsigned char*)malloc(SCENE_W * SCENE_H * 4);
	if (!actual || !golden) die("out of memory");

	for (i = 0; i < (int)(sizeof(g_scenes) / sizeof(g_scenes[0])); i++) {
		const struct Scene* s = &g_scenes[i];
		char path[1024];
		if (filter && !strstr(s->name, filter))
			continue;
		nrun++;

		render_scene(s, SCENE_W, SCENE_H, actual);
		snprintf(path, sizeof(path), "%s/%s.nvg", goldenDir, s->name);

		if (strcmp(mode, "update") == 0) {
			char pngpath[1024];
			if (write_golden(path, SCENE_W, SCENE_H, actual) != 0) {
				fprintf(stderr, "error: could not write %s\n", path);
				nfail++;
				continue;
			}
			snprintf(pngpath, sizeof(pngpath), "%s/%s.png", goldenDir, s->name);
			write_png(pngpath, SCENE_W, SCENE_H, actual);
			printf("updated %s (%dx%d)\n", path, SCENE_W, SCENE_H);
		} else {
			if (read_golden(path, SCENE_W, SCENE_H, golden) != 0) {
				printf("FAIL %s: missing or invalid golden %s (run 'make update')\n", s->name, path);
				nfail++;
				continue;
			}
			if (compare_scene(s->name, SCENE_W, SCENE_H, golden, actual, outDir) == 0) {
				printf("PASS %s\n", s->name);
				npass++;
			} else {
				nfail++;
			}
		}
	}

	if (nrun == 0)
		printf("no scenes matched\n");
	else if (strcmp(mode, "update") == 0)
		printf("UPDATE: %d scene(s) written, %d error(s)\n", nrun - nfail, nfail);
	else
		printf("SUMMARY: %d passed, %d failed, %d total\n", npass, nfail, nrun);

	free(actual);
	free(golden);
	nvgDeleteGL3(g_vg);
	eglDestroyContext(g_dpy, g_ctx);
	eglTerminate(g_dpy);
	return nfail ? 1 : 0;
}
