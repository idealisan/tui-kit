/*
 * platform_posix.c - PTY/process/font implementation for Linux & macOS.
 *
 * Uses openpty() on Linux (<pty.h>) and openpty() on macOS (<util.h>).
 * The slave becomes the child's controlling terminal.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(TR_PLATFORM_MACOS)
#  include <util.h>
#  include <sys/ioctl.h>
#else
#  include <pty.h>
#  include <termios.h>
#endif

#include "platform.h"

#define READ_CHUNK 4096

#if defined(TR_PLATFORM_MACOS)
typedef struct winsize tr_winsize;
#else
typedef struct winsize tr_winsize;
#endif

static void set_winsize(int fd, int rows, int cols)
{
    tr_winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ioctl(fd, TIOCSWINSZ, &ws);
}

int tr_proc_spawn(TrProc *out, const char *cmd, int rows, int cols)
{
    int master = -1, slave = -1;
    pid_t pid;

    memset(out, 0, sizeof(*out));

    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        perror("openpty");
        return -1;
    }

    set_winsize(slave, rows, cols);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        close(master);
        close(slave);
        return -1;
    }

    if (pid == 0) {
        /* child: make slave our controlling terminal */
        if (setsid() < 0)
            _exit(1);
        if (ioctl(slave, TIOCSCTTY, 0) < 0)
            _exit(1);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);
        close(master);

        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(slave);
    out->fd = master;
    out->proc = (void *)(intptr_t)pid;
    return 0;
}

int tr_proc_read(TrProc *proc, char *buf, int len)
{
    ssize_t n = read(proc->fd, buf, (size_t)len);
    if (n < 0) {
        if (errno == EIO)   /* slave closed */
            return 0;
        return -1;
    }
    return (int)n;
}

int tr_proc_drain(TrProc *proc, void *vt_ctx, int timeout_ms,
                  void (*feed)(void *, const char *, int))
{
    pid_t pid = (pid_t)(intptr_t)proc->proc;
    struct timeval deadline, now;

    gettimeofday(&now, NULL);
    deadline.tv_sec = now.tv_sec + timeout_ms / 1000;
    deadline.tv_usec = now.tv_usec + (timeout_ms % 1000) * 1000;

    for (;;) {
        fd_set fds;
        char buf[READ_CHUNK];
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);

        FD_ZERO(&fds);
        FD_SET(proc->fd, &fds);

        gettimeofday(&now, NULL);
        if (timercmp(&deadline, &now, <)) {
            fprintf(stderr, "timeout waiting for command output\n");
            return -1;
        }

        if (r == pid) {
            /* child gone: drain what remains with a short grace period */
            struct timeval grace = { 0, 100000 }; /* 100ms */
            int rc = select(proc->fd + 1, &fds, NULL, NULL, &grace);
            if (rc > 0 && FD_ISSET(proc->fd, &fds)) {
                int n = tr_proc_read(proc, buf, sizeof(buf));
                if (n > 0) {
                    feed(vt_ctx, buf, n);
                    continue;
                }
            }
            break;
        }

        struct timeval wait;
        timersub(&deadline, &now, &wait);

        int rc = select(proc->fd + 1, &fds, NULL, NULL, &wait);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            return -1;
        }
        if (rc == 0) {
            fprintf(stderr, "timeout waiting for command output\n");
            return -1;
        }

        int n = tr_proc_read(proc, buf, sizeof(buf));
        if (n > 0) {
            feed(vt_ctx, buf, n);
        } else if (n == 0) {
            break;
        }
    }

    return 0;
}

void tr_proc_close(TrProc *proc)
{
    if (proc->fd >= 0)
        close(proc->fd);
    proc->fd = -1;
}

#if defined(TR_PLATFORM_MACOS)
static const char *const tr_font_candidates[] = {
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Monaco.ttf",
    "/System/Library/Fonts/Supplemental/Monaco.ttf",
    "/Library/Fonts/Andale Mono.ttf",
    NULL,
};
#else
static const char *const tr_font_candidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/truetype/inconsolata/Inconsolata.otf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    NULL,
};
#endif

int tr_font_path(char *buf, size_t buflen)
{
    for (int i = 0; tr_font_candidates[i]; i++) {
        if (access(tr_font_candidates[i], R_OK) == 0) {
            snprintf(buf, buflen, "%s", tr_font_candidates[i]);
            return 0;
        }
    }
    return -1;
}
