# termrenderer

Run a command in a real pseudo-terminal and render the final screen to a PNG.
Architecture (terminal emulator without a GUI):

```
                  ┌─────────────┐
                  │   command   │   e.g. curl www.google.com
                  └──────┬──────┘
                         │ PTY / ConPTY
                 ┌───────▼───────┐
                 │   libvterm    │   parses VT escape sequences
                 └───────┬───────┘
                         │ cell state
              ┌──────────▼──────────┐
              │ bitmap renderer     │   FreeType glyph rasterisation
              │ (our own blitter)   │
              └──────────┬──────────┘
                         │ RGBA buffer
                 ┌───────▼───────┐
                 │     libpng    │
                 └───────┬───────┘
                         │
                        PNG
```

The terminal emulator core is [libvterm](https://github.com/neovim/libvterm)
(MIT). It parses VT/xterm output and reports cell-level changes; we take the
screen buffer and rasterize each glyph ourselves with FreeType. Nothing here
requires a display, so it is perfectly CI/headless friendly.

## Usage

```
./termrenderer [options] -- command [args...]
```

Options:

| Option        | Description                          | Default |
|---------------|--------------------------------------|---------|
| `--cols N`    | terminal columns                     | 80      |
| `--rows N`    | terminal rows                        | 24      |
| `--font PATH` | TrueType/OpenType font               | auto    |
| `--timeout MS`| max time to wait for output          | 5000    |
| `--output PATH` | output PNG path                    | out.png |
| `--fontsize N`  | glyph pixel size                   | 18      |

Example:

```
./termrenderer --cols 100 --rows 30 --timeout 15000 --fontsize 20 \
  --output google.png -- curl -sS -I https://www.google.com
```

## Build

### Linux / macOS

```
./build.sh            # static build (no runtime dependencies)
```

This produces a single static `termrenderer` binary. The bundled `lib/libvterm.a`
is compiled from libvterm; FreeType, libpng, zlib and bzip2 are linked
statically when their `.a` files are present (via `pkg-config --static`).

If you only have shared libraries, a dynamic build still works:

```
make
```

### Windows

`platform_windows.c` implements the same interface using the Windows Pseudo
Console (ConPTY, Win10 1809+). To cross-compile you need MinGW-w64 plus
Windows builds of FreeType, libpng and zlib available in the cross toolchain:

```
make OS=windows CC=x86_64-w64-mingw32-gcc EXE=termrenderer.exe \
     CFLAGS="-O2 -Wall -Wextra -Iinclude -Isrc"
```

To build *on* Windows with MSYS2, install `mingw-w64-x86_64-{gcc,freetype,libpng,zlib}`
then run `./build.sh`.

## Layout

```
lib/               vendored static libraries (libvterm.a)
include/           vendored headers (vterm.h)
src/
  main.c           CLI, orchestration
  platform.h       OS abstraction interface
  platform_posix.c PTY/process/font for Linux & macOS (forkpty/openpty)
  platform_windows.c ConPTY implementation
  render.c         bitmap renderer + FreeType rasterisation
  png.c            PNG encoder (libpng)
```

## Notes

- Vendored components keep their own permissive
  licenses (libvterm MIT, FreeType FTL, libpng/libpng-2.0, zlib).
- `vterm_set_utf8(vt, 1)` must be called **before** `vterm_obtain_screen()`,
  and `vterm_screen_reset()` must run once after creation — otherwise the
  state layer's encoding table is left uninitialised and the first text byte
  segfaults. See `main.c` for the correct order.
- Glyph coverage: full Unicode BMP via FreeType; wide/combining characters
  are handled by libvterm's wcwidth logic. Ligatures are not implemented.
