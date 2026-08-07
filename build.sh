# Build a self-contained termrenderer binary.

set -e

UNAME="$(uname -s 2>/dev/null || echo Windows)"

case "$UNAME" in
  Linux|Darwin)
    # Statically link everything we can for a portable binary.
    # If the static libs are missing (e.g. no .a for freetype/png), fall
    # back to a dynamic build and print a note.
    if pkg-config --static --libs freetype2 libpng >/dev/null 2>&1; then
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
