/*
 * main.c - termrenderer entry point
 *
 * Usage:
 *   termrenderer [--cols N] [--rows N] [--font PATH] [--timeout MS]
 *                [--output PATH] -- command [args...]
 *
 * Runs `command` in a pseudo-terminal, parses output via libvterm, and
 * rasterizes the final screen buffer to a PNG. Cross-platform: uses the
 * platform abstraction layer for PTY/process/font handling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"
#include "termrenderer.h"

/*
 * Bundled Noto fallback fonts, resolved relative to the executable at
 * runtime. Order does not matter much (the renderer picks the first face that
 * actually contains a codepoint), but the general sans is also used as the
 * ultimate fallback when no system monospace font is found.
 */
static const char *kNotoFonts[] = {
    "NotoSans-Regular.ttf",
    "NotoColorEmoji-Regular.ttf",
    "NotoSansMono-Regular.ttf",
    "NotoSansSC-Regular.otf",
    "NotoSansJP-Regular.otf",
    "NotoSansKR-Regular.otf",
    "NotoSansArabic-Regular.ttf",
    "NotoSansDevanagari-Regular.ttf",
    "NotoSansHebrew-Regular.ttf",
    "NotoSansThai-Regular.ttf",
    "NotoSansSymbols2-Regular.ttf",
    NULL,
};

#define MAX_FONTS 32

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [options] -- command [args...]\n"
        "options:\n"
        "  --cols N        terminal columns (default %d)\n"
        "  --rows N        terminal rows (default %d)\n"
        "  --font PATH     truetype font (auto-detect if omitted)\n"
        "  --timeout MS    wait for output, ms (default 5000)\n"
        "  --output PATH   output png (default out.png)\n"
        "  --fontsize N    glyph pixel size (default 18)\n"
        "  --noto          use the bundled Noto Sans as the primary font\n",
        prog, DEFAULT_COLS, DEFAULT_ROWS);
}

static void feed_vterm(void *vt_ctx, const char *buf, int len)
{
    vterm_input_write((VTerm *)vt_ctx, buf, (size_t)len);
}

int main(int argc, char *argv[])
{
    TermSize size = { DEFAULT_ROWS, DEFAULT_COLS };
    char font_buf[1024];
    const char *font_path = NULL;
    const char *output = "out.png";
    int timeout_ms = 5000;
    int font_px = 18;
    int force_noto = 0;

    int i = 1;
    const char *cmd = NULL;
    while (i < argc) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "--cols") == 0 && i + 1 < argc) {
            size.cols = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc) {
            size.rows = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            font_path = argv[++i];
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else         if (strcmp(argv[i], "--fontsize") == 0 && i + 1 < argc) {
            font_px = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--noto") == 0) {
            force_noto = 1;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
        i++;
    }

    if (i < argc) {
        /* join remaining args into a single command line */
        size_t len = 0;
        for (int j = i; j < argc; j++) {
            len += strlen(argv[j]) + 1;
        }
        char *cmdline = malloc(len);
        cmdline[0] = '\0';
        for (int j = i; j < argc; j++) {
            strcat(cmdline, argv[j]);
            if (j + 1 < argc) {
                strcat(cmdline, " ");
            }
        }
        cmd = cmdline;
    } else {
        usage(argv[0]);
        return 1;
    }

    if (!font_path) {
        if (force_noto) {
            /* Unified Noto primary. Use the monospace Noto Sans Mono: it is a
             * real monospace face whose box-drawing glyphs are designed for
             * its own cell size, so borders align. Other Noto fonts (and the
             * system default) remain in the chain as fallbacks for codepoints
             * Noto Sans Mono lacks. */
            char exedir[1024];
            if (tr_exe_dir(exedir, sizeof(exedir)) == 0) {
                snprintf(font_buf, sizeof(font_buf), "%s/fonts/%s",
                         exedir, "NotoSansMono-Regular.ttf");
                if (access(font_buf, R_OK) == 0) {
                    font_path = font_buf;
                }
            }
        }
        if (!font_path && tr_font_path(font_buf, sizeof(font_buf)) < 0) {
            /* No system monospace font: fall back to bundled Noto Sans. */
            char exedir[1024];
            if (tr_exe_dir(exedir, sizeof(exedir)) == 0) {
                snprintf(font_buf, sizeof(font_buf), "%s/fonts/%s",
                         exedir, "NotoSans-Regular.ttf");
            }
            if (access(font_buf, R_OK) != 0) {
                fprintf(stderr, "no monospace font found; use --font PATH\n");
                free((void *)cmd);
                return 1;
            }
        }
        font_path = font_buf;
    }

    /* Build the fallback chain: the chosen primary font first, then any
     * bundled Noto fonts that exist next to the executable. */
    const char *fonts[MAX_FONTS];
    char owned[MAX_FONTS][1024];
    int n_fonts = 0;

    fonts[n_fonts++] = font_path;

    char exedir[1024];
    if (tr_exe_dir(exedir, sizeof(exedir)) == 0) {
        for (int i = 0; kNotoFonts[i] && n_fonts < MAX_FONTS; i++) {
            char cand[1024];
            snprintf(cand, sizeof(cand), "%s/fonts/%s", exedir, kNotoFonts[i]);
            if (access(cand, R_OK) != 0) {
                continue;
            }
            if (strcmp(cand, fonts[0]) == 0) {
                continue; /* already the primary face */
            }
            snprintf(owned[n_fonts], sizeof(owned[n_fonts]), "%s", cand);
            fonts[n_fonts] = owned[n_fonts];
            n_fonts++;
        }
    }

    VTerm *vt = vterm_new(size.rows, size.cols);
    if (!vt) {
        fprintf(stderr, "failed to create vterm\n");
        free((void *)cmd);
        return 1;
    }

    /* utf8 mode must be set before the state layer is created, otherwise
     * the initial encoding is selected from an uninitialised flag */
    vterm_set_utf8(vt, 1);

    /* screen layer must exist before any input is fed in */
    VTermScreen *screen = vterm_obtain_screen(vt);
    if (!screen) {
        fprintf(stderr, "failed to obtain screen layer\n");
        vterm_free(vt);
        free((void *)cmd);
        return 1;
    }

    /* vterm_screen_reset triggers state reset which initialises the
     * encoding table; without it, state->encoding[] stays NULL */
    vterm_screen_reset(screen, 1);

    TrProc proc;
    if (tr_proc_spawn(&proc, cmd, size.rows, size.cols) < 0) {
        fprintf(stderr, "failed to spawn command\n");
        vterm_free(vt);
        free((void *)cmd);
        return 1;
    }

    if (tr_proc_drain(&proc, vt, timeout_ms, feed_vterm) < 0) {
        fprintf(stderr, "command did not finish in %dms\n", timeout_ms);
    }
    tr_proc_close(&proc);

    PixelSize px;
    void *pixels = render_screen(vt, &size, &px, fonts, n_fonts, font_px);
    if (!pixels) {
        fprintf(stderr, "render failed\n");
        vterm_free(vt);
        free((void *)cmd);
        return 1;
    }

    if (png_write(output, pixels, px.width, px.height) < 0) {
        fprintf(stderr, "failed to write %s\n", output);
        free(pixels);
        vterm_free(vt);
        free((void *)cmd);
        return 1;
    }

    fprintf(stderr, "wrote %s (%dx%d)\n", output, px.width, px.height);

    free(pixels);
    vterm_free(vt);
    free((void *)cmd);
    return 0;
}
