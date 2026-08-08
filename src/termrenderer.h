/*
 * termrenderer - PTY + libvterm + bitmap renderer
 *
 * Runs a command in a pseudo-terminal, parses its output with libvterm,
 * then rasterizes the screen buffer to a PNG using FreeType + libpng.
 */

#ifndef TERMRENDERER_H
#define TERMRENDERER_H

#include <stdint.h>
#include <vterm.h>

#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80
#define CELL_PADDING 2

typedef struct {
    int rows;
    int cols;
} TermSize;

typedef struct {
    int width;
    int height;
} PixelSize;

/* render.c - font_paths is an ordered fallback list (first match wins) */
void *render_screen(VTerm *vt, TermSize *size, PixelSize *out,
                    const char **font_paths, int n_fonts, int font_px);

/* png.c */
int png_write(const char *path, void *rgba, int width, int height);

#endif
