/*
 * platform_windows.c - ConPTY-based terminal spawning for Windows.
 *
 * ConPTY (Windows Pseudo Console, Win10 1809+) lets us run a child in a
 * real console and capture its VT output, which we then feed to libvterm.
 *
 * Pipe layout (from the ConPTY cookbook):
 *   pipeIn  : (hPipePTYIn, hPipeIn)   app writes stdin  -> hPipeIn
 *   pipeOut : (hPipeOut,  hPipePTYOut) app reads  stdout <- hPipeOut
 *   CreatePseudoConsole(size, hPipePTYIn, hPipePTYOut, 0, &pc)
 *   child is created with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE and its
 *   std handles set to the app-side pipe ends.
 *
 * This file is only compiled on _WIN32.
 */

#ifndef _WIN32
#error "platform_windows.c is Windows-only"
#endif

/* Expose ConPTY types (HPCON, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE). They
 * require Win10 RS5 headers; the MinGW default (_WIN32_WINNT 0x0A00 alone)
 * is not enough because wincon.h checks NTDDI_VERSION >= 0x0A000006. */
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000006
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

#include "platform.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

typedef struct {
    HANDLE hpc;          /* pseudo console */
    HANDLE stdin_write;  /* write child stdin */
    HANDLE stdout_read;  /* read child stdout */
    PROCESS_INFORMATION pi;
} TrWinState;

typedef HRESULT (WINAPI *CreatePseudoConsole_t)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef void    (WINAPI *ClosePseudoConsole_t)(HPCON);

static CreatePseudoConsole_t fnCreatePseudoConsole;
static ClosePseudoConsole_t  fnClosePseudoConsole;

static void win_init_conpty(void)
{
    HMODULE h = GetModuleHandleA("kernel32.dll");
    if (h) {
        fnCreatePseudoConsole =
            (CreatePseudoConsole_t)GetProcAddress(h, "CreatePseudoConsole");
        fnClosePseudoConsole =
            (ClosePseudoConsole_t)GetProcAddress(h, "ClosePseudoConsole");
    }
}

int tr_proc_spawn(TrProc *out, const char *cmd, int rows, int cols)
{
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hPipePTYIn = NULL, hPipeIn = NULL;
    HANDLE hPipeOut = NULL, hPipePTYOut = NULL;
    TrWinState *st;
    COORD size;
    HRESULT hr;

    win_init_conpty();
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    if (!fnCreatePseudoConsole || !fnClosePseudoConsole) {
        fprintf(stderr, "ConPTY not available (Windows 10 1809+ required)\n");
        return -1;
    }

    if (!CreatePipe(&hPipePTYIn, &hPipeIn, &sa, 0))
        return -1;
    if (!CreatePipe(&hPipeOut, &hPipePTYOut, &sa, 0)) {
        CloseHandle(hPipePTYIn); CloseHandle(hPipeIn);
        return -1;
    }

    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;

    hr = fnCreatePseudoConsole(size, hPipePTYIn, hPipePTYOut, 0, &out->platform);
    if (FAILED(hr)) {
        fprintf(stderr, "CreatePseudoConsole failed\n");
        CloseHandle(hPipePTYIn); CloseHandle(hPipeIn);
        CloseHandle(hPipeOut); CloseHandle(hPipePTYOut);
        return -1;
    }

    st = (TrWinState *)out->platform;
    st->hpc = (HPCON)out->platform;
    st->stdin_write = hPipeIn;
    st->stdout_read = hPipeOut;

    /* Build STARTUPINFOEX with the pseudo console attribute */
    STARTUPINFOEXA si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = st->stdin_write;
    si.StartupInfo.hStdOutput = st->stdout_read;
    si.StartupInfo.hStdError = st->stdout_read;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    si.lpAttributeList = malloc(attrSize);
    if (!si.lpAttributeList)
        goto fail;
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize))
        goto fail;
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, st->hpc, sizeof(st->hpc),
            NULL, NULL))
        goto fail;

    char cmdline[4096];
    snprintf(cmdline, sizeof(cmdline), "cmd.exe /c %s", cmd);

    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                             NULL, NULL, &si.StartupInfo, &st->pi);
    free(si.lpAttributeList);
    if (!ok) {
        fprintf(stderr, "CreateProcess failed\n");
        goto fail;
    }

    /* The pty owns the other pipe ends; close our references */
    CloseHandle(hPipePTYIn);
    CloseHandle(hPipePTYOut);

    out->proc = (void *)st;
    return 0;

