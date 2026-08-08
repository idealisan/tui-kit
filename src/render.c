/*
 * render.c - bitmap renderer: walks the libvterm screen buffer and draws
 *            each cell's glyph (via FreeType) into an RGBA pixel buffer.
 *
 * Supports font fallback: a list of font files is tried in order, and the
 * first face that actually contains a given codepoint is used. This lets a
 * monospace primary font be combined with CJK, script-specific and color
 * emoji fallback fonts.
 *
 * Color bitmap glyphs (e.g. Noto Color Emoji, FreeType FT_PIXEL_MODE_BGRA)
 * are blended in their own colours; every other glyph is tinted with the
 * cell's foreground colour.
 *
 * Box-drawing characters (U+2500..U+257F) are rendered from the font like any
 * other glyph, but are fitted to the cell so borders connect: a horizontal
 * bar is stretched across the cell width, a vertical bar across its height,
 * and corners/crosses fill the cell. This is how a terminal aligns box art
 * regardless of the exact font metrics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "termrenderer.h"

#define MAX_FACES 32

typedef struct {
    uint32_t *pixels;   /* RGBA, packed as 0xAARRGGBB */
    int width;
    int height;
    FT_Face faces[MAX_FACES];
    int n_faces;
    int font_height;    /* pixels per row */
    int font_width;     /* pixels per column (from primary face) */
    int ascender;
    int baseline;       /* y offset of baseline inside a cell */
} Canvas;

static void canvas_set_pixel(Canvas *c, int x, int y, uint32_t rgba)
{
    if (x < 0 || y < 0 || x >= c->width || y >= c->height) {
        return;
    }
    c->pixels[(size_t)y * c->width + x] = rgba;
}

static uint32_t make_color(VTermScreen *screen, const VTermColor *col)
{
    VTermColor c = *col;
    vterm_screen_convert_color_to_rgb(screen, &c);
    return (0xFFu << 24) | ((uint32_t)c.rgb.red << 16) |
           ((uint32_t)c.rgb.green << 8) | (uint32_t)c.rgb.blue;
}

