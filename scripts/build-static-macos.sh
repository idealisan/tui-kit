#!/bin/sh
# build-static-macos.sh - build a fully self-contained termrenderer for macOS.
#
# freetype, libpng and zlib are fetched and compiled from source as static
# archives (.a) and linked directly into the binary. Only the system libc
# (libSystem / libutil) stays dynamic, because macOS does not support fully
# static user-space executables (there is no crt0.o). The result is a single
# binary with no Homebrew / third-party dylib dependencies.
#
# Usage:
#   scripts/build-static-macos.sh [PREFIX]
# PREFIX defaults to ./extlibs (gitignored). The built binary is ./termrenderer
# at the repository root.

set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

PREFIX="${1:-$REPO/extlibs}"
BUILD="$REPO/extlibs-build"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

ZLIB_VER=1.3.1
LIBPNG_VER=1.6.43
FREETYPE_VER=2.13.3

ZLIB_URL="https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz"
LIBPNG_URL="https://github.com/pnggroup/libpng/archive/refs/tags/v$LIBPNG_VER.tar.gz"
FREETYPE_URL="https://downloads.sourceforge.net/project/freetype/freetype2/$FREETYPE_VER/freetype-$FREETYPE_VER.tar.gz"

fetch() {
    url="$1"; out="$2"
    if [ -f "$out" ]; then return; fi
    echo "==> downloading $url"
    curl -fsSL --retry 3 -o "$out" "$url"
}

mkdir -p "$PREFIX" "$BUILD"
cd "$BUILD"

# --- zlib ---------------------------------------------------------------
fetch "$ZLIB_URL" zlib.tar.gz
rm -rf "zlib-$ZLIB_VER"; tar xzf zlib.tar.gz
echo "==> building zlib (static)"
cmake -S "zlib-$ZLIB_VER" -B zlib-build \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build zlib-build -j"$JOBS"
cmake --install zlib-build

# --- libpng -------------------------------------------------------------
fetch "$LIBPNG_URL" libpng.tar.gz
rm -rf "libpng-$LIBPNG_VER"; tar xzf libpng.tar.gz
echo "==> building libpng (static)"
cmake -S "libpng-$LIBPNG_VER" -B libpng-build \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DZLIB_ROOT="$PREFIX" \
    -DPNG_TESTS=OFF -DPNG_TOOLS=OFF >/dev/null
cmake --build libpng-build -j"$JOBS"
cmake --install libpng-build

# --- freetype -----------------------------------------------------------
# PNG support MUST stay on: FreeType decodes color-emoji (CBDT/CBLC) glyphs
# via libpng. We only drop brotli/bzip2/harfbuzz, which are not needed.
fetch "$FREETYPE_URL" freetype.tar.gz
rm -rf "freetype-$FREETYPE_VER"; tar xzf freetype.tar.gz
echo "==> building freetype (static, PNG enabled)"
cmake -S "freetype-$FREETYPE_VER" -B freetype-build \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DZLIB_ROOT="$PREFIX" -DZLIB_LIBRARY="$PREFIX/lib/libz.a" -DZLIB_INCLUDE_DIR="$PREFIX/include" \
    -DPNG_LIBRARY="$PREFIX/lib/libpng16.a" -DPNG_PNG_INCLUDE_DIR="$PREFIX/include" \
    -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_HARFBUZZ=ON >/dev/null
cmake --build freetype-build -j"$JOBS"
cmake --install freetype-build

# --- termrenderer --------------------------------------------------------
# We are still in $BUILD (extlibs-build) from the download/compile steps
# above, so cd back to the repo root where src/ and lib/ live.
cd "$REPO"
echo "==> building termrenderer (static link)"

# Build libvterm.a directly (do NOT call `make` recursively: when this script
# is invoked from `make static`, a nested make inherits the parent's jobserver
# fds and can fail spuriously). Compile the vendored sources and archive them.
VTERM_DIR="$REPO/third_party/libvterm"
VTERM_SRCS="encoding keyboard mouse parser pen screen state unicode vterm"
mkdir -p lib
for s in $VTERM_SRCS; do
    cc -O2 -I"$VTERM_DIR/include" -I"$VTERM_DIR/src" -c -o "lib/vterm_$s.o" \
        "$VTERM_DIR/src/$s.c"
done
ar rcs lib/libvterm.a lib/vterm_*.o

# Include paths are explicit: FreeType installs under freetype2/ and libpng
# under libpng16/. (We avoid pkg-config here so the script works the same
# under /bin/sh and zsh, and so no Homebrew include dir can sneak in.)
cc -O2 -Wall -Wextra \
    -I"$PREFIX/include/freetype2" -I"$PREFIX/include/libpng16" -I"$PREFIX/include" \
    -Iinclude -Isrc \
    -o termrenderer \
    src/main.c src/render.c src/png.c src/platform_posix.c \
    lib/libvterm.a \
    "$PREFIX/lib/libfreetype.a" "$PREFIX/lib/libpng16.a" "$PREFIX/lib/libz.a" \
    -lutil

echo "==> done: $(file termrenderer | sed 's/.*: //')"
echo "==> dependencies:"
otool -L termrenderer
