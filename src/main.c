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
#include <pthread.h>
#include <unistd.h>

#include "platform.h"
#include "termrenderer.h"

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
        "  --input PATH    file to feed into the pty stdin (repeatable, fed in order)\n"
        "  --input-delay MS delay before feeding the first file (default 300)\n"
        "  --input-gap MS   delay between feeding consecutive files (default 0)\n"
        "  --dump PATH     write screen cells as text with fg colors\n",
        prog, DEFAULT_COLS, DEFAULT_ROWS);
}

struct feed_input {
    TrProc *proc;
    const char *const *paths;
    int count;
    int delay_ms;
    int gap_ms;
};

static void *feed_input_thread(void *arg)
{
    struct feed_input *fi = (struct feed_input *)arg;
    usleep((useconds_t)fi->delay_ms * 1000);

    char buf[4096];
    for (int i = 0; i < fi->count; i++) {
        FILE *f = fopen(fi->paths[i], "rb");
        if (!f) {
            fprintf(stderr, "cannot open input file %s\n", fi->paths[i]);
            return NULL;
        }
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            tr_proc_write(fi->proc, buf, (int)n);
        }
        fclose(f);
        if (i + 1 < fi->count)
            usleep((useconds_t)fi->gap_ms * 1000);
    }
    return NULL;
}

static void feed_vterm(void *vt_ctx, const char *buf, int len)
{
    vterm_input_write((VTerm *)vt_ctx, buf, (size_t)len);
}

/* UTF-8 encode a codepoint into out[], return byte count. */
static int utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* dump_screen writes every non-space cell as "row:col:char:r,g,b". */
static int dump_screen(VTerm *vt, TermSize *size, const char *path)
{
    VTermScreen *screen = vterm_obtain_screen(vt);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    int written = 0, cells = 0, blank = 0;
    for (int row = 0; row < size->rows; row++) {
        for (int col = 0; col < size->cols; col++) {
            VTermPos pos = { .row = row, .col = col };
            VTermScreenCell cell;
            if (!vterm_screen_get_cell(screen, pos, &cell)) {
                continue;
            }
            cells++;
            uint32_t ch = cell.chars[0];
            if (ch == 0 || ch == ' ') {
                blank++;
                continue;
            }
            VTermColor c = cell.fg;
            vterm_screen_convert_color_to_rgb(screen, &c);
            char ub[4];
            int n = utf8_encode(ch, ub);
            ub[n] = '\0';
            fprintf(f, "%d:%d:%s:%d,%d,%d\n", row, col, ub,
                    c.rgb.red, c.rgb.green, c.rgb.blue);
            written++;
        }
    }
    fclose(f);
    fprintf(stderr, "dump: rows=%d cols=%d cells=%d blank=%d written=%d\n",
            size->rows, size->cols, cells, blank, written);
    return 0;
}

int main(int argc, char *argv[])
{
    TermSize size = { DEFAULT_ROWS, DEFAULT_COLS };
    char font_buf[1024];
    const char *font_path = NULL;
    const char *output = "out.png";
    int timeout_ms = 5000;
    int font_px = 18;
    const char **inputs = NULL;
    int n_inputs = 0, cap_inputs = 0;
    int input_delay_ms = 300;
    int input_gap_ms = 0;
    const char *dump_path = NULL;

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
        } else if (strcmp(argv[i], "--fontsize") == 0 && i + 1 < argc) {
            font_px = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            if (n_inputs == cap_inputs) {
                cap_inputs = cap_inputs ? cap_inputs * 2 : 4;
                inputs = realloc(inputs, (size_t)cap_inputs * sizeof(char *));
            }
            inputs[n_inputs++] = argv[++i];
        } else if (strcmp(argv[i], "--input-delay") == 0 && i + 1 < argc) {
            input_delay_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--input-gap") == 0 && i + 1 < argc) {
            input_gap_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dump_path = argv[++i];
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
        if (tr_font_path(font_buf, sizeof(font_buf)) < 0) {
            fprintf(stderr, "no monospace font found; use --font PATH\n");
            free((void *)cmd);
            return 1;
        }
        font_path = font_buf;
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

    pthread_t feed_thr;
    struct feed_input fi = { &proc, inputs, n_inputs, input_delay_ms, input_gap_ms };
    int have_feed = 0;
    if (n_inputs > 0) {
        if (pthread_create(&feed_thr, NULL, feed_input_thread, &fi) != 0) {
            fprintf(stderr, "failed to start input thread\n");
        } else {
            have_feed = 1;
        }
    }

    if (tr_proc_drain(&proc, vt, timeout_ms, feed_vterm) < 0) {
        fprintf(stderr, "command did not finish in %dms\n", timeout_ms);
    }
    tr_proc_close(&proc);
    if (have_feed)
        pthread_join(feed_thr, NULL);

    /* apply pending damage to the cell buffer so get_cell sees the content */
    vterm_screen_flush_damage(screen);

    if (dump_path) {
        if (dump_screen(vt, &size, dump_path) < 0) {
            fprintf(stderr, "failed to write dump %s\n", dump_path);
        } else {
            fprintf(stderr, "wrote dump %s\n", dump_path);
        }
    }

    PixelSize px;
    void *pixels = render_screen(vt, &size, &px, font_path, font_px);
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
