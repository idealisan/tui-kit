# Build a self-contained termrenderer binary.

set -e

UNAME="$(uname -s 2>/dev/null || echo Windows)"

STATIC_LIBS=0
for a in "$@"; do
  case "$a" in
    --static-libs) STATIC_LIBS=1 ;;
  esac
done

case "$UNAME" in
  Linux|Darwin)
    # On macOS a *fully* static executable is impossible (no crt0.o), so
    # `--static-libs` builds freetype/libpng/zlib from source as static .a
    # archives and links them in, leaving only the system libc dynamic.
    if [ "$STATIC_LIBS" -eq 1 ]; then
      if [ "$UNAME" = "Darwin" ]; then
        ./scripts/build-static-macos.sh
      else
        make STATIC=1
      fi
    elif pkg-config --static --libs freetype2 libpng >/dev/null 2>&1; then
      make STATIC=1
    else
      echo "note: static freetype/libpng not found; building dynamic" >&2
      make
    fi
    ;;
  MINGW*|MSYS*|CYGWIN*|Windows*)
    # Native Windows build with MinGW-w64.
    if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
      make OS=windows CC=x86_64-w64-mingw32-gcc EXE=termrenderer.exe \
           CFLAGS="-O2 -Wall -Wextra -Iinclude -Isrc"
    else
      echo "error: MinGW-w64 gcc not found" >&2
      exit 1
    fi
    ;;
  *)
    echo "error: unsupported platform: $UNAME" >&2
    exit 1
    ;;
esac

echo "build complete"
