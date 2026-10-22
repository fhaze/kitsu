/*
 * win32-compat.c
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
#include <ws2tcpip.h>
#undef pollfd
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>
#include <aclapi.h>
#include <wtsapi32.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <corecrt_startup.h>
#include <direct.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#define UNUSED __attribute__((unused))

static void set_errno_from_wsa_error(void);
static bool token_sid_matches(HANDLE token, TOKEN_INFORMATION_CLASS cls, PSID sid, bool *matches);
void (*kitty_win32_on_fd_close)(int fd) = NULL;

void
set_errno_from_last_error(void) {
    switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_MOD_NOT_FOUND: errno = ENOENT; break;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION: errno = EACCES; break;
        case ERROR_PRIVILEGE_NOT_HELD: errno = EPERM; break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY: errno = ENOMEM; break;
        case ERROR_INVALID_HANDLE: errno = EBADF; break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: errno = EEXIST; break;
        case ERROR_DIRECTORY: errno = ENOTDIR; break;
        case ERROR_DIR_NOT_EMPTY: errno = ENOTEMPTY; break;
        case ERROR_FILENAME_EXCED_RANGE: errno = ENAMETOOLONG; break;
        case ERROR_CANT_RESOLVE_FILENAME: errno = ELOOP; break;
        case ERROR_NO_UNICODE_TRANSLATION: errno = EILSEQ; break;
        case ERROR_DISK_FULL: errno = ENOSPC; break;
        default: errno = EINVAL; break;
    }
}

static void
free_keep_errno(void *p) {
    int saved = errno;
    free(p);
    errno = saved;
}

// Unicode and long paths {{{
wchar_t *
kitty_win32_utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) {
        set_errno_from_last_error();
        return NULL;
    }
    wchar_t *ans = malloc(sizeof(wchar_t) * n);
    if (!ans) {
        errno = ENOMEM;
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, ans, n);
    return ans;
}

char *
kitty_win32_wide_to_utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) {
        set_errno_from_last_error();
        return NULL;
    }
    char *ans = malloc(n);
    if (!ans) {
        errno = ENOMEM;
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, s, -1, ans, n, NULL, NULL);
    return ans;
}

// Concatenation of up to three wide strings, NULL ones are skipped
static wchar_t *
join_wide(const wchar_t *a, const wchar_t *b, const wchar_t *c) {
    const size_t la = a ? wcslen(a) : 0, lb = b ? wcslen(b) : 0, lc = c ? wcslen(c) : 0;
    wchar_t *ans = malloc(sizeof(wchar_t) * (la + lb + lc + 1));
    if (!ans) {
        errno = ENOMEM;
        return NULL;
    }
    if (la) memcpy(ans, a, la * sizeof(wchar_t));
    if (lb) memcpy(ans + la, b, lb * sizeof(wchar_t));
    if (lc) memcpy(ans + la + lb, c, lc * sizeof(wchar_t));
    ans[la + lb + lc] = 0;
    return ans;
}

// \\?\ and \\.\ paths bypass Win32 path normalization and length limits
static bool
is_namespaced_path(const wchar_t *p) {
    return p[0] == L'\\' && p[1] == L'\\' && (p[2] == L'?' || p[2] == L'.') && p[3] == L'\\';
}

static bool
is_absolute_wide_path(const wchar_t *p) {
#define sep(c) ((c) == L'\\' || (c) == L'/')
    return (p[0] && p[1] == L':' && sep(p[2])) || (sep(p[0]) && sep(p[1]));
#undef sep
}

// Without the \\?\ prefix paths are limited to MAX_PATH, less 12 (room for an
// 8.3 name) for CreateDirectoryW(), unless long paths are enabled system wide
#define WIN32_PATH_LIMIT (MAX_PATH - 12)

// Takes ownership of p and returns it, or, if it is too long for the Win32
// namespace, its absolute, normalized form with the \\?\ prefix
static wchar_t *
extend_path(wchar_t *p) {
    if (!p || is_namespaced_path(p) || (is_absolute_wide_path(p) && wcslen(p) < WIN32_PATH_LIMIT)) return p;
    DWORD n = GetFullPathNameW(p, 0, NULL, NULL);
    if (!n) {
        set_errno_from_last_error();
        free(p);
        return NULL;
    }
    // room for \\?\UNC\ in place of the leading \\ of a UNC path
    wchar_t *full = malloc(sizeof(wchar_t) * (n + 8));
    if (!full) {
        errno = ENOMEM;
        free(p);
        return NULL;
    }
    wchar_t *dest = full + 8;
    DWORD m = GetFullPathNameW(p, n, dest, NULL);
    if (!m || m >= n) {
        if (m) errno = ENAMETOOLONG; // the cwd changed between the calls
        else set_errno_from_last_error();
        free(p);
        free(full);
        return NULL;
    }
    // Short relative paths stay as they are
    if (m < WIN32_PATH_LIMIT || is_namespaced_path(dest)) {
        free(full);
        return p;
    }
    free(p);
    if (dest[0] == L'\\' && dest[1] == L'\\') {
        dest -= 6; // \\server\share -> \\?\UNC\server\share
        memcpy(dest, L"\\\\?\\UNC", 7 * sizeof(wchar_t));
    } else {
        dest -= 4;
        memcpy(dest, L"\\\\?\\", 4 * sizeof(wchar_t));
    }
    memmove(full, dest, (wcslen(dest) + 1) * sizeof(wchar_t));
    return full;
}

wchar_t *
kitty_win32_wide_path(const char *path) {
    return extend_path(kitty_win32_utf8_to_wide(path));
}

// Removes the \\?\ prefix added by GetFinalPathNameByHandleW() where the path
// can be expressed without it, \\?\Volume{...}\ paths keep it.
static void
strip_long_path_prefix(wchar_t *p) {
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) memmove(p + 2, p + 8, (wcslen(p + 8) + 1) * sizeof(wchar_t)); // -> \\server\share
    else if (wcsncmp(p, L"\\\\?\\", 4) == 0 && p[4] && p[5] == L':') memmove(p, p + 4, (wcslen(p + 4) + 1) * sizeof(wchar_t));
}

// The final (symlinks resolved) path of an open file or directory
static wchar_t *
final_path_of_handle(HANDLE h) {
    DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED);
    if (!n) {
        set_errno_from_last_error();
        return NULL;
    }
    wchar_t *ans = malloc(sizeof(wchar_t) * n);
    if (!ans) {
        errno = ENOMEM;
        return NULL;
    }
    DWORD m = GetFinalPathNameByHandleW(h, ans, n, FILE_NAME_NORMALIZED);
    if (!m || m >= n) {
        if (m) errno = ENAMETOOLONG;
        else set_errno_from_last_error();
        free(ans);
        return NULL;
    }
    strip_long_path_prefix(ans);
    return ans;
}

// GetModuleFileNameW() of any length
static wchar_t *
module_file_name(HMODULE module) {
    for (DWORD sz = 512; sz <= 32768; sz *= 2) {
        wchar_t *buf = malloc(sizeof(wchar_t) * sz);
        if (!buf) {
            errno = ENOMEM;
            return NULL;
        }
        DWORD n = GetModuleFileNameW(module, buf, sz);
        if (n && n < sz) return buf;
        free(buf);
        if (!n) {
            set_errno_from_last_error();
            return NULL;
        }
    }
    errno = ENAMETOOLONG;
    return NULL;
}

// GetTempPathW(), which ends with a backslash
static wchar_t *
temp_dir(void) {
    DWORD n = GetTempPathW(0, NULL);
    wchar_t *ans = n ? malloc(sizeof(wchar_t) * n) : NULL;
    if (!ans) {
        if (n) errno = ENOMEM;
        else set_errno_from_last_error();
        return NULL;
    }
    DWORD m = GetTempPathW(n, ans);
    if (!m || m >= n) {
        if (m) errno = ENAMETOOLONG;
        else set_errno_from_last_error();
        free(ans);
        return NULL;
    }
    return ans;
}

char **
kitty_win32_utf8_argv(int *argc) {
    // Split the wide command line with the C runtime's rules, not those of
    // CommandLineToArgvW(), so that arguments are the same as in the narrow
    // argv, only not lossy. The two differ for "" inside double quotes: the C
    // runtime reads a literal " and stays inside the quotes, which the quoting
    // for batch files in kitty_win32_append_batch_arg() relies on, as do
    // other programs that pass arguments through batch files.
    errno_t err = _configure_wide_argv(_crt_argv_unexpanded_arguments);
    if (err) {
        errno = err;
        return NULL;
    }
    const int n = __argc;
    wchar_t **wargv = __wargv;
    if (!wargv) {
        errno = EINVAL;
        return NULL;
    }
    size_t total = sizeof(char *) * (n + 1);
    for (int i = 0; i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (len <= 0) {
            set_errno_from_last_error();
            return NULL;
        }
        total += len;
    }
    char **ans = malloc(total);
    if (!ans) {
        errno = ENOMEM;
        return NULL;
    }
    char *p = (char *)(ans + n + 1), *end = (char *)ans + total;
    for (int i = 0; i < n; i++) {
        ans[i] = p;
        p += WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, p, (int)(end - p), NULL, NULL);
    }
    ans[n] = NULL;
    *argc = n;
    return ans;
}
// }}}

// poll {{{
int
poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    WSAPOLLFD stack_buf[16], *wfds = stack_buf;
    if (nfds > sizeof(stack_buf) / sizeof(stack_buf[0])) {
        wfds = malloc(nfds * sizeof(WSAPOLLFD));
        if (!wfds) {
            errno = ENOMEM;
            return -1;
        }
    }
    for (nfds_t i = 0; i < nfds; i++) {
        wfds[i].fd = fds[i].fd < 0 ? INVALID_SOCKET : (SOCKET)_get_osfhandle(fds[i].fd);
        wfds[i].events = fds[i].events;
        wfds[i].revents = 0;
    }
    int ret = WSAPoll(wfds, (ULONG)nfds, timeout);
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        errno = err == WSAEINTR ? EINTR : (err == WSAENOTSOCK ? ENOTSOCK : EINVAL);
    } else {
        for (nfds_t i = 0; i < nfds; i++) fds[i].revents = wfds[i].revents;
    }
    if (wfds != stack_buf) free(wfds);
    return ret;
}
// }}}

// mmap {{{
typedef struct MappedRegion {
    void *addr;
    size_t length;
    HANDLE mapping;
    bool anonymous;
    struct MappedRegion *next;
} MappedRegion;

static MappedRegion *mapped_regions = NULL;
static CRITICAL_SECTION mapped_regions_lock;
static bool mapped_regions_lock_initialized = false;

static void
lock_regions(void) {
    if (!mapped_regions_lock_initialized) {
        InitializeCriticalSection(&mapped_regions_lock);
        mapped_regions_lock_initialized = true;
    }
    EnterCriticalSection(&mapped_regions_lock);
}

static void
unlock_regions(void) {
    LeaveCriticalSection(&mapped_regions_lock);
}

void *
mmap(void *addr UNUSED, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!length) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    MappedRegion *r = calloc(1, sizeof(MappedRegion));
    if (!r) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    r->length = length;
    if (flags & MAP_ANON) {
        DWORD protect = (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
        if (prot & PROT_EXEC) protect = (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        r->addr = VirtualAlloc(NULL, length, MEM_COMMIT | MEM_RESERVE, protect);
        if (!r->addr) {
            set_errno_from_last_error();
            free(r);
            return MAP_FAILED;
        }
        r->anonymous = true;
    } else {
        HANDLE fh = (HANDLE)_get_osfhandle(fd);
        if (fh == INVALID_HANDLE_VALUE) {
            errno = EBADF;
            free(r);
            return MAP_FAILED;
        }
        DWORD protect, access;
        if (prot & PROT_WRITE) {
            if (flags & MAP_PRIVATE) {
                protect = PAGE_WRITECOPY;
                access = FILE_MAP_COPY;
            } else {
                protect = PAGE_READWRITE;
                access = FILE_MAP_READ | FILE_MAP_WRITE;
            }
        } else {
            protect = PAGE_READONLY;
            access = FILE_MAP_READ;
        }
        uint64_t max_size = (uint64_t)offset + length;
        r->mapping = CreateFileMappingW(fh, NULL, protect, (DWORD)(max_size >> 32), (DWORD)(max_size & 0xffffffff), NULL);
        if (!r->mapping) {
            set_errno_from_last_error();
            free(r);
            return MAP_FAILED;
        }
        r->addr = MapViewOfFile(r->mapping, access, (DWORD)((uint64_t)offset >> 32), (DWORD)((uint64_t)offset & 0xffffffff), length);
        if (!r->addr) {
            set_errno_from_last_error();
            CloseHandle(r->mapping);
            free(r);
            return MAP_FAILED;
        }
    }
    lock_regions();
    r->next = mapped_regions;
    mapped_regions = r;
    unlock_regions();
    return r->addr;
}

int
munmap(void *addr, size_t length UNUSED) {
    lock_regions();
    MappedRegion **pp = &mapped_regions;
    while (*pp && (*pp)->addr != addr) pp = &(*pp)->next;
    MappedRegion *r = *pp;
    if (r) *pp = r->next;
    unlock_regions();
    if (!r) {
        errno = EINVAL;
        return -1;
    }
    bool ok;
    if (r->anonymous) {
        ok = VirtualFree(r->addr, 0, MEM_RELEASE);
    } else {
        ok = UnmapViewOfFile(r->addr);
        CloseHandle(r->mapping);
    }
    free(r);
    if (!ok) {
        set_errno_from_last_error();
        return -1;
    }
    return 0;
}

int
mlock(const void *addr, size_t len) {
    if (!VirtualLock((LPVOID)addr, len)) {
        set_errno_from_last_error();
        return -1;
    }
    return 0;
}

int
munlock(const void *addr, size_t len) {
    if (!VirtualUnlock((LPVOID)addr, len)) {
        set_errno_from_last_error();
        return -1;
    }
    return 0;
}

// Shared memory objects are emulated as files in a per-user directory, which
// allows them to be accessed by name from other processes (kitten, ssh, etc).
static wchar_t *
shm_path(const char *name) {
    while (*name == '/') name++;
    if (!*name || strchr(name, '/') || strchr(name, '\\')) {
        errno = EINVAL;
        return NULL;
    }
    wchar_t *wname = NULL, *tmp = NULL, *dir = NULL, *ans = NULL;
    if (!(wname = kitty_win32_utf8_to_wide(name)) || !(tmp = temp_dir())) goto end;
    if (!(dir = extend_path(join_wide(tmp, L"kitty-shm", NULL)))) goto end;
    CreateDirectoryW(dir, NULL);
    ans = extend_path(join_wide(tmp, L"kitty-shm\\", wname));
end:
    free_keep_errno(wname);
    free_keep_errno(tmp);
    free_keep_errno(dir);
    return ans;
}

// Opened with FILE_SHARE_DELETE so that a mapped object can be unlinked by its
// creator while other processes still have it open, as with POSIX shm
int
shm_open(const char *name, int oflag, mode_t mode UNUSED) {
    wchar_t *path = shm_path(name);
    if (!path) return -1;
    DWORD access = GENERIC_READ, disposition = OPEN_EXISTING;
    if (oflag & (O_WRONLY | O_RDWR)) access |= GENERIC_WRITE;
    if (oflag & O_CREAT) disposition = (oflag & O_EXCL) ? CREATE_NEW : (oflag & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (oflag & O_TRUNC) disposition = TRUNCATE_EXISTING;
    HANDLE h = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) set_errno_from_last_error();
    free_keep_errno(path);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int fd = _open_osfhandle((intptr_t)h, _O_BINARY | _O_NOINHERIT | ((oflag & (O_WRONLY | O_RDWR)) ? _O_RDWR : _O_RDONLY));
    if (fd < 0) {
        CloseHandle(h);
        errno = EMFILE;
    }
    return fd;
}

static int
unlink_posix(const wchar_t *path) {
    HANDLE h = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_last_error();
        return -1;
    }
    FILE_DISPOSITION_INFO_EX dex = {.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
    bool ok = SetFileInformationByHandle(h, FileDispositionInfoEx, &dex, sizeof(dex));
    if (!ok) { // filesystem without POSIX delete support
        FILE_DISPOSITION_INFO d = {.DeleteFile = TRUE};
        ok = SetFileInformationByHandle(h, FileDispositionInfo, &d, sizeof(d));
    }
    if (!ok) set_errno_from_last_error();
    CloseHandle(h);
    return ok ? 0 : -1;
}

// Remove the name immediately even if the file is still open elsewhere
int
kitty_win32_unlink_posix(const char *path) {
    wchar_t *wpath = kitty_win32_wide_path(path);
    if (!wpath) return -1;
    int ret = unlink_posix(wpath);
    free_keep_errno(wpath);
    return ret;
}

int
shm_unlink(const char *name) {
    wchar_t *path = shm_path(name);
    if (!path) return -1;
    int ret = unlink_posix(path);
    free_keep_errno(path);
    return ret;
}
// }}}

// dlfcn {{{
static char dlerror_buf[512];
static bool dlerror_set = false;

static void
set_dlerror(const char *what) {
    DWORD code = GetLastError();
    // The narrow FormatMessageA() would return text in the ANSI code page
    wchar_t wmsg[400] = {0};
    char msg[400] = {0};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, wmsg, sizeof(wmsg) / sizeof(wmsg[0]) - 1, NULL);
    WideCharToMultiByte(CP_UTF8, 0, wmsg, -1, msg, sizeof(msg) - 1, NULL, NULL);
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == ' ')) msg[--n] = 0;
    snprintf(dlerror_buf, sizeof(dlerror_buf), "%s: %s (error %lu)", what, msg, (unsigned long)code);
    dlerror_set = true;
}

void *
dlopen(const char *filename, int flags UNUSED) {
    HMODULE h;
    if (!filename) h = GetModuleHandleW(NULL);
    else {
        wchar_t *wpath = kitty_win32_utf8_to_wide(filename);
        if (!wpath) {
            snprintf(dlerror_buf, sizeof(dlerror_buf), "Invalid module name: %s", filename);
            dlerror_set = true;
            return NULL;
        }
        h = LoadLibraryExW(wpath, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        if (!h) h = LoadLibraryW(wpath);
        free(wpath);
    }
    if (!h) set_dlerror(filename ? filename : "main program");
    return (void *)h;
}

void *
dlsym(void *handle, const char *symbol) {
    FARPROC p = GetProcAddress((HMODULE)handle, symbol);
    if (!p) set_dlerror(symbol);
    return (void *)(uintptr_t)p;
}

int
dlclose(void *handle) {
    if (handle == (void *)GetModuleHandleW(NULL)) return 0;
    if (!FreeLibrary((HMODULE)handle)) {
        set_dlerror("FreeLibrary");
        return -1;
    }
    return 0;
}

char *
dlerror(void) {
    if (!dlerror_set) return NULL;
    dlerror_set = false;
    return dlerror_buf;
}
// }}}

// locale {{{
locale_t
newlocale(int category_mask, const char *locale, locale_t base) {
    if (base) _free_locale(base);
    return _create_locale(category_mask, locale);
}

void
freelocale(locale_t locobj) {
    if (locobj) _free_locale(locobj);
}
// }}}

// fcntl and friends {{{
static bool
is_socket(HANDLE h) {
    int type = 0, len = sizeof(type);
    return getsockopt((SOCKET)h, SOL_SOCKET, SO_TYPE, (char *)&type, &len) != SOCKET_ERROR;
}

static int
set_nonblocking(int fd, bool nonblock) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    // GetFileType() reports sockets as FILE_TYPE_PIPE, so check for them first
    if (!is_socket(h) && GetFileType(h) == FILE_TYPE_PIPE) {
        DWORD mode = nonblock ? PIPE_NOWAIT : PIPE_WAIT;
        if (!SetNamedPipeHandleState(h, &mode, NULL, NULL)) {
            set_errno_from_last_error();
            return -1;
        }
        return 0;
    }
    u_long arg = nonblock ? 1 : 0;
    if (ioctlsocket((SOCKET)h, FIONBIO, &arg) == SOCKET_ERROR) {
        errno = ENOTSOCK;
        return -1;
    }
    return 0;
}

int
fcntl(int fd, int cmd, ...) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    va_list ap;
    va_start(ap, cmd);
    int arg = (cmd == F_SETFD || cmd == F_SETFL) ? va_arg(ap, int) : 0;
    va_end(ap);
    DWORD flags = 0;
    switch (cmd) {
        case F_GETFD:
            if (!GetHandleInformation(h, &flags)) {
                set_errno_from_last_error();
                return -1;
            }
            return (flags & HANDLE_FLAG_INHERIT) ? 0 : FD_CLOEXEC;
        case F_SETFD:
            if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, (arg & FD_CLOEXEC) ? 0 : HANDLE_FLAG_INHERIT)) {
                set_errno_from_last_error();
                return -1;
            }
            return 0;
        case F_GETFL: return 0;
        case F_SETFL: return set_nonblocking(fd, (arg & O_NONBLOCK) != 0);
        default: errno = EINVAL; return -1;
    }
}

int
kitty_win32_fd_from_socket_handle(intptr_t sock) {
    if (!is_socket((HANDLE)sock)) {
        errno = ENOTSOCK;
        return -1;
    }
    int fd = _open_osfhandle(sock, _O_NOINHERIT);
    if (fd < 0) errno = EMFILE;
    return fd;
}

bool
kitty_win32_peer_credentials_are_available(int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        // fail closed, like the POSIX implementation
        return true;
    }
    if (!is_socket(h)) {
        errno = ENOTSOCK;
        // fail closed
        return true;
    }
    struct sockaddr_storage addr = {0};
    int sz = sizeof(addr);
    if (getsockname((SOCKET)h, (struct sockaddr *)&addr, &sz) == SOCKET_ERROR) {
        set_errno_from_last_error();
        // fail closed
        return true;
    }
    // No Windows listener may bypass the same-user check.
    return true;
}

static DWORD
tcp_peer_pid(const struct sockaddr_storage *local, const struct sockaddr_storage *peer) {
    void *table = NULL;
    DWORD size = 0, result = ERROR_INSUFFICIENT_BUFFER, pid = 0;
    // The table can grow between calls. Bound retries so churn fails closed.
    for (unsigned i = 0; i < 4 && result == ERROR_INSUFFICIENT_BUFFER; i++) {
        result = GetExtendedTcpTable(table, &size, false, peer->ss_family, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            free(table);
            table = malloc(size);
            if (!table) { errno = ENOMEM; return 0; }
        }
    }
    if (result == NO_ERROR && peer->ss_family == AF_INET) {
        const struct sockaddr_in *l = (const struct sockaddr_in *)local, *p = (const struct sockaddr_in *)peer;
        MIB_TCPTABLE_OWNER_PID *t = table;
        for (DWORD i = 0; i < t->dwNumEntries; i++) {
            MIB_TCPROW_OWNER_PID *r = t->table + i;
            if (r->dwLocalAddr == p->sin_addr.s_addr && r->dwLocalPort == p->sin_port &&
                r->dwRemoteAddr == l->sin_addr.s_addr && r->dwRemotePort == l->sin_port) {
                pid = r->dwOwningPid;
                break;
            }
        }
    } else if (result == NO_ERROR && peer->ss_family == AF_INET6) {
        const struct sockaddr_in6 *l = (const struct sockaddr_in6 *)local, *p = (const struct sockaddr_in6 *)peer;
        MIB_TCP6TABLE_OWNER_PID *t = table;
        for (DWORD i = 0; i < t->dwNumEntries; i++) {
            MIB_TCP6ROW_OWNER_PID *r = t->table + i;
            if (!memcmp(r->ucLocalAddr, &p->sin6_addr, 16) && r->dwLocalPort == p->sin6_port &&
                r->dwLocalScopeId == htonl(p->sin6_scope_id) &&
                !memcmp(r->ucRemoteAddr, &l->sin6_addr, 16) && r->dwRemotePort == l->sin6_port &&
                r->dwRemoteScopeId == htonl(l->sin6_scope_id)) {
                pid = r->dwOwningPid;
                break;
            }
        }
    }
    free(table);
    if (!pid) errno = EACCES;
    return pid;
}

static void
normalize_mapped_ipv4(struct sockaddr_storage *addr) {
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)addr;
    if (addr->ss_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
        struct sockaddr_in v4 = {.sin_family = AF_INET, .sin_port = v6->sin6_port};
        memcpy(&v4.sin_addr, &v6->sin6_addr.s6_addr[12], sizeof(v4.sin_addr));
        memset(addr, 0, sizeof(*addr));
        memcpy(addr, &v4, sizeof(v4));
    }
}

bool
kitty_win32_peer_is_same_user(int fd) {
    SOCKET s = (SOCKET)_get_osfhandle(fd);
    struct sockaddr_storage local = {0}, peer = {0};
    int llen = sizeof(local), plen = sizeof(peer);
    if (s == INVALID_SOCKET || getsockname(s, (struct sockaddr *)&local, &llen) == SOCKET_ERROR ||
        getpeername(s, (struct sockaddr *)&peer, &plen) == SOCKET_ERROR) {
        errno = ENOTSOCK;
        return false;
    }
    normalize_mapped_ipv4(&local);
    normalize_mapped_ipv4(&peer);
    if (local.ss_family != peer.ss_family ||
        (peer.ss_family == AF_INET && (ntohl(((struct sockaddr_in *)&peer)->sin_addr.s_addr) >> 24) != 127) ||
        (peer.ss_family == AF_INET6 && !IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)&peer)->sin6_addr)) ||
        (peer.ss_family != AF_INET && peer.ss_family != AF_INET6)) {
        errno = EACCES;
        return false;
    }
    DWORD pid = tcp_peer_pid(&local, &peer);
    if (!pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
    if (!process) { set_errno_from_last_error(); return false; }
    HANDLE ours = NULL, theirs = NULL;
    bool matches = false;
    struct { TOKEN_USER user; BYTE sid[SECURITY_MAX_SID_SIZE]; } info;
    DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ours) || !OpenProcessToken(process, TOKEN_QUERY, &theirs) ||
        !GetTokenInformation(ours, TokenUser, &info, sizeof(info), &size)) {
        set_errno_from_last_error();
    } else if (token_sid_matches(theirs, TokenUser, info.user.User.Sid, &matches) && matches) {
        // Pin the process and recheck the tuple to rule out PID reuse before OpenProcess().
        matches = tcp_peer_pid(&local, &peer) == pid;
    }
    if (ours) CloseHandle(ours);
    if (theirs) CloseHandle(theirs);
    CloseHandle(process);
    if (!matches) errno = EACCES;
    return matches;
}

void (*kitty_win32_socketpair_listening_hook)(unsigned short port) = NULL;

static bool
same_inet_address(SOCKET s, bool peer_of_s, const struct sockaddr_in *expected) {
    struct sockaddr_in addr = {0};
    int addrlen = sizeof(addr);
    int ret = peer_of_s ? getpeername(s, (struct sockaddr *)&addr, &addrlen) : getsockname(s, (struct sockaddr *)&addr, &addrlen);
    return ret != SOCKET_ERROR && addrlen == (int)sizeof(addr) && addr.sin_family == AF_INET &&
        addr.sin_addr.s_addr == expected->sin_addr.s_addr && addr.sin_port == expected->sin_port;
}

int
kitty_win32_socketpair(uintptr_t out[2]) {
    SOCKET listener = INVALID_SOCKET, a = INVALID_SOCKET, b = INVALID_SOCKET;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
    struct sockaddr_in a_local = {0}, a_peer = {0};
    int addrlen = sizeof(addr);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) goto fail;
    BOOL exclusive = TRUE;
    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive)) == SOCKET_ERROR) goto fail;
    if (bind(listener, (struct sockaddr *)&addr, addrlen) == SOCKET_ERROR) goto fail;
    if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) == SOCKET_ERROR) goto fail;
    // Any local process, of any user, can connect to the listener, so a full
    // backlog must not make our own connect() fail and the accepted socket
    // must be checked to be the peer of a (CVE-2024-3219 in CPython)
    if (listen(listener, SOMAXCONN) == SOCKET_ERROR) goto fail;
    if (kitty_win32_socketpair_listening_hook) kitty_win32_socketpair_listening_hook(ntohs(addr.sin_port));
    a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (a == INVALID_SOCKET) goto fail;
    if (connect(a, (struct sockaddr *)&addr, addrlen) == SOCKET_ERROR) goto fail;
    addrlen = sizeof(a_local);
    if (getsockname(a, (struct sockaddr *)&a_local, &addrlen) == SOCKET_ERROR) goto fail;
    addrlen = sizeof(a_peer);
    if (getpeername(a, (struct sockaddr *)&a_peer, &addrlen) == SOCKET_ERROR) goto fail;
    // connections are accepted in order, so a is found once those that were
    // queued before it are dropped, unless it was reset
    for (int attempts = 0; ; attempts++) {
        if (attempts >= 1024) {
            WSASetLastError(WSAECONNABORTED);
            goto fail;
        }
        b = accept(listener, NULL, NULL);
        if (b == INVALID_SOCKET) goto fail;
        if (same_inet_address(b, true, &a_local) && same_inet_address(b, false, &a_peer)) break;
        closesocket(b);
        b = INVALID_SOCKET;
    }
    closesocket(listener);
    out[0] = b;
    out[1] = a;
    return 0;
fail:
    set_errno_from_wsa_error();
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (a != INVALID_SOCKET) closesocket(a);
    if (b != INVALID_SOCKET) closesocket(b);
    return -1;
}

int
pipe2(int fds[2], int flags) {
    SOCKET s[2];
    if (kitty_win32_socketpair((uintptr_t *)s) != 0) return -1;
    // Keep both TCP directions open: Windows expires FIN_WAIT_2 connections,
    // breaking long-lived signal and wakeup pipes even though their fds are open.
    for (int i = 0; i < 2; i++) {
        if (flags & O_NONBLOCK) {
            u_long arg = 1;
            ioctlsocket(s[i], FIONBIO, &arg);
        }
        if (flags & O_CLOEXEC) SetHandleInformation((HANDLE)s[i], HANDLE_FLAG_INHERIT, 0);
        fds[i] = _open_osfhandle((intptr_t)s[i], (flags & O_CLOEXEC) ? _O_NOINHERIT : 0);
        if (fds[i] < 0) {
            if (i == 1) _close(fds[0]);
            else closesocket(s[1]);
            closesocket(s[i]);
            errno = EMFILE;
            return -1;
        }
    }
    return 0;
}

ssize_t
kitty_win32_read(int fd, void *buf, size_t count) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (is_socket(h)) return kitty_win32_recv(fd, buf, count, 0);
    return _read(fd, buf, (unsigned)(count > INT_MAX ? INT_MAX : count));
}

ssize_t
kitty_win32_write(int fd, const void *buf, size_t count) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (is_socket(h)) return kitty_win32_send(fd, buf, count, 0);
    return _write(fd, buf, (unsigned)(count > INT_MAX ? INT_MAX : count));
}

int
kitty_win32_close(int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (is_socket(h)) {
        if (kitty_win32_on_fd_close) kitty_win32_on_fd_close(fd);
        // closesocket() releases the Winsock state, _close() then fails
        // (the handle is gone) but still frees the CRT fd slot.
        closesocket((SOCKET)h);
        _close(fd);
        return 0;
    }
    return _close(fd);
}

static bool
is_absolute_path(const char *path) {
    return path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':');
}

// Wide path for path relative to the directory open at dirfd
static wchar_t *
wide_path_at(int dirfd, const char *path) {
    if (dirfd == AT_FDCWD || is_absolute_path(path)) return kitty_win32_wide_path(path);
    HANDLE h = (HANDLE)_get_osfhandle(dirfd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return NULL;
    }
    wchar_t *dir = final_path_of_handle(h), *rel = NULL, *ans = NULL;
    if (dir && (rel = kitty_win32_utf8_to_wide(path))) ans = extend_path(join_wide(dir, L"\\", rel));
    free_keep_errno(dir);
    free_keep_errno(rel);
    return ans;
}

// CRT open() can neither open directories nor refuse to follow symlinks
static int
open_dir_or_nofollow(const wchar_t *wpath, bool want_dir, bool nofollow) {
    DWORD cflags = want_dir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    if (nofollow) cflags |= FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE h = CreateFileW(
        wpath, want_dir ? FILE_LIST_DIRECTORY : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, cflags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_last_error();
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) {
        set_errno_from_last_error();
        CloseHandle(h);
        return -1;
    }
    if (want_dir && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(h);
        errno = ENOTDIR;
        return -1;
    }
    if (nofollow && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(h);
        errno = ELOOP;
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) {
        CloseHandle(h);
        errno = EMFILE;
    }
    return fd;
}

// CRT open() follows a symlink at the final component, even for O_CREAT |
// O_EXCL, where POSIX fails with EEXIST. Open the name itself instead, so that
// CREATE_NEW fails on an existing symlink and O_NOFOLLOW can refuse one.
// Truncation is deferred until the file is known not to be a reparse point.
static int
open_file_without_following(const wchar_t *wpath, int flags, int mode) {
    const bool nofollow = flags & O_NOFOLLOW;
    DWORD access = GENERIC_READ, disposition = OPEN_EXISTING, attrs = FILE_ATTRIBUTE_NORMAL;
    if (flags & O_WRONLY) access = GENERIC_WRITE;
    else if (flags & O_RDWR) access |= GENERIC_WRITE;
    if (flags & O_CREAT) {
        disposition = (flags & O_EXCL) ? CREATE_NEW : OPEN_ALWAYS;
        if (!(mode & _S_IWRITE)) attrs = FILE_ATTRIBUTE_READONLY;
    }
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof(sa), .bInheritHandle = !(flags & O_CLOEXEC)};
    HANDLE h = CreateFileW(
        wpath, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, disposition, attrs | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_last_error();
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) {
        set_errno_from_last_error();
        CloseHandle(h);
        return -1;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        // only reachable with O_NOFOLLOW, CREATE_NEW never opens an existing name
        CloseHandle(h);
        errno = nofollow ? ELOOP : EEXIST;
        return -1;
    }
    if ((flags & O_TRUNC) && (access & GENERIC_WRITE)) {
        FILE_END_OF_FILE_INFO eof = {0};
        if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof(eof))) {
            set_errno_from_last_error();
            CloseHandle(h);
            return -1;
        }
    }
    int fd = _open_osfhandle((intptr_t)h, _O_BINARY | (flags & (O_APPEND | O_CLOEXEC)));
    if (fd < 0) {
        CloseHandle(h);
        errno = EMFILE;
    }
    return fd;
}

int
openat(int dirfd, const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    wchar_t *wpath = wide_path_at(dirfd, path);
    if (!wpath) return -1;
    const bool want_dir = flags & O_DIRECTORY, nofollow = flags & O_NOFOLLOW;
    const bool readonly = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) == 0;
    int fd;
    if (want_dir || (nofollow && readonly)) fd = open_dir_or_nofollow(wpath, want_dir, nofollow);
    else if (nofollow || (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) fd = open_file_without_following(wpath, flags, mode);
    else fd = _wopen(wpath, (flags & ~KITTY_WIN32_NON_CRT_OPEN_FLAGS) | _O_BINARY, mode);
    free_keep_errno(wpath);
    return fd;
}

int
kitty_win32_open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    return openat(AT_FDCWD, path, flags, mode);
}

static int
mkdir_wide(const wchar_t *path) {
    if (CreateDirectoryW(path, NULL)) return 0;
    set_errno_from_last_error();
    return -1;
}

int
mkdirat(int dirfd, const char *path, mode_t mode UNUSED) {
    wchar_t *wpath = wide_path_at(dirfd, path);
    if (!wpath) return -1;
    int ret = mkdir_wide(wpath);
    free_keep_errno(wpath);
    return ret;
}

int
kitty_win32_mkdir(const char *path) {
    return mkdirat(AT_FDCWD, path, 0);
}

int
symlinkat(const char *target, int dirfd, const char *linkpath) {
    // The target is stored as is, so relative ones must stay relative, long
    // absolute ones need the \\?\ prefix, which readlink() removes
    wchar_t *wtarget = kitty_win32_utf8_to_wide(target), *wlink = NULL;
    if (wtarget && is_absolute_wide_path(wtarget)) wtarget = extend_path(wtarget);
    int ret = -1;
    if (wtarget && (wlink = wide_path_at(dirfd, linkpath))) {
        DWORD attrs = GetFileAttributesW(wtarget);
        DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
        if (CreateSymbolicLinkW(wlink, wtarget, flags)) ret = 0;
        else set_errno_from_last_error();
    }
    free_keep_errno(wtarget);
    free_keep_errno(wlink);
    return ret;
}

int
lockf(int fd, int cmd, off_t len) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov = {0};
    LARGE_INTEGER pos = {.QuadPart = 0};
    SetFilePointerEx(h, pos, &pos, FILE_CURRENT);
    ov.Offset = pos.LowPart;
    ov.OffsetHigh = pos.HighPart;
    DWORD lo = len ? (DWORD)(len & 0xffffffff) : 0xffffffff, hi = len ? (DWORD)((uint64_t)len >> 32) : 0x7fffffff;
    BOOL ok;
    switch (cmd) {
        case F_ULOCK: ok = UnlockFileEx(h, 0, lo, hi, &ov); break;
        case F_LOCK: ok = LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, lo, hi, &ov); break;
        case F_TLOCK:
        case F_TEST:
            ok = LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, lo, hi, &ov);
            if (!ok && GetLastError() == ERROR_LOCK_VIOLATION) {
                errno = EAGAIN;
                return -1;
            }
            if (ok && cmd == F_TEST) UnlockFileEx(h, 0, lo, hi, &ov);
            break;
        default: errno = EINVAL; return -1;
    }
    if (!ok) {
        set_errno_from_last_error();
        return -1;
    }
    return 0;
}

int
mkostemp(char *template, int flags) {
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int attempt = 0; attempt < 100; attempt++) {
        unsigned char rnd[6];
        if (!secure_random_bytes_win32(rnd, sizeof(rnd))) {
            errno = EIO;
            return -1;
        }
        for (int i = 0; i < 6; i++) template[len - 6 + i] = chars[rnd[i] % (sizeof(chars) - 1)];
        wchar_t *wpath = kitty_win32_wide_path(template);
        if (!wpath) return -1;
        int fd = _wopen(wpath, flags | _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
        free_keep_errno(wpath);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

int
mkstemp(char *template) {
    return mkostemp(template, 0);
}

static ssize_t
positional_io(int fd, void *buf, size_t count, off_t offset, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)((uint64_t)offset & 0xffffffff);
    ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD done = 0;
    BOOL ok = write ? WriteFile(h, buf, (DWORD)count, &done, &ov) : ReadFile(h, buf, (DWORD)count, &done, &ov);
    if (!ok) {
        if (!write && GetLastError() == ERROR_HANDLE_EOF) return 0;
        set_errno_from_last_error();
        return -1;
    }
    return (ssize_t)done;
}

ssize_t
pread(int fd, void *buf, size_t count, off_t offset) {
    return positional_io(fd, buf, count, offset, false);
}

ssize_t
pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return positional_io(fd, (void *)buf, count, offset, true);
}

// REPARSE_DATA_BUFFER from ntdef.h uses an anonymous union which is
// rejected by -pedantic
struct symlink_reparse_buffer {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    ULONG Flags;
    WCHAR PathBuffer[1];
};

// Reads the target of a symlink into a new string, *len is set to its length
static wchar_t *
read_symlink_target(const wchar_t *wpath, size_t *len) {
    HANDLE h = CreateFileW(
        wpath,
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        NULL);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_last_error();
        return NULL;
    }
    struct symlink_reparse_buffer *rdb = malloc(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    if (!rdb) {
        CloseHandle(h);
        errno = ENOMEM;
        return NULL;
    }
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, rdb, MAXIMUM_REPARSE_DATA_BUFFER_SIZE, &ret, NULL);
    DWORD err = GetLastError();
    CloseHandle(h);
    wchar_t *ans = NULL;
    if (!ok) errno = err == ERROR_NOT_A_REPARSE_POINT ? EINVAL : EIO;
    else if (rdb->ReparseTag != IO_REPARSE_TAG_SYMLINK) errno = EINVAL;
    else {
        const wchar_t *pb = rdb->PathBuffer;
        const wchar_t *name = pb + rdb->PrintNameOffset / sizeof(wchar_t);
        size_t n = rdb->PrintNameLength / sizeof(wchar_t);
        if (!n) {
            name = pb + rdb->SubstituteNameOffset / sizeof(wchar_t);
            n = rdb->SubstituteNameLength / sizeof(wchar_t);
            if (n >= 4 && wcsncmp(name, L"\\??\\", 4) == 0) {
                name += 4;
                n -= 4;
            }
        }
        if ((ans = malloc(sizeof(wchar_t) * (n + 1)))) {
            memcpy(ans, name, n * sizeof(wchar_t));
            ans[n] = 0;
            strip_long_path_prefix(ans);
            *len = wcslen(ans);
        } else errno = ENOMEM;
    }
    free_keep_errno(rdb);
    return ans;
}

ssize_t
readlink(const char *path, char *buf, size_t bufsiz) {
    wchar_t *wpath = kitty_win32_wide_path(path), *target = NULL;
    size_t len = 0;
    ssize_t ans = -1;
    if (wpath && (target = read_symlink_target(wpath, &len))) {
        int n = len ? WideCharToMultiByte(CP_UTF8, 0, target, (int)len, NULL, 0, NULL, NULL) : 0;
        char *utf8 = n > 0 ? malloc(n) : NULL;
        if (len && !utf8) errno = n > 0 ? ENOMEM : EIO;
        else {
            if (n) WideCharToMultiByte(CP_UTF8, 0, target, (int)len, utf8, n, NULL, NULL);
            ans = (size_t)n > bufsiz ? (ssize_t)bufsiz : n;
            if (ans) memcpy(buf, utf8, ans);
            free(utf8);
        }
    }
    free_keep_errno(wpath);
    free_keep_errno(target);
    return ans;
}

static time_t
filetime_to_time_t(FILETIME ft) {
    const uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    // 100ns intervals since 1601-01-01 to seconds since 1970-01-01
    return (time_t)((int64_t)(t / 10000000ull) - 11644473600ll);
}

static bool
has_executable_extension(const wchar_t *path) {
    const wchar_t *dot = wcsrchr(path, L'.');
    if (!dot || wcschr(dot, L'\\') || wcschr(dot, L'/')) return false;
    return _wcsicmp(dot, L".exe") == 0 || _wcsicmp(dot, L".com") == 0 || _wcsicmp(dot, L".bat") == 0 || _wcsicmp(dot, L".cmd") == 0;
}

// stat() using the wide APIs, symlinks are followed
static int
stat_wide(const wchar_t *wpath, struct stat *st) {
    BY_HANDLE_FILE_INFORMATION info = {0};
    // FILE_FLAG_BACKUP_SEMANTICS is needed to open directories
    HANDLE h = CreateFileW(
        wpath, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        const DWORD type = GetFileType(h);
        if (type == FILE_TYPE_CHAR || type == FILE_TYPE_PIPE) {
            // devices such as NUL and CON, as reported by the C runtime
            CloseHandle(h);
            memset(st, 0, sizeof(*st));
            st->st_mode = (type == FILE_TYPE_CHAR ? S_IFCHR : S_IFIFO) | 0666;
            st->st_nlink = 1;
            return 0;
        }
        bool ok = GetFileInformationByHandle(h, &info);
        if (!ok) set_errno_from_last_error();
        CloseHandle(h);
        if (!ok) return -1;
    } else {
        // Files that cannot be opened at all, such as the page file, still have attributes
        DWORD err = GetLastError();
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if ((err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) || !GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad)) {
            SetLastError(err);
            set_errno_from_last_error();
            return -1;
        }
        info.dwFileAttributes = fad.dwFileAttributes;
        info.ftCreationTime = fad.ftCreationTime;
        info.ftLastAccessTime = fad.ftLastAccessTime;
        info.ftLastWriteTime = fad.ftLastWriteTime;
        info.nFileSizeHigh = fad.nFileSizeHigh;
        info.nFileSizeLow = fad.nFileSizeLow;
        info.nNumberOfLinks = 1;
    }
    memset(st, 0, sizeof(*st));
    // Permissions as reported by the C runtime: the owner's copied to group and others
    unsigned mode = S_IREAD;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) mode |= S_IWRITE;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) mode |= S_IFDIR | S_IEXEC;
    else mode |= S_IFREG | (has_executable_extension(wpath) ? S_IEXEC : 0);
    mode |= (mode & 0700) >> 3;
    mode |= (mode & 0700) >> 6;
    st->st_mode = (unsigned short)mode;
    st->st_nlink = (short)info.nNumberOfLinks;
    st->st_size = (off_t)(((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow);
    st->st_atime = filetime_to_time_t(info.ftLastAccessTime);
    st->st_mtime = filetime_to_time_t(info.ftLastWriteTime);
    st->st_ctime = filetime_to_time_t(info.ftCreationTime);
    st->st_dev = st->st_rdev = info.dwVolumeSerialNumber;
    return 0;
}

int
kitty_win32_stat(const char *path, struct stat *st) {
    wchar_t *wpath = kitty_win32_wide_path(path);
    if (!wpath) return -1;
    int ret = stat_wide(wpath, st);
    free_keep_errno(wpath);
    return ret;
}

int
kitty_win32_lstat(const char *path, struct stat *st) {
    wchar_t *wpath = kitty_win32_wide_path(path);
    if (!wpath) return -1;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad) && (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        size_t len = 0;
        wchar_t *target = read_symlink_target(wpath, &len);
        if (target) {
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFLNK | S_IRUSR | S_IWUSR;
            st->st_nlink = 1;
            st->st_size = len ? WideCharToMultiByte(CP_UTF8, 0, target, (int)len, NULL, 0, NULL, NULL) : 0;
            free(target);
            free(wpath);
            return 0;
        }
    }
    // not a symlink (junctions are followed, as before)
    int ret = stat_wide(wpath, st);
    free_keep_errno(wpath);
    return ret;
}

int
ttyname_r(int fd UNUSED, char *buf UNUSED, size_t buflen UNUSED) {
    return ENOTTY;
}

void
explicit_bzero(void *s, size_t n) {
    SecureZeroMemory(s, n);
}
// }}}

// sockets {{{
static SOCKET
socket_for_fd(int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return INVALID_SOCKET;
    }
    return (SOCKET)h;
}

static void
set_errno_from_wsa_error(void) {
    switch (WSAGetLastError()) {
        case WSAEINTR: errno = EINTR; break;
        case WSAEWOULDBLOCK: errno = EAGAIN; break;
        case WSAENOTSOCK: errno = ENOTSOCK; break;
        case WSAECONNRESET: errno = ECONNRESET; break;
        case WSAECONNREFUSED: errno = ECONNREFUSED; break;
        case WSAEADDRINUSE: errno = EADDRINUSE; break;
        case WSAEBADF: errno = EBADF; break;
        default: errno = EIO; break;
    }
}

ssize_t
kitty_win32_recv(int fd, void *buf, size_t len, int flags) {
    SOCKET s = socket_for_fd(fd);
    if (s == INVALID_SOCKET) return -1;
    int n = recv(s, buf, (int)len, flags);
    if (n == SOCKET_ERROR) {
        set_errno_from_wsa_error();
        return -1;
    }
    return n;
}

ssize_t
kitty_win32_send(int fd, const void *buf, size_t len, int flags) {
    SOCKET s = socket_for_fd(fd);
    if (s == INVALID_SOCKET) return -1;
    int n = send(s, buf, (int)len, flags);
    if (n == SOCKET_ERROR) {
        set_errno_from_wsa_error();
        return -1;
    }
    return n;
}

int
kitty_win32_accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    SOCKET s = socket_for_fd(fd);
    if (s == INVALID_SOCKET) return -1;
    SOCKET peer = accept(s, addr, addrlen);
    if (peer == INVALID_SOCKET) {
        set_errno_from_wsa_error();
        return -1;
    }
    int ans = _open_osfhandle((intptr_t)peer, _O_NOINHERIT);
    if (ans < 0) closesocket(peer);
    return ans;
}

int
kitty_win32_connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    SOCKET s = socket_for_fd(fd);
    if (s == INVALID_SOCKET) return -1;
    if (connect(s, addr, addrlen) == SOCKET_ERROR) {
        set_errno_from_wsa_error();
        return -1;
    }
    return 0;
}

int
kitty_win32_bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    SOCKET s = socket_for_fd(fd);
    if (s == INVALID_SOCKET) return -1;
    if (bind(s, addr, addrlen) == SOCKET_ERROR) {
        set_errno_from_wsa_error();
        return -1;
    }
    return 0;
}

int
kitty_win32_shutdown(int fd, int how) {
    SOCKET s = socket_for_fd(fd);
    if (s == INVALID_SOCKET) return -1;
    if (shutdown(s, how) == SOCKET_ERROR) {
        set_errno_from_wsa_error();
        return -1;
    }
    return 0;
}
// }}}

// signals {{{
int
sigemptyset(sigset_t *set) {
    *set = 0;
    return 0;
}

int
sigaddset(sigset_t *set, int signum) {
    if (signum <= 0 || signum >= 32) {
        errno = EINVAL;
        return -1;
    }
    *set |= 1u << signum;
    return 0;
}

int
sigprocmask(int how UNUSED, const sigset_t *set UNUSED, sigset_t *oldset) {
    if (oldset) *oldset = 0;
    return 0;
}

static struct sigaction console_signal_actions[32];
static bool console_ctrl_handler_installed = false;
static HANDLE reload_config_event = NULL, reload_config_wait = NULL;

static void CALLBACK
reload_config_callback(void *context UNUSED, BOOLEAN timed_out UNUSED) {
    kitty_win32_deliver_signal(SIGUSR1, (pid_t)GetCurrentProcessId(), 0);
}

bool
kitty_win32_start_reload_listener(void) {
    if (reload_config_event) return true;
    wchar_t name[64];
    swprintf(name, 64, L"Local\\kitsu-reload-config-%lu", GetCurrentProcessId());
    reload_config_event = CreateEventW(NULL, FALSE, FALSE, name);
    if (!reload_config_event) {
        set_errno_from_last_error();
        return false;
    }
    if (!RegisterWaitForSingleObject(&reload_config_wait, reload_config_event, reload_config_callback, NULL, INFINITE, WT_EXECUTEDEFAULT)) {
        set_errno_from_last_error();
        CloseHandle(reload_config_event);
        reload_config_event = NULL;
        return false;
    }
    return true;
}

void
kitty_win32_stop_reload_listener(void) {
    // Drain callbacks before the signal pipe is closed.
    if (reload_config_wait) UnregisterWaitEx(reload_config_wait, INVALID_HANDLE_VALUE);
    reload_config_wait = NULL;
    if (reload_config_event) CloseHandle(reload_config_event);
    reload_config_event = NULL;
}

void
kitty_win32_deliver_signal(int sig, pid_t pid, int status) {
    if (sig <= 0 || sig >= 32) return;
    struct sigaction *act = console_signal_actions + sig;
    if (act->sa_flags & SA_SIGINFO) {
        if (act->sa_sigaction) {
            siginfo_t si = {.si_signo = sig, .si_pid = pid, .si_status = status, .si_code = sig == SIGCHLD ? CLD_EXITED : 0};
            act->sa_sigaction(sig, &si, NULL);
        }
    } else if (act->sa_handler && act->sa_handler != SIG_IGN && act->sa_handler != SIG_DFL) act->sa_handler(sig);
}

static void
deliver_console_signal(int sig) {
    kitty_win32_deliver_signal(sig, (pid_t)GetCurrentProcessId(), 0);
}

static BOOL WINAPI
console_ctrl_handler(DWORD ctrl_type) {
    int sig;
    switch (ctrl_type) {
        case CTRL_C_EVENT: sig = SIGINT; break;
        case CTRL_BREAK_EVENT: sig = SIGTERM; break;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: sig = SIGHUP; break;
        default: return FALSE;
    }
    struct sigaction *act = console_signal_actions + sig;
    if (act->sa_handler == SIG_DFL && !(act->sa_flags & SA_SIGINFO)) return FALSE;
    deliver_console_signal(sig);
    return TRUE;
}

int
sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (signum <= 0 || signum >= 32) {
        errno = EINVAL;
        return -1;
    }
    if (oldact) *oldact = console_signal_actions[signum];
    if (act) {
        console_signal_actions[signum] = *act;
        if (!console_ctrl_handler_installed) {
            SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
            console_ctrl_handler_installed = true;
        }
    }
    return 0;
}
// }}}

// files {{{
int
kitty_win32_open_readonly_shared(const char *path) {
    wchar_t *wpath = kitty_win32_wide_path(path);
    if (!wpath) return -1;
    // The path comes from a client, so: should it name a named pipe, its
    // server may only identify us, not impersonate us. Do not fetch files
    // from offline storage, such as cloud files that are not on disk. And
    // only read actual files, not pipes or devices such as CON.
    HANDLE h = CreateFileW(
        wpath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_NO_RECALL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        NULL);
    if (h == INVALID_HANDLE_VALUE) set_errno_from_last_error();
    free_keep_errno(wpath);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (GetFileType(h) != FILE_TYPE_DISK) {
        CloseHandle(h);
        errno = EACCES;
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) {
        CloseHandle(h);
        errno = EMFILE;
    }
    return fd;
}

FILE *
kitty_win32_fopen(const char *path, const char *mode) {
    wchar_t *wpath = kitty_win32_wide_path(path), *wmode = NULL;
    FILE *ans = NULL;
    if (wpath && (wmode = kitty_win32_utf8_to_wide(mode))) ans = _wfopen(wpath, wmode);
    free_keep_errno(wpath);
    free_keep_errno(wmode);
    return ans;
}

int
kitty_win32_access(const char *path, int mode) {
    wchar_t *wpath = kitty_win32_wide_path(path);
    if (!wpath) return -1;
    // The C runtime rejects X_OK, Windows has no execute permission
    int ret = _waccess(wpath, mode & (R_OK | W_OK));
    free_keep_errno(wpath);
    return ret;
}

int
kitty_win32_unlink(const char *path) {
    wchar_t *wpath = kitty_win32_wide_path(path);
    if (!wpath) return -1;
    int ret = _wunlink(wpath);
    free_keep_errno(wpath);
    return ret;
}

char *
kitty_win32_getcwd(char *buf, size_t size) {
    if (buf && !size) {
        errno = EINVAL;
        return NULL;
    }
    DWORD n = GetCurrentDirectoryW(0, NULL);
    wchar_t *wcwd = n ? malloc(sizeof(wchar_t) * n) : NULL;
    if (!wcwd) {
        if (n) errno = ENOMEM;
        else set_errno_from_last_error();
        return NULL;
    }
    DWORD m = GetCurrentDirectoryW(n, wcwd);
    char *cwd = NULL;
    if (!m || m >= n) {
        if (m) errno = ERANGE; // changed by another thread between the calls
        else set_errno_from_last_error();
    } else {
        strip_long_path_prefix(wcwd);
        cwd = kitty_win32_wide_to_utf8(wcwd);
    }
    free_keep_errno(wcwd);
    if (!cwd) return NULL;
    const size_t len = strlen(cwd) + 1;
    if (size && size < len) {
        free(cwd);
        errno = ERANGE;
        return NULL;
    }
    if (!buf) return cwd;
    memcpy(buf, cwd, len);
    free(cwd);
    return buf;
}

void *
kitty_win32_map_file(const char *path, size_t *size) {
    wchar_t *wpath = kitty_win32_wide_path(path);
    if (!wpath) return NULL;
    // Writers are locked out as changing a mapped file under its user can crash it
    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) set_errno_from_last_error();
    free_keep_errno(wpath);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    void *addr = NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) set_errno_from_last_error();
    else if (!sz.QuadPart || (uint64_t)sz.QuadPart > SIZE_MAX) errno = EINVAL; // empty files cannot be mapped
    else {
        HANDLE mapping = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
        // the view keeps the mapping alive
        if (mapping) addr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (!addr) set_errno_from_last_error();
        if (mapping) CloseHandle(mapping);
        if (addr) *size = (size_t)sz.QuadPart;
    }
    int saved = errno;
    CloseHandle(h);
    errno = saved;
    return addr;
}

void
kitty_win32_unmap_file(void *addr) {
    if (addr) UnmapViewOfFile(addr);
}

struct kitty_win32_DIR {
    HANDLE find;
    bool started, done;
    WIN32_FIND_DATAW data;
    struct kitty_win32_dirent entry;
};

kitty_win32_DIR *
kitty_win32_opendir(const char *path) {
    wchar_t *wpath = kitty_win32_utf8_to_wide(path), *pattern = NULL;
    kitty_win32_DIR *ans = NULL;
    if (!wpath) return NULL;
    const size_t len = wcslen(wpath);
    const bool has_sep = len && (wpath[len - 1] == L'\\' || wpath[len - 1] == L'/' || wpath[len - 1] == L':');
    if (!(pattern = extend_path(join_wide(wpath, has_sep ? NULL : L"\\", L"*")))) goto end;
    if (!(ans = calloc(1, sizeof(kitty_win32_DIR)))) {
        errno = ENOMEM;
        goto end;
    }
    ans->find = FindFirstFileExW(pattern, FindExInfoBasic, &ans->data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (ans->find == INVALID_HANDLE_VALUE) {
        // an empty root directory does not even have . and ..
        if (GetLastError() == ERROR_FILE_NOT_FOUND) ans->done = true;
        else {
            set_errno_from_last_error();
            free(ans);
            ans = NULL;
        }
    }
end:
    free_keep_errno(wpath);
    free_keep_errno(pattern);
    return ans;
}

struct kitty_win32_dirent *
kitty_win32_readdir(kitty_win32_DIR *dir) {
    while (!dir->done) {
        if (dir->started && !FindNextFileW(dir->find, &dir->data)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) set_errno_from_last_error();
            dir->done = true;
            break;
        }
        dir->started = true;
        // names are at most MAX_PATH UTF-16 units, so this cannot fail in practice
        if (WideCharToMultiByte(CP_UTF8, 0, dir->data.cFileName, -1, dir->entry.d_name, sizeof(dir->entry.d_name), NULL, NULL)) return &dir->entry;
    }
    return NULL;
}

int
kitty_win32_closedir(kitty_win32_DIR *dir) {
    if (!dir) {
        errno = EBADF;
        return -1;
    }
    if (dir->find != INVALID_HANDLE_VALUE) FindClose(dir->find);
    free(dir);
    return 0;
}

// Creates a uniquely named file in dir (which must end with a path separator)
// that the OS deletes when its last handle is closed, which also happens if
// the process crashes. It is opened without sharing, so no other process can
// open or delete it while it is in use.
static int
open_delete_on_close_file(const wchar_t *dir, const wchar_t *prefix, DWORD attributes) {
    static const wchar_t chars[] = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int attempt = 0; attempt < 100; attempt++) {
        unsigned char rnd[12];
        if (!secure_random_bytes_win32(rnd, sizeof(rnd))) {
            errno = EIO;
            return -1;
        }
        wchar_t suffix[sizeof(rnd) + 1];
        for (size_t i = 0; i < sizeof(rnd); i++) suffix[i] = chars[rnd[i] % (sizeof(chars) / sizeof(chars[0]) - 1)];
        suffix[sizeof(rnd)] = 0;
        wchar_t *path = extend_path(join_wide(dir, prefix, suffix));
        if (!path) return -1;
        HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, attributes | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        DWORD err = GetLastError();
        free(path);
        if (h == INVALID_HANDLE_VALUE) {
            if (err == ERROR_FILE_EXISTS) continue;
            SetLastError(err);
            set_errno_from_last_error();
            return -1;
        }
        int fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY | _O_NOINHERIT);
        if (fd < 0) {
            CloseHandle(h);
            errno = EMFILE;
        }
        return fd;
    }
    errno = EEXIST;
    return -1;
}

int
kitty_win32_open_anonymous_tmpfile(void) {
    wchar_t *dir = temp_dir();
    if (!dir) return -1;
    int fd = open_delete_on_close_file(dir, L"kitty-tmp-", FILE_ATTRIBUTE_TEMPORARY);
    free_keep_errno(dir);
    return fd;
}

// Converts UTF-8 dir to a wide path ending with a path separator
static wchar_t *
wide_dir_path(const char *dir) {
    wchar_t *w = kitty_win32_utf8_to_wide(dir);
    if (!w) return NULL;
    const size_t n = wcslen(w);
    if (!n || w[n - 1] == L'\\' || w[n - 1] == L'/') return w;
    wchar_t *ans = join_wide(w, L"\\", NULL);
    free_keep_errno(w);
    return ans;
}

int
kitty_win32_open_delete_on_close_file(const char *dir, const char *prefix) {
    wchar_t *wdir = wide_dir_path(dir), *wprefix = NULL;
    int fd = -1;
    if (wdir && (wprefix = kitty_win32_utf8_to_wide(prefix))) fd = open_delete_on_close_file(wdir, wprefix, FILE_ATTRIBUTE_NORMAL);
    free_keep_errno(wdir);
    free_keep_errno(wprefix);
    return fd;
}

int
kitty_win32_delete_unused_files(const char *dir, const char *prefix) {
    wchar_t *wdir = wide_dir_path(dir), *wprefix = NULL, *pattern = NULL;
    int count = -1;
    if (!wdir || !(wprefix = kitty_win32_utf8_to_wide(prefix)) || !(pattern = extend_path(join_wide(wdir, wprefix, L"*")))) goto end;
    const size_t prefixlen = wcslen(wprefix);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) count = 0;
        else set_errno_from_last_error();
        goto end;
    }
    count = 0;
    do {
        // the wildcard can also match 8.3 short names, so check the real name
        if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || wcsncmp(fd.cFileName, wprefix, prefixlen) != 0) continue;
        wchar_t *path = extend_path(join_wide(wdir, fd.cFileName, NULL));
        // A file that is open in some process fails to delete, as it was not
        // opened with FILE_SHARE_DELETE
        if (path && DeleteFileW(path)) count++;
        free(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
end:
    free_keep_errno(wdir);
    free_keep_errno(wprefix);
    free_keep_errno(pattern);
    return count;
}

int
kitty_win32_path_from_fd(int fd, char *buf, size_t bufsz) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    wchar_t *wpath = final_path_of_handle(h);
    if (!wpath) return -1;
    int ok = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, buf, (int)bufsz, NULL, NULL);
    free(wpath);
    if (!ok) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static bool
token_sid_matches(HANDLE token, TOKEN_INFORMATION_CLASS cls, PSID sid, bool *matches) {
    DWORD sz = 0;
    GetTokenInformation(token, cls, NULL, 0, &sz);
    if (!sz) {
        set_errno_from_last_error();
        return false;
    }
    void *info = malloc(sz);
    if (!info) {
        errno = ENOMEM;
        return false;
    }
    if (!GetTokenInformation(token, cls, info, sz, &sz)) {
        set_errno_from_last_error();
        free(info);
        return false;
    }
    // TOKEN_USER and TOKEN_OWNER both start with a PSID
    PSID token_sid = cls == TokenUser ? ((TOKEN_USER *)info)->User.Sid : ((TOKEN_OWNER *)info)->Owner;
    *matches = EqualSid(sid, token_sid);
    free(info);
    return true;
}

int
kitty_win32_fd_owned_by_current_user(int fd, bool *owned) {
    *owned = false;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    PSID owner = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD r = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd);
    if (r != ERROR_SUCCESS) {
        SetLastError(r);
        set_errno_from_last_error();
        return -1;
    }
    HANDLE token = NULL;
    int ret = -1;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        // Objects created by elevated administrators are owned by the
        // Administrators group rather than the user, so accept the token's
        // default owner as well as its user.
        bool is_user = false, is_default_owner = false;
        if (token_sid_matches(token, TokenUser, owner, &is_user) && token_sid_matches(token, TokenOwner, owner, &is_default_owner)) {
            *owned = is_user || is_default_owner;
            ret = 0;
        }
        CloseHandle(token);
    } else set_errno_from_last_error();
    LocalFree(sd);
    return ret;
}

size_t
kitty_win32_num_logged_in_users(void) {
    size_t users = 0;
    PWTS_SESSION_INFOW sessions = NULL;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return 0;
    for (DWORD i = 0; i < count; i++) {
        if (sessions[i].State != WTSActive) continue;
        LPWSTR user = NULL;
        DWORD sz = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId, WTSUserName, &user, &sz)) {
            if (user && user[0]) users++;
            WTSFreeMemory(user);
        }
    }
    WTSFreeMemory(sessions);
    return users;
}
// }}}

// threads {{{
bool
kitty_win32_set_current_thread_name(const char *name) {
    wchar_t wname[64] = {0};
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, (int)(sizeof(wname) / sizeof(wname[0])) - 1)) return false;
    return SUCCEEDED(SetThreadDescription(GetCurrentThread(), wname));
}
// }}}

// processes {{{
int
kill(pid_t pid, int sig) {
    if (pid <= 0) {
        errno = ESRCH;
        return -1;
    }
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) {
        errno = GetLastError() == ERROR_ACCESS_DENIED ? EPERM : ESRCH;
        return -1;
    }
    int ret = 0;
    switch (sig) {
        case 0: break;
        case SIGKILL:
        case SIGTERM:
        case SIGHUP:
        case SIGINT:
            if (!TerminateProcess(h, 128 + sig)) {
                DWORD err = GetLastError();
                // TerminateProcess() on an already exited process fails with ERROR_ACCESS_DENIED
                if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) errno = ESRCH;
                else errno = err == ERROR_ACCESS_DENIED ? EPERM : EINVAL;
                ret = -1;
            }
            break;
        default:
            errno = ENOSYS;
            ret = -1;
            break;
    }
    CloseHandle(h);
    return ret;
}

pid_t
waitpid(pid_t pid, int *status, int options) {
    if (pid <= 0) {
        // Reaping arbitrary children is not possible on Windows, the child
        // monitor tracks process handles explicitly instead.
        errno = ECHILD;
        return -1;
    }
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) {
        errno = ECHILD;
        return -1;
    }
    DWORD wait = WaitForSingleObject(h, (options & WNOHANG) ? 0 : INFINITE);
    if (wait == WAIT_TIMEOUT) {
        CloseHandle(h);
        return 0;
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(h, &exit_code);
    CloseHandle(h);
    if (status) *status = (int)((exit_code & 0xff) << 8);
    return pid;
}
// }}}

void
kitty_win32_beep(void) {
    MessageBeep(MB_OK);
}

// console {{{
// A single fd refers to CONIN$, the output side is looked up via
// GetStdHandle() since console modes are per console, not per handle.
int
kitty_win32_open_console(int flags) {
    HANDLE h = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_last_error();
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, (flags & O_CLOEXEC) ? _O_NOINHERIT : 0);
    if (fd < 0) CloseHandle(h);
    return fd;
}

static HANDLE
console_output_handle(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) return h;
    h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) return h;
    return INVALID_HANDLE_VALUE;
}

int
tcgetattr(int fd, struct termios *t) {
    HANDLE in = (HANDLE)_get_osfhandle(fd);
    DWORD mode = 0;
    if (in == INVALID_HANDLE_VALUE || !GetConsoleMode(in, &mode)) {
        errno = ENOTTY;
        return -1;
    }
    t->input_mode = mode;
    t->output_mode = 0;
    HANDLE out = console_output_handle();
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) t->output_mode = mode;
    t->read_with_timeout = false;
    return 0;
}

int
tcsetattr(int fd, int optional_actions, const struct termios *t) {
    HANDLE in = (HANDLE)_get_osfhandle(fd);
    if (in == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (optional_actions == TCSAFLUSH) FlushConsoleInputBuffer(in);
    if (!SetConsoleMode(in, (DWORD)t->input_mode)) {
        errno = ENOTTY;
        return -1;
    }
    HANDLE out = console_output_handle();
    if (out != INVALID_HANDLE_VALUE && t->output_mode) SetConsoleMode(out, (DWORD)t->output_mode);
    return 0;
}

int
kitty_win32_console_raw_mode(int fd, const struct termios *saved, bool read_with_timeout) {
    struct termios raw = *saved;
    raw.input_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT);
    raw.input_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (raw.output_mode) {
        raw.output_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;
        raw.output_mode &= ~ENABLE_WRAP_AT_EOL_OUTPUT;
    }
    raw.read_with_timeout = read_with_timeout;
    return tcsetattr(fd, TCSAFLUSH, &raw);
}
// }}}

// stdlib {{{
int
posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (!alignment || (alignment & (alignment - 1)) || alignment % sizeof(void *)) return EINVAL;
    void *p = _aligned_malloc(size ? size : 1, alignment);
    if (!p) return ENOMEM;
    *memptr = p;
    return 0;
}

// The C runtime's narrow environment is in the ANSI code page, so values are
// read with the wide API and the UTF-8 strings are kept around, per name, for
// the lifetime of the process.
static struct {
    struct {
        char *name, *value;
    } *items;
    size_t count, capacity;
    SRWLOCK lock;
} env_cache = {.lock = SRWLOCK_INIT};

static char *
cached_env_value(const char *name, char *value) {
    char *ans = value;
    AcquireSRWLockExclusive(&env_cache.lock);
    size_t i = 0;
    while (i < env_cache.count && strcmp(env_cache.items[i].name, name) != 0) i++;
    if (i < env_cache.count) {
        if (strcmp(env_cache.items[i].value, value) == 0) {
            ans = env_cache.items[i].value;
            free(value);
        } else env_cache.items[i].value = value; // the old value is leaked, callers may still use it
    } else {
        if (env_cache.count >= env_cache.capacity) {
            size_t cap = env_cache.capacity ? env_cache.capacity * 2 : 16;
            void *items = realloc(env_cache.items, cap * sizeof(env_cache.items[0]));
            if (items) {
                env_cache.items = items;
                env_cache.capacity = cap;
            }
        }
        char *n = env_cache.count < env_cache.capacity ? strdup(name) : NULL;
        if (n) {
            env_cache.items[env_cache.count].name = n;
            env_cache.items[env_cache.count++].value = value;
        } // else out of memory, leak value
    }
    ReleaseSRWLockExclusive(&env_cache.lock);
    return ans;
}

char *
kitty_win32_getenv(const char *name) {
    wchar_t *wname = kitty_win32_utf8_to_wide(name), *wvalue = NULL;
    char *value = NULL;
    if (!wname) return NULL;
    DWORD n = GetEnvironmentVariableW(wname, NULL, 0); // includes the NUL, so 0 means unset
    while (n) {
        wchar_t *buf = realloc(wvalue, sizeof(wchar_t) * n);
        if (!buf) {
            errno = ENOMEM;
            break;
        }
        wvalue = buf;
        SetLastError(ERROR_SUCCESS);
        DWORD m = GetEnvironmentVariableW(wname, wvalue, n);
        if (m < n) {
            // m is 0 for an empty value, or if the variable was removed meanwhile
            if (m || GetLastError() != ERROR_ENVVAR_NOT_FOUND) value = kitty_win32_wide_to_utf8(wvalue);
            break;
        }
        n = m; // the value grew meanwhile
    }
    free(wname);
    free(wvalue);
    return value ? cached_env_value(name, value) : NULL;
}

static int
putenv_utf8(const char *name, const char *value) {
    wchar_t *wname = kitty_win32_utf8_to_wide(name), *wvalue = NULL;
    int ret = -1;
    if (wname && (wvalue = kitty_win32_utf8_to_wide(value))) {
        errno_t err = _wputenv_s(wname, wvalue);
        if (err) errno = err;
        else ret = 0;
    }
    free_keep_errno(wname);
    free_keep_errno(wvalue);
    return ret;
}

int
setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && kitty_win32_getenv(name)) return 0;
    return putenv_utf8(name, value);
}

int
unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    return putenv_utf8(name, "");
}

char *
realpath(const char *path, char *resolved_path) {
    wchar_t *wpath = kitty_win32_wide_path(path), *final = NULL;
    if (!wpath) return NULL;
    // Opening the file resolves symlinks and junctions and fails if it does not exist
    HANDLE h = CreateFileW(
        wpath, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) set_errno_from_last_error();
    else {
        final = final_path_of_handle(h);
        CloseHandle(h);
    }
    free_keep_errno(wpath);
    if (!final) return NULL;
    char *ans = kitty_win32_wide_to_utf8(final);
    free_keep_errno(final);
    if (!ans) return NULL;
    for (char *p = ans; *p; p++)
        if (*p == '\\') *p = '/';
    if (!resolved_path) return ans;
    const size_t len = strlen(ans) + 1;
    if (len > PATH_MAX) {
        free(ans);
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(resolved_path, ans, len);
    free(ans);
    return resolved_path;
}

bool
win32_exe_path(char *buf, size_t buf_sz) {
    wchar_t *exe = module_file_name(NULL);
    if (!exe) return false;
    strip_long_path_prefix(exe);
    int n = WideCharToMultiByte(CP_UTF8, 0, exe, -1, buf, (int)buf_sz, NULL, NULL);
    free(exe);
    if (n <= 0) return false;
    for (char *p = buf; *p; p++)
        if (*p == '\\') *p = '/';
    return true;
}

#define emit(ch)                    \
    {                               \
        if (*pos + 1 >= buf_sz) {   \
            errno = E2BIG;          \
            return false;           \
        }                           \
        buf[(*pos)++] = ch;         \
    }

bool
kitty_win32_append_quoted_arg(char *buf, size_t buf_sz, size_t *pos, const char *arg) {
    // Quoting rules for CommandLineToArgvW()
    bool needs_quotes = !*arg || strpbrk(arg, " \t\"") != NULL;
    if (*pos) emit(' ');
    if (needs_quotes) emit('"');
    size_t backslashes = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == '\\') {
            backslashes++;
            continue;
        }
        if (*p == '"') {
            for (size_t i = 0; i < 2 * backslashes + 1; i++) emit('\\');
        } else {
            for (size_t i = 0; i < backslashes; i++) emit('\\');
        }
        backslashes = 0;
        emit(*p);
    }
    for (size_t i = 0; i < (needs_quotes ? 2 * backslashes : backslashes); i++) emit('\\');
    if (needs_quotes) emit('"');
    buf[*pos] = 0;
    return true;
}

static bool
is_safe_unquoted_batch_char(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || (ch && strchr("#$*+-./:?@\\_", ch));
}

bool
kitty_win32_is_batch_file(const wchar_t *path) {
    // CreateProcess() runs files with a .bat or .cmd extension via cmd.exe,
    // also when the name has trailing dots or spaces, which Windows strips
    // from file names unless the path is a \\?\ one.
    size_t n = wcslen(path);
    if (wcsncmp(path, L"\\\\?\\", 4) != 0)
        while (n && (path[n - 1] == L'.' || path[n - 1] == L' ')) n--;
    return n >= 4 && (_wcsnicmp(path + n - 4, L".bat", 4) == 0 || _wcsnicmp(path + n - 4, L".cmd", 4) == 0);
}

bool
kitty_win32_append_batch_arg(char *buf, size_t buf_sz, size_t *pos, const char *arg) {
    // cmd.exe parses the command line of batch files itself, so the
    // CommandLineToArgvW() rules do not protect their arguments. Outside
    // double quotes & | < > ^ ( ) are metacharacters and %var% is expanded
    // even inside them, allowing command injection (CVE-2024-24576, aka
    // BatBadBut). This follows the fix in the Rust standard library: quote
    // every argument with anything but known safe characters, escape embedded
    // quotes by doubling them, which keeps cmd.exe inside the quoted region,
    // and turn every % into the zero length substring %%cd:~,% so that no
    // variable name can be formed. That needs command extensions, and
    // delayed expansion disabled so that ! is not special, see
    // KITTY_WIN32_BATCH_PREFIX. CR and LF end the command and cannot be
    // escaped at all.
    if (strpbrk(arg, "\r\n")) {
        errno = EINVAL;
        return false;
    }
    // Quote empty arguments so they are not dropped and those ending with a
    // backslash so that it does not escape the closing quote of "%~1"
    bool needs_quotes = !*arg || arg[strlen(arg) - 1] == '\\';
    for (const unsigned char *p = (const unsigned char *)arg; *p && !needs_quotes; p++) {
        if (*p < 0x80) needs_quotes = !is_safe_unquoted_batch_char(*p);
        else if (*p == 0xc2 && p[1] >= 0x80 && p[1] < 0xa0) needs_quotes = true; // C1 control characters
    }
    if (*pos) emit(' ');
    if (needs_quotes) emit('"');
    size_t backslashes = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == '\\') {
            backslashes++;
        } else {
            if (*p == '"') {
                // n backslashes before a quote become 2n, followed by ""
                for (size_t i = 0; i < backslashes; i++) emit('\\');
                emit('"');
            } else if (*p == '%') {
                for (const char *q = "%%cd:~,"; *q; q++) emit(*q);
            }
            backslashes = 0;
        }
        emit(*p);
    }
    if (needs_quotes) {
        for (size_t i = 0; i < backslashes; i++) emit('\\');
        emit('"');
    }
    buf[*pos] = 0;
    return true;
}
#undef emit

// The command line for CreateProcessW() running argv0 with the arguments argv[1:]
static wchar_t *
wide_command_line(const char *argv0, char *const argv[]) {
    // quoting at most doubles an argument and adds a space and two quotes
    size_t sz = 2 * strlen(argv0) + 4, pos = 0;
    for (int i = 1; argv[i]; i++) sz += 2 * strlen(argv[i]) + 4;
    char *cmdline = malloc(sz);
    if (!cmdline) {
        errno = ENOMEM;
        return NULL;
    }
    bool ok = kitty_win32_append_quoted_arg(cmdline, sz, &pos, argv0);
    for (int i = 1; ok && argv[i]; i++) ok = kitty_win32_append_quoted_arg(cmdline, sz, &pos, argv[i]);
    wchar_t *ans = ok ? kitty_win32_utf8_to_wide(cmdline) : NULL;
    if (!ok) errno = E2BIG;
    free_keep_errno(cmdline);
    // the limit of CreateProcessW()
    if (ans && wcslen(ans) >= 32767) {
        free(ans);
        errno = E2BIG;
        return NULL;
    }
    return ans;
}

bool
win32_add_dll_dir_of_module(const wchar_t *module_name) {
    // Extension modules are loaded with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS which
    // ignores PATH, so DLLs next to libpython (zlib1.dll, etc.) are only found
    // if that directory is explicitly registered.
    HMODULE mod = GetModuleHandleW(module_name);
    if (!mod) return false;
    wchar_t *path = module_file_name(mod);
    if (!path) return false;
    wchar_t *sep = wcsrchr(path, L'\\');
    bool ok = false;
    if (sep) {
        *sep = 0;
        ok = AddDllDirectory(path) != NULL;
    }
    free(path);
    return ok;
}

bool
win32_spawn_detached(char *const argv[]) {
    char exe[PATH_MAX + 1] = {0};
    wchar_t *wexe = NULL, *cmdline = NULL;
    bool ok = false;
    if (win32_exe_path(exe, sizeof(exe)) && (wexe = module_file_name(NULL)) && (cmdline = wide_command_line(exe, argv))) {
        STARTUPINFOW si = {.cb = sizeof(si)};
        PROCESS_INFORMATION pi = {0};
        if (CreateProcessW(wexe, cmdline, NULL, NULL, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            ok = true;
        } else set_errno_from_last_error();
    }
    free_keep_errno(wexe);
    free_keep_errno(cmdline);
    return ok;
}

static BOOL WINAPI
ignore_console_ctrl_event(DWORD type UNUSED) {
    return TRUE;
}

int
win32_spawn_and_wait(const char *exe, char *const argv[]) {
    wchar_t *wexe = kitty_win32_wide_path(exe), *cmdline = NULL;
    if (!wexe || !(cmdline = wide_command_line(exe, argv))) {
        free_keep_errno(wexe);
        return -1;
    }
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    // Ctrl+C is delivered to every process attached to the console, let the
    // child deal with it and keep waiting for its exit code.
    SetConsoleCtrlHandler(ignore_console_ctrl_event, TRUE);
    bool ok = CreateProcessW(wexe, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    if (!ok) set_errno_from_last_error();
    free_keep_errno(wexe);
    free_keep_errno(cmdline);
    if (!ok) {
        SetConsoleCtrlHandler(ignore_console_ctrl_event, FALSE);
        return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    SetConsoleCtrlHandler(ignore_console_ctrl_event, FALSE);
    return (int)code;
}

static HANDLE
valid_std_handle(DWORD which) {
    HANDLE h = GetStdHandle(which);
    return (h == INVALID_HANDLE_VALUE) ? NULL : h;
}

bool
win32_attach_parent_console(void) {
    const DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE inherited[3];
    for (int i = 0; i < 3; i++) inherited[i] = valid_std_handle(ids[i]);
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    // AttachConsole() points all three std handles at the console, keep the
    // ones the parent explicitly redirected (pipes, files, its own console).
    for (int i = 0; i < 3; i++) {
        if (inherited[i]) SetStdHandle(ids[i], inherited[i]);
    }
    // The C runtime bound the std streams at startup, when the handles were
    // missing, so re-open those that had nothing to bind to on the console.
    if (!inherited[0]) freopen("CONIN$", "r", stdin);
    if (!inherited[1]) freopen("CONOUT$", "w", stdout);
    if (!inherited[2]) freopen("CONOUT$", "w", stderr);
    return true;
}
// }}}

// GUI diagnostics {{{
char *
win32_default_log_path(void) {
    const char *local = kitty_win32_getenv("LOCALAPPDATA");
    wchar_t folder[MAX_PATH];
    wchar_t *base = local && local[0] ? kitty_win32_utf8_to_wide(local) : NULL;
    if (!base && SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, folder))) base = _wcsdup(folder);
    wchar_t *path = base ? join_wide(base, L"\\Kitsu\\logs\\kitsu.log", NULL) : NULL;
    char *ans = path ? kitty_win32_wide_to_utf8(path) : NULL;
    free(base);
    free(path);
    return ans;
}

static HANDLE
stderr_handle(void) {
    int fd = fileno(stderr);
    return fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)_get_osfhandle(fd);
}

static bool
invisible_stderr(void) {
    HANDLE h = stderr_handle();
    DWORD mode;
    if (h == INVALID_HANDLE_VALUE || !h) return true;
    DWORD type = GetFileType(h);
    return type != FILE_TYPE_DISK && type != FILE_TYPE_PIPE && !GetConsoleMode(h, &mode);
}

bool
win32_redirect_log(const char *path, bool stdout_too) {
    FILE *file = kitty_win32_fopen(path, "ab");
    if (!file) return false;
    // With no inherited handles freopen(NUL) can allocate fds above 2. Keep
    // both the CRT streams and raw writes to the standard descriptors working.
    bool ok = _dup2(fileno(file), fileno(stderr)) == 0 && _dup2(fileno(stderr), 2) == 0;
    fclose(file);
    if (ok) {
        setvbuf(stderr, NULL, _IONBF, 0);
        SetStdHandle(STD_ERROR_HANDLE, stderr_handle());
        if (stdout_too) {
            ok = _dup2(fileno(stderr), fileno(stdout)) == 0 && _dup2(fileno(stdout), 1) == 0;
            if (ok) {
                setvbuf(stdout, NULL, _IONBF, 0);
                SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(fileno(stdout)));
            }
        }
    }
    return ok;
}

void
win32_setup_gui_logging(void) {
    if (!invisible_stderr()) return;
    char *path = win32_default_log_path();
    wchar_t *wpath = path ? kitty_win32_wide_path(path) : NULL;
    if (wpath) {
        wchar_t *sep = wcsrchr(wpath, L'\\');
        if (sep) {
            *sep = 0;
            wchar_t *parent = wcsrchr(wpath, L'\\');
            if (parent) {
                *parent = 0;
                CreateDirectoryW(wpath, NULL);
                *parent = L'\\';
            }
            CreateDirectoryW(wpath, NULL);
            *sep = L'\\';
        }
        // Rotate at startup, keeping one previous log. An open log in another
        // instance may prevent rotation; never discard it in that case.
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExW(wpath, GetFileExInfoStandard, &info) && (info.nFileSizeHigh || info.nFileSizeLow >= 1024 * 1024)) {
            wchar_t *old = join_wide(wpath, L".1", NULL);
            if (old) MoveFileExW(wpath, old, MOVEFILE_REPLACE_EXISTING);
            free(old);
        }
        win32_redirect_log(path, false);
    }
    free(wpath);
    free(path);
}

void
win32_debug_log(const char *message) {
    wchar_t *wide = kitty_win32_utf8_to_wide(message);
    if (wide) {
        OutputDebugStringW(wide);
        OutputDebugStringW(L"\n");
        free(wide);
    }
}

void
win32_report_fatal_error(void) {
    char *path = win32_default_log_path();
    wchar_t *wpath = path ? kitty_win32_wide_path(path) : NULL;
    bool show = invisible_stderr(), logged = false;
    if (wpath && !show) {
        HANDLE file = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION a, b;
            logged = GetFileInformationByHandle(file, &a) && GetFileInformationByHandle(stderr_handle(), &b) &&
                a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
            CloseHandle(file);
        }
        show = logged;
    }
    if (show) {
        fflush(stderr);
        wchar_t *display = path ? kitty_win32_utf8_to_wide(path) : NULL;
        wchar_t *message = join_wide(
            logged ? L"Kitsu encountered a fatal error. Details have been written to:\n\n" :
                L"Kitsu encountered a fatal error and could not write its diagnostic log. Expected log location:\n\n",
            display ? display : L"%LOCALAPPDATA%\\Kitsu\\logs\\kitsu.log", NULL);
        MessageBoxW(NULL, message ? message : L"Kitsu encountered a fatal error.", L"Kitsu error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        free(message);
        free(display);
    }
    free(wpath);
    free(path);
}
// }}}

// misc {{{

int
fsync(int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (!FlushFileBuffers(h)) {
        set_errno_from_last_error();
        return -1;
    }
    return 0;
}

bool
secure_random_bytes_win32(void *buf, size_t nbytes) {
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)nbytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

void
win32_compat_init(void) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleOutputCP(CP_UTF8);
}
// }}}

#endif
