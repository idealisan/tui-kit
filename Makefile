# termrenderer Makefile
#
# Cross-platform static build.
#   make                 - build (Linux/macOS)
#   make STATIC=1        - fully static binary (no runtime deps)
#   make CC=x86_64-w64-mingw32-gcc OS=windows - cross-compile for Windows
#
# Dependencies (vendored/static):
#   libvterm.a (MIT)   - terminal emulator core
#   FreeType  (FTL)    - font rasterisation
#   libpng    (libpng) - PNG encoding
#   zlib/bz2/brotli    - static libs required by FreeType (or --font with
#                        a minimal build). STATIC=1 links them statically.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -Iinclude -Isrc
LDFLAGS += -Llib

OS ?= posix

# --- platform selection -------------------------------------------------
ifeq ($(OS),windows)
  PLATFORM_SRC = src/platform_windows.c
  EXE = termrenderer.exe
  LIBS = -lws2_32 -luser32
  CFLAGS += -DWIN32_LEAN_AND_MEAN
else
  PLATFORM_SRC = src/platform_posix.c
  EXE = termrenderer
  LIBS = -lutil
endif

# --- dependency libs ----------------------------------------------------
# pkg-config gives the link flags; with STATIC=1 we use --static for libs
# but still need the include paths for headers.
ifeq ($(STATIC),1)
  CFLAGS += $(shell pkg-config --cflags freetype2 libpng 2>/dev/null)
  LDFLAGS += -static
  DEP_LIBS := $(shell pkg-config --static --libs freetype2 libpng 2>/dev/null)
else
  CFLAGS += $(shell pkg-config --cflags freetype2 libpng 2>/dev/null)
  DEP_LIBS := $(shell pkg-config --libs freetype2 libpng 2>/dev/null)
endif

SRCS = src/main.c src/render.c src/png.c $(PLATFORM_SRC)
OBJS = $(SRCS:.c=.o)

all: $(EXE)

$(EXE): $(OBJS) lib/libvterm.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) lib/libvterm.a $(DEP_LIBS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f termrenderer termrenderer.exe $(OBJS)

.PHONY: all clean
