/*
 * render.c - bitmap renderer: walks the libvterm screen buffer and draws
 *            each cell's glyph (via FreeType) into an RGBA pixel buffer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "termrenderer.h"

typedef struct {
    uint32_t *pixels;   /* RGBA, packed as 0xAARRGGBB */
    int width;
    int height;
    FT_Face face;
    int font_height;    /* pixels per row */
    int font_width;     /* pixels per column */
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

/* draw a FreeType glyph bitmap at (gx, gy), tinted with fg color */
static void draw_glyph(Canvas *c, FT_Bitmap *bitmap, int gx, int gy, uint32_t fg)
{
    for (int y = 0; y < (int)bitmap->rows; y++) {
        for (int x = 0; x < (int)bitmap->width; x++) {
            uint8_t cov;
            if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
                int byte = x / 8;
                int bit = 7 - (x % 8);
                cov = (bitmap->buffer[y * bitmap->pitch + byte] >> bit) & 1;
                cov = cov ? 255 : 0;
            } else {
                cov = bitmap->buffer[y * bitmap->pitch + x];
            }
            if (cov == 0) {
                continue;
            }
            uint32_t px = (0xFFu << 24) | (fg & 0x00FFFFFFu);
            if (cov < 255) {
                px = ((uint32_t)cov << 24) | (fg & 0x00FFFFFFu);
            }
            int dx = gx + x;
            int dy = gy + y;
            if (dx < 0 || dy < 0 || dx >= c->width || dy >= c->height) {
                continue;
            }
            c->pixels[(size_t)dy * c->width + dx] =
                blend(px, c->pixels[(size_t)dy * c->width + dx]);
        }
    }
}

/* draw a 1px horizontal underline/overline/strike line across a cell */
static void draw_hline(Canvas *c, int cell_x, int cell_y, int yoff, uint32_t fg)
{
    for (int x = 0; x < c->font_width; x++) {
        canvas_set_pixel(c, cell_x + x, cell_y + yoff, fg);
    }
}

/*
 * Render the entire screen into an RGBA buffer.
 * Returns a malloc'd buffer (width*height uint32s), caller must free().
 */
void *render_screen(VTerm *vt, TermSize *size, PixelSize *out, const char *font_path, int font_px)
{
    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0) {
        fprintf(stderr, "failed to init freetype\n");
        return NULL;
    }

    Canvas c;
    memset(&c, 0, sizeof(c));

    FT_Error err = FT_New_Face(lib, font_path, 0, &c.face);
    if (err) {
        fprintf(stderr, "failed to load font %s\n", font_path);
        FT_Done_FreeType(lib);
        return NULL;
    }

    int px = font_px > 0 ? font_px : 18; /* roughly 1024x768 for 80x24 */
    if (FT_Set_Pixel_Sizes(c.face, 0, px) != 0) {
        fprintf(stderr, "failed to set font size\n");
        FT_Done_Face(c.face);
        FT_Done_FreeType(lib);
        return NULL;
    }

    c.font_height = px + CELL_PADDING;
    c.font_width = (int)((float)c.face->size->metrics.max_advance / 64.0f);
    c.ascender = (int)((float)c.face->size->metrics.ascender / 64.0f);
    c.baseline = c.font_height - CELL_PADDING / 2 - 1;

    c.width = size->cols * c.font_width;
    c.height = size->rows * c.font_height;

    c.pixels = malloc(sizeof(uint32_t) * (size_t)c.width * (size_t)c.height);
    if (!c.pixels) {
        FT_Done_Face(c.face);
        FT_Done_FreeType(lib);
        return NULL;
    }

    VTermScreen *screen = vterm_obtain_screen(vt);
    if (!screen) {
        fprintf(stderr, "failed to obtain screen\n");
        free(c.pixels);
        FT_Done_Face(c.face);
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

            /* background fill */
            if (bg != def_bg) {
                for (int y = 0; y < c.font_height; y++) {
                    for (int x = 0; x < c.font_width; x++) {
                        c.pixels[(size_t)(cell_y + y) * c.width + (cell_x + x)] = bg;
                    }
                }
            }

            /* foreground glyph */
            uint32_t ch = cell.chars[0];
            if (ch == 0) {
                continue;
            }

            if (FT_Load_Char(c.face, (FT_ULong)ch, FT_LOAD_RENDER |
                             (cell.attrs.bold ? FT_LOAD_FORCE_AUTOHINT : 0)) != 0) {
                /* fallback: draw '?' box */
                continue;
            }

            int pen_x = cell_x + (c.font_width - c.face->glyph->bitmap.width) / 2;
            int pen_y = cell_y + c.baseline - c.face->glyph->bitmap_top;

            /* handle bold via stroking for simplicity: double-draw with offset */
            draw_glyph(&c, &c.face->glyph->bitmap, pen_x, pen_y, fg);
            if (cell.attrs.bold) {
                draw_glyph(&c, &c.face->glyph->bitmap, pen_x + 1, pen_y, fg);
            }

            /* underline / strike */
            if (cell.attrs.underline != VTERM_UNDERLINE_OFF) {
                draw_hline(&c, cell_x, cell_y,
                           c.baseline + 1, fg);
            }
            if (cell.attrs.strike) {
                draw_hline(&c, cell_x, cell_y, c.baseline - 4, fg);
            }
        }
    }

    FT_Done_Face(c.face);
    FT_Done_FreeType(lib);

    out->width = c.width;
    out->height = c.height;
    return c.pixels;
}