fail:
    if (st->hpc) fnClosePseudoConsole(st->hpc);
    if (st->stdin_write) CloseHandle(st->stdin_write);
    if (st->stdout_read) CloseHandle(st->stdout_read);
    free(st);
    out->platform = NULL;
    return -1;
}

int tr_proc_read(TrProc *proc, char *buf, int len)
{
    TrWinState *st = (TrWinState *)proc->proc;
    DWORD n = 0;
    if (!ReadFile(st->stdout_read, buf, (DWORD)len, &n, NULL))
        return 0;
    return (int)n;
}

int tr_proc_drain(TrProc *proc, void *vt_ctx, int timeout_ms,
                  void (*feed)(void *, const char *, int))
{
    TrWinState *st = (TrWinState *)proc->proc;
    DWORD deadline = GetTickCount() + (DWORD)timeout_ms;
    char buf[4096];

    for (;;) {
        /* Non-blocking read: PeekNamedPipe reports how many bytes are
         * buffered, and ReadFile on a pipe returns as soon as any data is
         * present, so only read when avail > 0. This matters because a
         * blocking ReadFile can wait forever here -- after the child exits,
         * the ConPTY host keeps the output pipe open (no EOF), so ReadFile
         * would never return and the timeout below would never be reached. */
        DWORD avail = 0;
        if (PeekNamedPipe(st->stdout_read, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            int n = tr_proc_read(proc, buf, sizeof(buf));
            if (n > 0) { feed(vt_ctx, buf, n); continue; }
        }

        if (WaitForSingleObject(st->pi.hProcess, 0) == WAIT_OBJECT_0) {
            /* Child exited. Drain anything written just before exit, then
             * stop -- again without a blocking read. */
            for (;;) {
                DWORD rem = 0;
                if (!PeekNamedPipe(st->stdout_read, NULL, 0, NULL, &rem, NULL) || rem == 0)
                    break;
                int n = tr_proc_read(proc, buf, sizeof(buf));
                if (n <= 0) break;
                feed(vt_ctx, buf, n);
            }
            break;
        }

        if ((long)(GetTickCount() - deadline) >= 0) {
            fprintf(stderr, "timeout waiting for command output\n");
            return -1;
        }
        Sleep(10);
    }
    return 0;
}

void tr_proc_close(TrProc *proc)
{
    TrWinState *st = (TrWinState *)proc->proc;
    if (!st)
        return;
    if (st->pi.hProcess) {
        TerminateProcess(st->pi.hProcess, 0);
        CloseHandle(st->pi.hProcess);
        CloseHandle(st->pi.hThread);
    }
    if (st->stdout_read) CloseHandle(st->stdout_read);
    if (st->stdin_write) CloseHandle(st->stdin_write);
    if (st->hpc) fnClosePseudoConsole(st->hpc);
    free(st);
    proc->proc = NULL;
}

int tr_font_path(char *buf, size_t buflen)
{
    static const char *const cands[] = {
        "C:\\Windows\\Fonts\\consola.ttf",
        "C:\\Windows\\Fonts\\cour.ttf",
        "C:\\Windows\\Fonts\\lucon.ttf",
        NULL,
    };
    for (int i = 0; cands[i]; i++) {
        if (GetFileAttributesA(cands[i]) != INVALID_FILE_ATTRIBUTES) {
            snprintf(buf, buflen, "%s", cands[i]);
            return 0;
        }
    }
    return -1;
}

int tr_exe_dir(char *buf, size_t buflen)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return -1;
    char *dir = dirname(path);
    if (!dir)
        return -1;
    snprintf(buf, buflen, "%s", dir);
    return 0;
}
