/*
 * platform.h - cross-platform abstractions
 *
 * termrenderer aims to run on Linux, macOS and Windows. This header defines
 * a small portability layer that isolates OS-specific behaviour:
 *   - terminal process spawning (PTY on POSIX, ConPTY/Winpty on Windows)
 *   - font path discovery
 *   - byte/endian helpers
 *
 * Library choices (all permissive licenses):
 *   - libvterm (MIT)  : terminal state machine, already compiled
 *   - FreeType  (FTL) : glyph rasterisation
 *   - libpng    (libpng-2.0) : PNG encoding
 */

#ifndef TERMRENDERER_PLATFORM_H
#define TERMRENDERER_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  define TR_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
#  define TR_PLATFORM_MACOS 1
#  define TR_PLATFORM_POSIX 1
#else
#  define TR_PLATFORM_LINUX 1
#  define TR_PLATFORM_POSIX 1
#endif

/*
 * A spawned terminal process. On POSIX `fd` is the pty master;
 * on Windows it is a handle we can read from.
 */
typedef struct {
    int fd;              /* POSIX pty master; -1 if not applicable */
    void *proc;          /* platform-specific child handle */
    void *platform;      /* private per-OS state */
} TrProc;

/* Spawn `cmd` in a terminal of the given size. Returns 0 or -1. */
int tr_proc_spawn(TrProc *out, const char *cmd, int rows, int cols);

/* Read up to `len` bytes of terminal output into `buf`. Returns bytes read,
 * 0 on EOF, -1 on error. */
int tr_proc_read(TrProc *proc, char *buf, int len);

/* Wait for process exit, draining remaining output into `vt`.
 * Returns 0 on success, -1 on timeout. */
int tr_proc_drain(TrProc *proc, void *vt_ctx, int timeout_ms,
                  void (*feed)(void *vt_ctx, const char *buf, int len));

/* Close process resources. */
void tr_proc_close(TrProc *proc);

/* Find a usable monospace font path. Returns 0 and fills buf, or -1. */
int tr_font_path(char *buf, size_t buflen);

/* Fill buf with the directory containing the running executable (no trailing
 * slash). Returns 0 on success, -1 if it cannot be determined. */
int tr_exe_dir(char *buf, size_t buflen);

#endif /* TERMRENDERER_PLATFORM_H */
