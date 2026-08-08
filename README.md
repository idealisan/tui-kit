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

This produces a single static `termrenderer` binary. The bundled libvterm source
in `third_party/libvterm/` is compiled on every build, so the archive always
matches the host architecture; FreeType, libpng, zlib and bzip2 are linked
statically when their `.a` files are present (via `pkg-config --static`).

If you only have shared libraries, a dynamic build still works:

```
make
```

#### Static build from source (macOS)

On macOS a *fully* static user-space executable is impossible — there is no
`crt0.o`, so `ld -static` fails. `./build.sh --static-libs` therefore closes the
gap the only way possible: it downloads **zlib**, **libpng** and **FreeType**,
compiles each from source as a static archive (`.a`) into `extlibs/` (gitignored),
and links those directly into the binary. Only the system libc
(`libSystem` / `libutil`) remains dynamic, so the result carries **no Homebrew or
other third-party dylib dependencies**.

```
./build.sh --static-libs      # or: make static
```

FreeType is built with PNG support **enabled** (`FT_CONFIG_OPTION_USE_PNG`):
it decodes color-emoji (CBDT/CBLC) glyphs through libpng, so dropping PNG would
silently break emoji rendering. Only brotli, bzip2 and harfbuzz are disabled
(they are not needed). The build script is
`scripts/build-static-macos.sh`; its vendored source and the built `extlibs/`
directory are excluded via `.gitignore`. The output binary is `termrenderer` at
the repository root (also matched by the `termrenderer-static` gitignore entry).

> Note: the Linux path uses `pkg-config --static` (or `make STATIC=1`) and can be
> fully static because glibc provides `crt0.o`. macOS cannot, hence the
> from-source approach above.

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
lib/               build-time output (libvterm.a, gitignored)
third_party/       vendored source (libvterm/)
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

- Vendored components keep their own permissive licenses (libvterm MIT,
  FreeType FTL, libpng/libpng-2.0, zlib).
- `vterm_set_utf8(vt, 1)` must be called **before** `vterm_obtain_screen()`,
  and `vterm_screen_reset()` must run once after creation — otherwise the
  state layer's encoding table is left uninitialised and the first text byte
  segfaults. See `main.c` for the correct order.
- Glyph coverage: full Unicode BMP via FreeType. Wide characters (e.g. CJK)
  and combining sequences are rendered by drawing the whole `cell.chars[]`
  run, sized to `cell.width` columns and centered per cell. This works only
  if the chosen font actually contains those glyphs — `font_width` is derived
  from the advance of the representative `M` glyph (not `max_advance`, which a
  proportional face can inflate with a single wide glyph), so a monospace
  Noto face yields ~1em cells where
  two columns fit a wide glyph. Combining marks are placed via simple
  centering, not true advance-based kerning. Ligatures are not implemented.
