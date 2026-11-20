# Offscreen pixel-regression tests for NanoVG (headless EGL + software GL).
#
#   make test    - build the runner and compare all scenes against golden/
#   make update  - build the runner and (re)generate golden/ images
#   make clean   - remove the binary and the out/ diff-artifact dir
#
# Toolchain can be overridden from the command line, e.g.:
#   make CC=/path/to/gcc \
#        EGL_CFLAGS=-I/path/to/prefix/usr/include \
#        EGL_LIBS="/path/to/libEGL.so.1 /path/to/libGL.so.1 -Wl,-rpath-link,/path/to" \
#        test

CC          ?= cc
CFLAGS      ?= -O2 -g
EGL_CFLAGS  ?=
EGL_LIBS    ?= -lEGL -lGL

ROOT        := ..
INCLUDES    := -I$(ROOT)/src -I$(ROOT)/example $(EGL_CFLAGS)
BIN         := nvgtest_render
GOLDEN      := golden
OUT         := out
FONT        := $(ROOT)/example/Roboto-Regular.ttf

# Software rasterizer -> results do not depend on the host GPU.
RUNENV      := LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe

all: test

$(BIN): nvgtest_render.c $(ROOT)/src/nanovg.c $(ROOT)/src/nanovg.h $(ROOT)/src/nanovg_gl.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ nvgtest_render.c $(ROOT)/src/nanovg.c $(EGL_LIBS) -lm

test: $(BIN)
	$(RUNENV) ./$(BIN) test --golden $(GOLDEN) --out $(OUT) --font $(FONT)

update: $(BIN)
	@mkdir -p $(GOLDEN)
	$(RUNENV) ./$(BIN) update --golden $(GOLDEN) --out $(OUT) --font $(FONT)

clean:
	rm -f $(BIN)
	rm -rf $(OUT)

.PHONY: all test update clean
