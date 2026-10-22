/*
 * win32-pty.c
 * Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#ifdef _WIN32
#define KITTY_WIN32_COMPAT_IMPL
#include "win32-compat.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// winsock2.h has its own struct pollfd with a SOCKET fd member
#define pollfd wsa_pollfd
#include <winsock2.h>
#undef pollfd
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define UNUSED __attribute__((unused))

// Kept minimal on purpose, data-types.h pulls in Python.h and kitty types that
// collide with <windows.h>
bool win32_pty_open(int *master_fd, int *slave_fd, unsigned short rows, unsigned short cols);
pid_t win32_pty_spawn(int master_fd, const char *exe, const char *cwd, char *const argv[], char *const env[], int ready_read_fd, int stdin_read_fd);
bool win32_pty_resize(int master_fd, unsigned short rows, unsigned short cols);
bool win32_pty_signal_pid(pid_t pid, int sig);
void win32_pty_init(void);
const char *win32_pty_provider(void);

// The in-box conhost.exe that kernel32's CreatePseudoConsole() spawns drops
// escape sequences it does not understand, which includes the APC based kitty
// graphics protocol. The MIT licensed conpty.dll + OpenConsole.exe from the
// Windows Terminal project (NuGet package Microsoft.Windows.Console.ConPTY)
// relay them unmodified, so prefer that when it is installed next to kitty.exe.
typedef HRESULT(WINAPI *create_pseudo_console_func)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef HRESULT(WINAPI *resize_pseudo_console_func)(HPCON, COORD);
typedef VOID(WINAPI *close_pseudo_console_func)(HPCON);
static struct {
    create_pseudo_console_func create;
    resize_pseudo_console_func resize;
    close_pseudo_console_func close;
} conpty_api = {0};

const char *
win32_pty_provider(void) {
    if (!conpty_api.create) return "uninitialized";
    return conpty_api.create == CreatePseudoConsole ? "system" : "bundled";
}

static void
load_conpty_api(void) {
    conpty_api.create = CreatePseudoConsole;
    conpty_api.resize = ResizePseudoConsole;
    conpty_api.close = ClosePseudoConsole;
    // install directories can be longer than MAX_PATH
    const DWORD sz = 32768;
    wchar_t *path = malloc(sizeof(wchar_t) * (sz + 16));
    if (!path) return;
    DWORD n = GetModuleFileNameW(NULL, path, sz);
    wchar_t *slash = n && n < sz ? wcsrchr(path, L'\\') : NULL;
    HMODULE m = NULL;
    if (slash) {
        wcscpy(slash + 1, L"conpty.dll");
        m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    free(path);
    if (!m) return;
    create_pseudo_console_func c = NULL;
    resize_pseudo_console_func r = NULL;
    close_pseudo_console_func x = NULL;
    *(void **)(&c) = dlsym(m, "ConptyCreatePseudoConsole");
    *(void **)(&r) = dlsym(m, "ConptyResizePseudoConsole");
    *(void **)(&x) = dlsym(m, "ConptyClosePseudoConsole");
    if (c && r && x) {
        conpty_api.create = c;
        conpty_api.resize = r;
        conpty_api.close = x;
    } else FreeLibrary(m);
}

typedef struct Win32Pty {
    HPCON hpc;
    HANDLE pty_in_write, pty_out_read;
    SOCKET bridge_sock;
    int master_fd;
    HANDLE process, wait_handle;
    DWORD pid;
    volatile LONG refcount;
    volatile LONG output_done, pty_closed;
} Win32Pty;

#define MAX_PTYS 512
static Win32Pty *ptys[MAX_PTYS];
static CRITICAL_SECTION ptys_lock;

static void
pty_unref(Win32Pty *pty) {
    if (InterlockedDecrement(&pty->refcount) > 0) return;
    if (pty->wait_handle) UnregisterWaitEx(pty->wait_handle, NULL);
    if (pty->pty_in_write != INVALID_HANDLE_VALUE) CloseHandle(pty->pty_in_write);
    if (pty->pty_out_read != INVALID_HANDLE_VALUE) CloseHandle(pty->pty_out_read);
    if (pty->bridge_sock != INVALID_SOCKET) closesocket(pty->bridge_sock);
    if (pty->process) CloseHandle(pty->process);
    free(pty);
}

static Win32Pty *
pty_ref(Win32Pty *pty) {
    InterlockedIncrement(&pty->refcount);
    return pty;
}

static void
close_pseudo_console(Win32Pty *pty) {
    if (InterlockedExchange(&pty->pty_closed, 1) == 0) {
        // Closing the pty input pipe tells conhost there will be no more
        // input, ClosePseudoConsole() then terminates conhost which closes
        // the output pipe, so the output bridge thread sees EOF.
        conpty_api.close(pty->hpc);
    }
}

static void
unregister_pty(Win32Pty *pty) {
    EnterCriticalSection(&ptys_lock);
    bool found = false;
    for (size_t i = 0; i < MAX_PTYS; i++) {
        if (ptys[i] == pty) {
            ptys[i] = NULL;
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&ptys_lock);
    if (found) pty_unref(pty);
}

static Win32Pty *
pty_for_master_fd(int master_fd) {
    Win32Pty *ans = NULL;
    EnterCriticalSection(&ptys_lock);
    for (size_t i = 0; i < MAX_PTYS; i++) {
        if (ptys[i] && ptys[i]->master_fd == master_fd) {
            ans = pty_ref(ptys[i]);
            break;
        }
    }
    LeaveCriticalSection(&ptys_lock);
    return ans;
}

static Win32Pty *
pty_for_pid(pid_t pid) {
    Win32Pty *ans = NULL;
    EnterCriticalSection(&ptys_lock);
    for (size_t i = 0; i < MAX_PTYS; i++) {
        if (ptys[i] && ptys[i]->process && ptys[i]->pid == (DWORD)pid) {
            ans = pty_ref(ptys[i]);
            break;
        }
    }
    LeaveCriticalSection(&ptys_lock);
    return ans;
}

static void
on_master_fd_closed(int fd) {
    Win32Pty *pty = pty_for_master_fd(fd);
    if (!pty) return;
    // The terminal has gone away, tear everything down
    if (pty->process) TerminateProcess(pty->process, 1);
    close_pseudo_console(pty);
    unregister_pty(pty);
    pty_unref(pty);
}

// bridge threads {{{
static unsigned __stdcall
output_bridge_thread(void *arg) {
    Win32Pty *pty = arg;
    char buf[64 * 1024];
    DWORD n;
    while (ReadFile(pty->pty_out_read, buf, sizeof(buf), &n, NULL) && n > 0) {
        const char *p = buf;
        while (n) {
            int sent = send(pty->bridge_sock, p, (int)n, 0);
            if (sent == SOCKET_ERROR) goto end;
            p += sent;
            n -= sent;
        }
    }
end:
    InterlockedExchange(&pty->output_done, 1);
    // Tell the master side there is no more data, this causes read() to
    // return 0 which the child monitor treats as the child having died.
    shutdown(pty->bridge_sock, SD_SEND);
    pty_unref(pty);
    return 0;
}

static unsigned __stdcall
input_bridge_thread(void *arg) {
    Win32Pty *pty = arg;
    char buf[16 * 1024];
    while (true) {
        int n = recv(pty->bridge_sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        const char *p = buf;
        while (n) {
            DWORD written = 0;
            if (!WriteFile(pty->pty_in_write, p, (DWORD)n, &written, NULL)) goto end;
            p += written;
            n -= (int)written;
        }
    }
end:
    pty_unref(pty);
    return 0;
}

static void CALLBACK
process_exited(void *arg, BOOLEAN timed_out UNUSED) {
    Win32Pty *pty = arg;
    DWORD exit_code = 0;
    GetExitCodeProcess(pty->process, &exit_code);
    // Makes conhost flush remaining output and close the output pipe
    close_pseudo_console(pty);
    kitty_win32_deliver_signal(SIGCHLD, (pid_t)pty->pid, (int)((exit_code & 0xff) << 8));
    pty_unref(pty);
}

static bool
start_thread(unsigned(__stdcall *func)(void *), Win32Pty *pty) {
    pty_ref(pty);
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, func, pty, 0, NULL);
    if (!t) {
        pty_unref(pty);
        return false;
    }
    CloseHandle(t);
    return true;
}
// }}}

bool
win32_pty_open(int *master_fd, int *slave_fd, unsigned short rows, unsigned short cols) {
    *master_fd = *slave_fd = -1;
    HANDLE in_read = INVALID_HANDLE_VALUE, in_write = INVALID_HANDLE_VALUE, out_read = INVALID_HANDLE_VALUE, out_write = INVALID_HANDLE_VALUE;
    SOCKET s[2] = {INVALID_SOCKET, INVALID_SOCKET};
    int saved_errno;
    Win32Pty *pty = calloc(1, sizeof(Win32Pty));
    if (!pty) {
        errno = ENOMEM;
        return false;
    }
    pty->pty_in_write = pty->pty_out_read = INVALID_HANDLE_VALUE;
    pty->bridge_sock = INVALID_SOCKET;
    pty->refcount = 1;
    pty->master_fd = -1;
    if (!CreatePipe(&in_read, &in_write, NULL, 0) || !CreatePipe(&out_read, &out_write, NULL, 0)) {
        set_errno_from_last_error();
        goto fail;
    }
    COORD size = {.X = (SHORT)(cols ? cols : 80), .Y = (SHORT)(rows ? rows : 24)};
    HRESULT hr = conpty_api.create(size, in_read, out_write, 0, &pty->hpc);
    if (FAILED(hr)) {
        log_error("CreatePseudoConsole failed with HRESULT: 0x%08lx", (unsigned long)hr);
        errno = hr == E_OUTOFMEMORY ? ENOMEM : EIO;
        goto fail;
    }
    // conhost has its own copies of these
    CloseHandle(in_read);
    in_read = INVALID_HANDLE_VALUE;
    CloseHandle(out_write);
    out_write = INVALID_HANDLE_VALUE;
    pty->pty_in_write = in_write;
    in_write = INVALID_HANDLE_VALUE;
    pty->pty_out_read = out_read;
    out_read = INVALID_HANDLE_VALUE;

    if (kitty_win32_socketpair((uintptr_t *)s) != 0) {
        log_error("Failed to create loopback socketpair for ConPTY bridge with WSA error: %d", WSAGetLastError());
        goto fail;
    }
    SetHandleInformation((HANDLE)s[0], HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation((HANDLE)s[1], HANDLE_FLAG_INHERIT, 0);
    pty->bridge_sock = s[1];
    s[1] = INVALID_SOCKET;
    *master_fd = _open_osfhandle((intptr_t)s[0], _O_NOINHERIT);
    if (*master_fd < 0) {
        errno = EMFILE;
        goto fail;
    }
    s[0] = INVALID_SOCKET;
    pty->master_fd = *master_fd;
    // The slave fd is only a placeholder so that callers can treat the pair
    // like openpty(). Use a duplicate of the pty input handle so that closing
    // it is harmless.
    HANDLE slave_h;
    if (!DuplicateHandle(GetCurrentProcess(), pty->pty_in_write, GetCurrentProcess(), &slave_h, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        set_errno_from_last_error();
        goto fail;
    }
    *slave_fd = _open_osfhandle((intptr_t)slave_h, 0);
    if (*slave_fd < 0) {
        CloseHandle(slave_h);
        errno = EMFILE;
        goto fail;
    }

    bool registered = false;
    EnterCriticalSection(&ptys_lock);
    for (size_t i = 0; i < MAX_PTYS; i++) {
        if (!ptys[i]) {
            ptys[i] = pty_ref(pty);
            registered = true;
            break;
        }
    }
    LeaveCriticalSection(&ptys_lock);
    if (!registered) {
        errno = EMFILE;
        goto fail;
    }
    if (!start_thread(output_bridge_thread, pty) || !start_thread(input_bridge_thread, pty)) {
        errno = EAGAIN;
        unregister_pty(pty);
        goto fail;
    }
    pty_unref(pty); // the registry and the threads hold references
    return true;
fail:
    saved_errno = errno;
    if (pty->hpc) close_pseudo_console(pty);
    if (in_read != INVALID_HANDLE_VALUE) CloseHandle(in_read);
    if (in_write != INVALID_HANDLE_VALUE) CloseHandle(in_write);
    if (out_read != INVALID_HANDLE_VALUE) CloseHandle(out_read);
    if (out_write != INVALID_HANDLE_VALUE) CloseHandle(out_write);
    if (s[0] != INVALID_SOCKET) closesocket(s[0]);
    if (s[1] != INVALID_SOCKET) closesocket(s[1]);
    if (*master_fd >= 0) {
        kitty_win32_close(*master_fd);
        *master_fd = -1;
    }
    if (*slave_fd >= 0) {
        _close(*slave_fd);
        *slave_fd = -1;
    }
    pty_unref(pty);
    errno = saved_errno;
    return false;
}

// spawn {{{
static wchar_t *
utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *ans = malloc(n * sizeof(wchar_t));
    if (!ans) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, ans, n) <= 0) {
        free(ans);
        return NULL;
    }
    return ans;
}

static void
to_backslashes(wchar_t *s) {
    for (; *s; s++)
        if (*s == L'/') *s = L'\\';
}

static char *
wide_to_utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *ans = malloc(n);
    if (!ans) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, ans, n, NULL, NULL) <= 0) {
        free(ans);
        return NULL;
    }
    return ans;
}

typedef bool (*append_arg_func)(char *buf, size_t buf_sz, size_t *pos, const char *arg);

static bool
append_raw(char *buf, size_t buf_sz, size_t *pos, const char *s) {
    size_t n = strlen(s);
    if (*pos + n >= buf_sz) {
        errno = E2BIG;
        return false;
    }
    memcpy(buf + *pos, s, n + 1);
    *pos += n;
    return true;
}

typedef struct {
    char *buf;
    size_t cap, pos;
} CommandLine;

static bool
append_to_command_line(CommandLine *c, append_arg_func append, const char *arg) {
    const size_t before = c->pos;
    while (!append(c->buf, c->cap, &c->pos, arg)) {
        if (errno != E2BIG) return false;
        // the arg may have been partially appended
        c->pos = before;
        char *nbuf = realloc(c->buf, c->cap * 2);
        if (!nbuf) {
            errno = ENOMEM;
            return false;
        }
        c->buf = nbuf;
        c->cap *= 2;
    }
    return true;
}

// Returns NULL with errno set on failure. When batch_file is not NULL, argv[0]
// is ignored and the command line is the one for running batch_file via cmd.exe
static wchar_t *
build_command_line(char *const argv[], const char *batch_file) {
    CommandLine c = {.buf = malloc(4096), .cap = 4096};
    if (!c.buf) {
        errno = ENOMEM;
        return NULL;
    }
    c.buf[0] = 0;
    bool ok = true;
    if (batch_file) {
        ok = append_to_command_line(&c, append_raw, KITTY_WIN32_BATCH_PREFIX) &&
             append_to_command_line(&c, kitty_win32_append_batch_arg, batch_file);
        for (size_t i = 1; ok && argv[i]; i++) ok = append_to_command_line(&c, kitty_win32_append_batch_arg, argv[i]);
        ok = ok && append_to_command_line(&c, append_raw, "\"");
    } else {
        for (size_t i = 0; ok && argv[i]; i++) ok = append_to_command_line(&c, kitty_win32_append_quoted_arg, argv[i]);
    }
    wchar_t *ans = NULL;
    if (ok && !(ans = utf8_to_wide(c.buf))) errno = ENOMEM;
    free(c.buf);
    return ans;
}

// CreateProcess() runs batch files as cmd.exe /c with the command line as is,
// letting cmd.exe interpret metacharacters in the arguments, see
// kitty_win32_append_batch_arg(). Run them via cmd.exe explicitly instead,
// with a command line that is safe for it. On success *exe is replaced by the
// path to cmd.exe and *batch_file is the full path to the script, as cmd.exe
// would resolve a relative one against the cwd of the child, not ours.
static bool
resolve_batch_file(wchar_t **exe, char **batch_file) {
    *batch_file = NULL;
    if (!kitty_win32_is_batch_file(*exe)) return true;
    wchar_t sysdir[MAX_PATH + 1], full[32768];
    UINT n = GetSystemDirectoryW(sysdir, MAX_PATH - 8);
    DWORD fn = GetFullPathNameW(*exe, sizeof(full) / sizeof(full[0]), full, NULL);
    if (!n || n >= MAX_PATH - 8 || !fn || fn >= sizeof(full) / sizeof(full[0])) {
        set_errno_from_last_error();
        return false;
    }
    wcscpy(sysdir + n, L"\\cmd.exe");
    wchar_t *cmd = _wcsdup(sysdir);
    if (!cmd || !(*batch_file = wide_to_utf8(full))) {
        free(cmd);
        errno = ENOMEM;
        return false;
    }
    free(*exe);
    *exe = cmd;
    return true;
}

char *
win32_pty_batch_command_line(const char *exe, char *const argv[], char **cmd_exe) {
    *cmd_exe = NULL;
    wchar_t *wexe = utf8_to_wide(exe), *wcmdline = NULL;
    char *batch_file = NULL, *ans = NULL;
    int err = 0;
    if (!wexe) {
        errno = ENOMEM;
        return NULL;
    }
    to_backslashes(wexe);
    if (!resolve_batch_file(&wexe, &batch_file)) err = errno;
    else if (batch_file) {
        if (!(wcmdline = build_command_line(argv, batch_file))) err = errno;
        else if (!(ans = wide_to_utf8(wcmdline)) || !(*cmd_exe = wide_to_utf8(wexe))) {
            free(ans);
            ans = NULL;
            err = ENOMEM;
        }
    }
    free(wexe);
    free(wcmdline);
    free(batch_file);
    errno = err;
    return ans;
}

static wchar_t *
build_environment_block(char *const env[]) {
    size_t total = 1; // trailing NUL
    size_t count = 0;
    for (; env[count]; count++) total += strlen(env[count]) + 1;
    if (!count) total++;
    char *buf = malloc(total);
    if (!buf) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(env[i]) + 1;
        memcpy(buf + pos, env[i], len);
        pos += len;
    }
    if (!count) buf[pos++] = 0;
    buf[pos++] = 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, buf, (int)pos, NULL, 0);
    wchar_t *ans = n > 0 ? malloc(n * sizeof(wchar_t)) : NULL;
    if (ans && MultiByteToWideChar(CP_UTF8, 0, buf, (int)pos, ans, n) <= 0) {
        free(ans);
        ans = NULL;
    }
    free(buf);
    return ans;
}

typedef struct {
    HANDLE ready, thread;
    Win32Pty *pty;
} ResumeData;

static unsigned __stdcall
resume_thread(void *arg) {
    ResumeData *d = arg;
    if (d->ready != INVALID_HANDLE_VALUE) {
        char byte;
        DWORD n;
        // Blocks until the parent closes its write end, signalling the terminal is ready
        while (ReadFile(d->ready, &byte, 1, &n, NULL) && n > 0);
        CloseHandle(d->ready);
    }
    ResumeThread(d->thread);
    CloseHandle(d->thread);
    pty_unref(d->pty);
    free(d);
    return 0;
}

static HANDLE
dup_fd_handle(int fd, BOOL inheritable) {
    if (fd < 0) return INVALID_HANDLE_VALUE;
    HANDLE src = (HANDLE)_get_osfhandle(fd), ans = INVALID_HANDLE_VALUE;
    if (src == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), &ans, 0, inheritable, DUPLICATE_SAME_ACCESS)) return INVALID_HANDLE_VALUE;
    return ans;
}

static bool
is_existing_directory(const wchar_t *path) {
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool
initialize_console_utf8(LPPROC_THREAD_ATTRIBUTE_LIST attrs) {
    // Code pages belong to the pseudoconsole, not the terminal process. Use
    // the DLL-free console shim to set them without wrapping the real child.
    wchar_t exe[32768];
    DWORD n = GetModuleFileNameW(NULL, exe, sizeof(exe) / sizeof(exe[0]));
    if (!n || n >= sizeof(exe) / sizeof(exe[0])) {
        errno = ENAMETOOLONG;
        return false;
    }
    wchar_t *sep = wcsrchr(exe, L'\\');
    const wchar_t name[] = L"kitsu.com";
    if (!sep || (size_t)(sep + 1 - exe) + sizeof(name) / sizeof(name[0]) > sizeof(exe) / sizeof(exe[0])) {
        errno = ENAMETOOLONG;
        return false;
    }
    wcscpy(sep + 1, name);
    wchar_t cmdline[] = L"kitsu.com --initialize-console-utf8";
    STARTUPINFOEXW si = {.StartupInfo.cb = sizeof(si), .StartupInfo.dwFlags = STARTF_USESTDHANDLES, .lpAttributeList = attrs};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(exe, cmdline, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
        set_errno_from_last_error();
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD code = 1;
    bool ok = WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) && code == 0;
    if (!ok) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    if (!ok) errno = EIO;
    return ok;
}

pid_t
win32_pty_spawn(int master_fd, const char *exe, const char *cwd, char *const argv[], char *const env[], int ready_read_fd, int stdin_read_fd) {
    Win32Pty *pty = pty_for_master_fd(master_fd);
    if (!pty) {
        errno = EBADF;
        return -1;
    }
    pid_t ans = -1;
    wchar_t *wexe = utf8_to_wide(exe), *wcwd = cwd && *cwd ? utf8_to_wide(cwd) : NULL;
    wchar_t *cmdline = NULL, *envblock = build_environment_block(env);
    char *batch_file = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SIZE_T attrs_sz = 0;
    ResumeData *rd = NULL;
    HANDLE child_stdin = INVALID_HANDLE_VALUE;
    if (!wexe || (cwd && *cwd && !wcwd) || !envblock) {
        errno = ENOMEM;
        goto end;
    }
    to_backslashes(wexe);
    if (!resolve_batch_file(&wexe, &batch_file)) goto end;
    if (!(cmdline = build_command_line(argv, batch_file))) goto end;
    if (wcwd) {
        to_backslashes(wcwd);
        // Like chdir() failing in the POSIX child, run in our own cwd when the
        // requested one does not exist, for example a path reported via OSC 7
        // by a shell running inside WSL.
        if (!is_existing_directory(wcwd)) {
            free(wcwd);
            wcwd = NULL;
        }
    }
    if (stdin_read_fd > -1) {
        child_stdin = dup_fd_handle(stdin_read_fd, TRUE);
        if (child_stdin == INVALID_HANDLE_VALUE) {
            set_errno_from_last_error();
            goto end;
        }
    }
    DWORD num_attrs = child_stdin == INVALID_HANDLE_VALUE ? 1 : 2;
    InitializeProcThreadAttributeList(NULL, num_attrs, 0, &attrs_sz);
    attrs = malloc(attrs_sz);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, num_attrs, 0, &attrs_sz)) {
        set_errno_from_last_error();
        goto end;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pty->hpc, sizeof(pty->hpc), NULL, NULL)) {
        set_errno_from_last_error();
        goto end;
    }
    // Best effort: a missing or failing helper must not prevent the shell from starting.
    if (!initialize_console_utf8(attrs)) log_error("Failed to set UTF-8 code pages for the ConPTY child, it keeps the default OEM code page: %s", strerror(errno));
    // Restrict inheritance to only the stdin pipe
    if (child_stdin != INVALID_HANDLE_VALUE &&
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &child_stdin, sizeof(child_stdin), NULL, NULL)) {
        set_errno_from_last_error();
        goto end;
    }
    // Without STARTF_USESTDHANDLES the child gets copies of our (possibly
    // redirected) std handles instead of the pseudoconsole's. NULL handles
    // make the console subsystem hand it the ConPTY handles instead.
    STARTUPINFOEXW si = {.StartupInfo.cb = sizeof(si), .StartupInfo.dwFlags = STARTF_USESTDHANDLES, .lpAttributeList = attrs};
    if (child_stdin != INVALID_HANDLE_VALUE) si.StartupInfo.hStdInput = child_stdin;
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(
            wexe,
            cmdline,
            NULL,
            NULL,
            child_stdin != INVALID_HANDLE_VALUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
            envblock,
            wcwd,
            &si.StartupInfo,
            &pi)) {
        set_errno_from_last_error();
        goto end;
    }
    pty->process = pi.hProcess;
    pty->pid = pi.dwProcessId;
    pty_ref(pty);
    if (!RegisterWaitForSingleObject(&pty->wait_handle, pty->process, process_exited, pty, INFINITE, WT_EXECUTEONLYONCE)) {
        pty_unref(pty);
        set_errno_from_last_error();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        goto end;
    }
    rd = calloc(1, sizeof(ResumeData));
    if (!rd) {
        errno = ENOMEM;
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        goto end;
    }
    rd->ready = dup_fd_handle(ready_read_fd, FALSE);
    rd->thread = pi.hThread;
    rd->pty = pty_ref(pty);
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, resume_thread, rd, 0, NULL);
    if (!t) {
        // Run the child without waiting for the ready signal
        pty_unref(pty);
        if (rd->ready != INVALID_HANDLE_VALUE) CloseHandle(rd->ready);
        free(rd);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
    } else CloseHandle(t);
    ans = (pid_t)pi.dwProcessId;
end:
    if (child_stdin != INVALID_HANDLE_VALUE) CloseHandle(child_stdin);
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    free(wexe);
    free(wcwd);
    free(cmdline);
    free(envblock);
    free(batch_file);
    pty_unref(pty);
    return ans;
}
// }}}

bool
win32_pty_resize(int master_fd, unsigned short rows, unsigned short cols) {
    Win32Pty *pty = pty_for_master_fd(master_fd);
    if (!pty) {
        errno = EBADF;
        return false;
    }
    COORD size = {.X = (SHORT)cols, .Y = (SHORT)rows};
    HRESULT hr = pty->pty_closed ? S_OK : conpty_api.resize(pty->hpc, size);
    pty_unref(pty);
    if (FAILED(hr)) {
        errno = EIO;
        return false;
    }
    return true;
}

bool
win32_pty_signal_pid(pid_t pid, int sig) {
    Win32Pty *pty = pty_for_pid(pid);
    if (!pty) return false;
    bool ans = false;
    switch (sig) {
        case SIGINT: {
            // ConPTY translates Ctrl-C in the input stream to CTRL_C_EVENT for the client
            static const char ctrl_c = 3;
            DWORD written = 0;
            ans = WriteFile(pty->pty_in_write, &ctrl_c, 1, &written, NULL) && written == 1;
            break;
        }
        case SIGHUP:
        case SIGTERM:
        case SIGKILL: ans = TerminateProcess(pty->process, 128 + sig) || GetLastError() == ERROR_ACCESS_DENIED; break;
        default: break;
    }
    pty_unref(pty);
    return ans;
}

void
win32_pty_init(void) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    win32_compat_init();
    load_conpty_api();
    InitializeCriticalSection(&ptys_lock);
    kitty_win32_on_fd_close = on_master_fd_closed;
}
#endif