/* alpha-over blend: src packed as 0xAARRGGBB */
static uint32_t blend(uint32_t src, uint32_t dst)
{
    uint8_t a = (src >> 24) & 0xFF;
    if (a == 0) {
        return dst;
    }
    if (a == 0xFF) {
        return src;
    }
    uint8_t sa = a;
    uint8_t da = 0xFF - a;
    uint8_t sr = (src >> 16) & 0xFF;
    uint8_t sg = (src >> 8) & 0xFF;
    uint8_t sb = src & 0xFF;
    uint8_t dr = (dst >> 16) & 0xFF;
    uint8_t dg = (dst >> 8) & 0xFF;
    uint8_t db = dst & 0xFF;
    uint8_t r = (uint8_t)((sr * sa + dr * da) / 255);
    uint8_t g = (uint8_t)((sg * sa + dg * da) / 255);
    uint8_t b = (uint8_t)((sb * sa + db * da) / 255);
    return (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Box-drawing range (light, heavy, and arcs). */
static int is_box_drawing(uint32_t ch)
{
    return (ch >= 0x2500 && ch <= 0x257F);
}

/*
 * Draw a FreeType glyph bitmap into the cell at (cell_x, cell_y).
 *
 * `box` selects box-drawing fitting: the glyph is stretched to fill the cell
 * along the axis of its stroke (horizontal bar -> full width, vertical bar ->
 * full height, corners/crosses -> full cell) so adjacent cells connect. Normal
 * glyphs are only scaled down to fit and baseline-aligned.
 *
 * Tinted with `fg` for non-colour glyphs; colour (BGRA) glyphs use their own
 * pixels.
 */
static void draw_glyph(Canvas *c, FT_Bitmap *bmp, int cell_x, int cell_y,
                       int avail_w, int avail_h, int baseline, int bitmap_top,
                       int bitmap_left, uint32_t fg, int bold, int box)
{
    int w = (int)bmp->width;
    int h = (int)bmp->rows;
    if (w <= 0 || h <= 0 || bmp->pixel_mode == FT_PIXEL_MODE_NONE) {
        return;
    }

    int dw, dh, pen_x, pen_y;
    double sxf, syf;

    if (box) {
        /* Render the real box-drawing glyph at its natural size, positioned
         * with the font's own metrics (bitmap_left / bitmap_top). A proper
         * box font places every stroke on the cell grid, so adjacent cells
         * connect automatically -- there is no stretching or re-centering to
         * do. Stretching would move the junction arms off the bar positions
         * and break the borders. */
        dw = w; dh = h;
        pen_x = cell_x + bitmap_left;
        pen_y = cell_y + baseline - bitmap_top;
        sxf = syf = 1.0;
    } else {
        double s = 1.0;
        if (w > avail_w) s = (double)avail_w / (double)w;
        if (h > avail_h) s = (double)avail_h / (double)h;
        if (s > 1.0) s = 1.0;

        dw = (int)(w * s + 0.5);
        dh = (int)(h * s + 0.5);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;

        pen_x = cell_x + (avail_w - dw) / 2;
        if (s >= 1.0) {
            pen_y = cell_y + baseline - bitmap_top;
        } else {
            pen_y = cell_y + (avail_h - dh) / 2;
        }
        sxf = syf = 1.0 / s;
    }

    int is_color = (bmp->pixel_mode == FT_PIXEL_MODE_BGRA);
    int pitch = bmp->pitch;

    for (int dy = 0; dy < dh; dy++) {
        int sy = (int)(dy * syf);
        if (sy >= h) sy = h - 1;
        unsigned char *row = bmp->buffer + (size_t)sy * pitch;
        for (int dx = 0; dx < dw; dx++) {
            int sx = (int)(dx * sxf);
            if (sx >= w) sx = w - 1;

            uint32_t px;
            if (is_color) {
                int idx = sx * 4;
                uint8_t b = row[idx + 0];
                uint8_t g = row[idx + 1];
                uint8_t r = row[idx + 2];
                uint8_t a = row[idx + 3];
                if (a == 0) continue;
                px = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
            } else if (bmp->pixel_mode == FT_PIXEL_MODE_MONO) {
                int byte = sx / 8;
                int bit = 7 - (sx % 8);
                uint8_t cov = (row[byte] >> bit) & 1;
                if (!cov) continue;
                px = (0xFFu << 24) | (fg & 0x00FFFFFFu);
            } else { /* FT_PIXEL_MODE_GRAY (8-bit coverage) */
                uint8_t cov = row[sx];
                if (cov == 0) continue;
                px = ((uint32_t)cov << 24) | (fg & 0x00FFFFFFu);
            }

            int dest_x = pen_x + dx;
            int dest_y = pen_y + dy;
            if (dest_x < 0 || dest_y < 0 || dest_x >= c->width ||
                dest_y >= c->height) {
                continue;
            }
            c->pixels[(size_t)dest_y * c->width + dest_x] =
                blend(px, c->pixels[(size_t)dest_y * c->width + dest_x]);
        }
    }

    /* crude bold: redraw one pixel to the right for non-colour glyphs */
    if (bold && !is_color && !box) {
        for (int dy = 0; dy < dh; dy++) {
            int sy = (int)(dy * syf);
            if (sy >= h) sy = h - 1;
            unsigned char *row = bmp->buffer + (size_t)sy * pitch;
            for (int dx = 0; dx < dw; dx++) {
                int sx = (int)(dx * sxf);
                if (sx >= w) sx = w - 1;
                uint8_t cov;
                if (bmp->pixel_mode == FT_PIXEL_MODE_MONO) {
                    int byte = sx / 8;
                    int bit = 7 - (sx % 8);
                    cov = (row[byte] >> bit) & 1;
                    cov = cov ? 255 : 0;
                } else {
                    cov = row[sx];
                }
                if (cov == 0) continue;
                int dest_x = pen_x + dx + 1;
                int dest_y = pen_y + dy;
                if (dest_x < 0 || dest_y < 0 || dest_x >= c->width ||
                    dest_y >= c->height) {
                    continue;
                }
                uint32_t px = ((uint32_t)cov << 24) | (fg & 0x00FFFFFFu);
                c->pixels[(size_t)dest_y * c->width + dest_x] =
                    blend(px, c->pixels[(size_t)dest_y * c->width + dest_x]);
            }
        }
    }
}

/*
 * Render the entire screen into an RGBA buffer.
 * Returns a malloc'd buffer (width*height uint32s), caller must free().
 *
 * `font_paths` is an ordered fallback list; the first face that contains a
 * given codepoint is used to draw it.
 */
void *render_screen(VTerm *vt, TermSize *size, PixelSize *out,
                    const char **font_paths, int n_fonts, int font_px)
{
    if (n_fonts <= 0 || !font_paths) {
        fprintf(stderr, "no fonts supplied\n");
        return NULL;
    }

    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0) {
        fprintf(stderr, "failed to init freetype\n");
        return NULL;
    }

    Canvas c;
    memset(&c, 0, sizeof(c));

    /* Load as many fallback faces as we can. */
    for (int i = 0; i < n_fonts && c.n_faces < MAX_FACES; i++) {
        FT_Face f;
        if (FT_New_Face(lib, font_paths[i], 0, &f) != 0) {
            fprintf(stderr, "warning: failed to load font %s\n", font_paths[i]);
            continue;
        }
        int px = font_px > 0 ? font_px : 18;
        if (FT_Set_Pixel_Sizes(f, 0, px) != 0) {
            /* Bitmap-only fonts (e.g. color emoji) have no scalable outline,
             * so select the nearest fixed strike instead. */
            int best = -1, best_diff = 1 << 30;
            for (int s = 0; s < f->num_fixed_sizes; s++) {
                int h = (int)f->available_sizes[s].height;
                int d = h - px; if (d < 0) d = -d;
                if (d < best_diff) { best_diff = d; best = s; }
            }
            if (best < 0 || FT_Select_Size(f, best) != 0) {
                fprintf(stderr, "warning: failed to size font %s\n",
                        font_paths[i]);
                FT_Done_Face(f);
                continue;
            }
        }
        c.faces[c.n_faces++] = f;
    }

    if (c.n_faces == 0) {
        fprintf(stderr, "no usable font faces\n");
        FT_Done_FreeType(lib);
        return NULL;
    }

    FT_Face primary = c.faces[0];

    c.font_height = ((font_px > 0 ? font_px : 18)) + CELL_PADDING;
    /* Cell width is the advance of a representative monospace glyph (M),
     * NOT max_advance: a proportional font's max_advance can be inflated by a
     * single wide/fullwidth glyph (e.g. 51px for an 18px Latin face), which
     * would make every cell absurdly wide and stop box glyphs from filling
     * it. */
    FT_Load_Char(primary, 'M', FT_LOAD_NO_HINTING);
    int cell_adv = (int)(primary->glyph->advance.x / 64);
    if (cell_adv < 1) {
        cell_adv = (int)((float)primary->size->metrics.max_advance / 64.0f);
    }
    c.font_width = cell_adv;
    c.ascender = (int)((float)primary->size->metrics.ascender / 64.0f);
    c.baseline = c.font_height - CELL_PADDING / 2 - 1;

    c.width = size->cols * c.font_width;
    c.height = size->rows * c.font_height;

    c.pixels = malloc(sizeof(uint32_t) * (size_t)c.width * (size_t)c.height);
    if (!c.pixels) {
        for (int i = 0; i < c.n_faces; i++) FT_Done_Face(c.faces[i]);
        FT_Done_FreeType(lib);
        return NULL;
    }

    VTermScreen *screen = vterm_obtain_screen(vt);
    if (!screen) {
        fprintf(stderr, "failed to obtain screen\n");
        free(c.pixels);
        for (int i = 0; i < c.n_faces; i++) FT_Done_Face(c.faces[i]);
        FT_Done_FreeType(lib);
        return NULL;
    }

    VTermState *state = vterm_obtain_state(vt);
    VTermColor default_fg, default_bg;
    vterm_state_get_default_colors(state, &default_fg, &default_bg);
    (void)default_fg;
    uint32_t def_bg = make_color(screen, &default_bg);

    /* clear background */
    for (int i = 0; i < c.width * c.height; i++) {
        c.pixels[i] = def_bg;
    }

    for (int row = 0; row < size->rows; row++) {
        for (int col = 0; col < size->cols; col++) {
            VTermPos pos = { .row = row, .col = col };
            VTermScreenCell cell;
            if (!vterm_screen_get_cell(screen, pos, &cell)) {
                continue;
            }

            uint32_t fg = make_color(screen, &cell.fg);
            uint32_t bg = make_color(screen, &cell.bg);
            int cell_x = col * c.font_width;
            int cell_y = row * c.font_height;

            /* A cell with width <= 0 is the trailing half of a wide character;
               its area was already painted by the leading cell, so skip it. */
            if (cell.width <= 0) {
                continue;
            }
            int cell_px_w = (int)cell.width * c.font_width;

            /* background fill */
            if (bg != def_bg) {
                for (int y = 0; y < c.font_height; y++) {
                    for (int x = 0; x < cell_px_w; x++) {
                        c.pixels[(size_t)(cell_y + y) * c.width +
                                 (cell_x + x)] = bg;
                    }
                }
            }

            /* Foreground glyphs: render the whole chars[] sequence -- the base
               character plus any combining marks, or a wide character that spans
               this cell run. Each glyph is centered within the cell's pixel
               width so wide and combining glyphs land in the right place. The
               first face that actually contains the codepoint is used. */
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; i++) {
                uint32_t ch = cell.chars[i];

                FT_Face pick = NULL;
                for (int f = 0; f < c.n_faces; f++) {
                    if (FT_Get_Char_Index(c.faces[f], ch) != 0) {
                        pick = c.faces[f];
                        break;
                    }
                }
                if (!pick) {
                    continue; /* no fallback face has this glyph */
                }

                if (FT_Load_Char(pick, (FT_ULong)ch,
                                 FT_LOAD_RENDER | FT_LOAD_COLOR |
                                 (cell.attrs.bold ? FT_LOAD_FORCE_AUTOHINT : 0)) != 0) {
                    continue;
                }

                FT_GlyphSlot slot = pick->glyph;
                draw_glyph(&c, &slot->bitmap, cell_x, cell_y,
                           cell_px_w, c.font_height, c.baseline,
                           slot->bitmap_top, slot->bitmap_left, fg,
                           cell.attrs.bold, is_box_drawing(ch));
            }

            /* underline / strike */
            if (cell.attrs.underline != VTERM_UNDERLINE_OFF) {
                for (int x = 0; x < cell_px_w; x++) {
                    canvas_set_pixel(&c, cell_x + x, cell_y + c.baseline + 1, fg);
                }
            }
            if (cell.attrs.strike) {
                for (int x = 0; x < cell_px_w; x++) {
                    canvas_set_pixel(&c, cell_x + x, cell_y + c.baseline - 4, fg);
                }
            }
        }
    }

    for (int i = 0; i < c.n_faces; i++) {
        FT_Done_Face(c.faces[i]);
    }
    FT_Done_FreeType(lib);

    out->width = c.width;
    out->height = c.height;
    return c.pixels;
}
