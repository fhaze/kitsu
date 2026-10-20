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
#undef pollfd
#include <windows.h>
#include <winioctl.h>
#include <aclapi.h>
#include <wtsapi32.h>
#include <bcrypt.h>
#include <direct.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#define UNUSED __attribute__((unused))

static void set_errno_from_wsa_error(void);
void (*kitty_win32_on_fd_close)(int fd) = NULL;

void
set_errno_from_last_error(void) {
    switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_MOD_NOT_FOUND: errno = ENOENT; break;
        case ERROR_ACCESS_DENIED: errno = EACCES; break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY: errno = ENOMEM; break;
        case ERROR_INVALID_HANDLE: errno = EBADF; break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: errno = EEXIST; break;
        default: errno = EINVAL; break;
    }
}

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
static bool
shm_path(const char *name, char *buf, size_t bufsz) {
    while (*name == '/') name++;
    if (!*name || strchr(name, '/') || strchr(name, '\\')) {
        errno = EINVAL;
        return false;
    }
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(tmp), tmp);
    if (!n || n >= sizeof(tmp)) {
        errno = ENOENT;
        return false;
    }
    int written = snprintf(buf, bufsz, "%skitty-shm", tmp);
    if (written < 0 || (size_t)written >= bufsz) {
        errno = ENAMETOOLONG;
        return false;
    }
    _mkdir(buf);
    written = snprintf(buf, bufsz, "%skitty-shm\\%s", tmp, name);
    if (written < 0 || (size_t)written >= bufsz) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

// Opened with FILE_SHARE_DELETE so that a mapped object can be unlinked by its
// creator while other processes still have it open, as with POSIX shm
int
shm_open(const char *name, int oflag, mode_t mode UNUSED) {
    char path[MAX_PATH];
    if (!shm_path(name, path, sizeof(path))) return -1;
    DWORD access = GENERIC_READ, disposition = OPEN_EXISTING;
    if (oflag & (O_WRONLY | O_RDWR)) access |= GENERIC_WRITE;
    if (oflag & O_CREAT) disposition = (oflag & O_EXCL) ? CREATE_NEW : (oflag & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (oflag & O_TRUNC) disposition = TRUNCATE_EXISTING;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_last_error();
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_BINARY | _O_NOINHERIT | ((oflag & (O_WRONLY | O_RDWR)) ? _O_RDWR : _O_RDONLY));
    if (fd < 0) {
        CloseHandle(h);
        errno = EMFILE;
    }
    return fd;
}

// Remove the name immediately even if the file is still open elsewhere
int
kitty_win32_unlink_posix(const char *path) {
    HANDLE h = CreateFileA(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
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

int
shm_unlink(const char *name) {
    char path[MAX_PATH];
    if (!shm_path(name, path, sizeof(path))) return -1;
    return kitty_win32_unlink_posix(path);
}
// }}}

// dlfcn {{{
static char dlerror_buf[512];
static bool dlerror_set = false;

static void
set_dlerror(const char *what) {
    DWORD code = GetLastError();
    char msg[400] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, msg, sizeof(msg) - 1, NULL);
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
        int wlen = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
        wchar_t *wpath = wlen > 0 ? malloc(sizeof(wchar_t) * wlen) : NULL;
        if (!wpath) {
            snprintf(dlerror_buf, sizeof(dlerror_buf), "Invalid module name: %s", filename);
            dlerror_set = true;
            return NULL;
        }
        MultiByteToWideChar(CP_UTF8, 0, filename, -1, wpath, wlen);
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
    // Windows local sockets have no peer credentials; report AF_UNIX as
    // verifiable so that the caller denies the peer rather than trusting it
    return addr.ss_family == AF_UNIX;
}

int
kitty_win32_socketpair(uintptr_t out[2]) {
    SOCKET listener = INVALID_SOCKET, a = INVALID_SOCKET, b = INVALID_SOCKET;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
    int addrlen = sizeof(addr);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) goto fail;
    if (bind(listener, (struct sockaddr *)&addr, addrlen) == SOCKET_ERROR) goto fail;
    if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) == SOCKET_ERROR) goto fail;
    if (listen(listener, 1) == SOCKET_ERROR) goto fail;
    a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (a == INVALID_SOCKET) goto fail;
    if (connect(a, (struct sockaddr *)&addr, addrlen) == SOCKET_ERROR) goto fail;
    b = accept(listener, NULL, NULL);
    if (b == INVALID_SOCKET) goto fail;
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
    // pipes are unidirectional
    shutdown(s[0], SD_SEND);
    shutdown(s[1], SD_RECEIVE);
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

// Resolves path relative to the directory open at dirfd into buf (UTF-8).
// Returns the path to use, which is path itself when no resolution is needed.
static const char *
resolve_at(int dirfd, const char *path, char *buf, size_t bufsz) {
    if (dirfd == AT_FDCWD || is_absolute_path(path)) return path;
    HANDLE h = (HANDLE)_get_osfhandle(dirfd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return NULL;
    }
    wchar_t wdir[32768];
    DWORD n = GetFinalPathNameByHandleW(h, wdir, (DWORD)(sizeof(wdir) / sizeof(wdir[0])), FILE_NAME_NORMALIZED);
    if (!n || n >= sizeof(wdir) / sizeof(wdir[0])) {
        set_errno_from_last_error();
        return NULL;
    }
    // strip the \\?\ prefix added by GetFinalPathNameByHandleW
    const wchar_t *dir = wdir;
    if (wcsncmp(dir, L"\\\\?\\UNC\\", 8) == 0) {
        dir += 6;
        wdir[6] = L'\\';
    } else if (wcsncmp(dir, L"\\\\?\\", 4) == 0) dir += 4;
    int len = WideCharToMultiByte(CP_UTF8, 0, dir, -1, buf, (int)bufsz, NULL, NULL);
    if (len <= 0) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    size_t used = strlen(buf);
    if (snprintf(buf + used, bufsz - used, "\\%s", path) >= (int)(bufsz - used)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return buf;
}

int
openat(int dirfd, const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    char buf[PATH_MAX * 4];
    const char *p = resolve_at(dirfd, path, buf, sizeof(buf));
    if (!p) return -1;
    const bool want_dir = flags & O_DIRECTORY, nofollow = flags & O_NOFOLLOW;
    const bool readonly = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) == 0;
    if (want_dir || (nofollow && readonly)) {
        // CRT open() can neither open directories nor refuse to follow symlinks
        wchar_t wpath[32768];
        if (!MultiByteToWideChar(CP_UTF8, 0, p, -1, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) {
            errno = ENAMETOOLONG;
            return -1;
        }
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
    return _open(p, (flags & ~KITTY_WIN32_NON_CRT_OPEN_FLAGS) | _O_BINARY, mode);
}

int
kitty_win32_open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    return openat(AT_FDCWD, path, flags, mode);
}

int
mkdirat(int dirfd, const char *path, mode_t mode UNUSED) {
    char buf[PATH_MAX * 4];
    const char *p = resolve_at(dirfd, path, buf, sizeof(buf));
    if (!p) return -1;
    return _mkdir(p);
}

int
symlinkat(const char *target, int dirfd, const char *linkpath) {
    char buf[PATH_MAX * 4];
    const char *p = resolve_at(dirfd, linkpath, buf, sizeof(buf));
    if (!p) return -1;
    wchar_t wtarget[32768], wlink[32768];
    if (!MultiByteToWideChar(CP_UTF8, 0, target, -1, wtarget, (int)(sizeof(wtarget) / sizeof(wtarget[0]))) ||
        !MultiByteToWideChar(CP_UTF8, 0, p, -1, wlink, (int)(sizeof(wlink) / sizeof(wlink[0])))) {
        errno = ENAMETOOLONG;
        return -1;
    }
    DWORD attrs = GetFileAttributesW(wtarget);
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    if (!CreateSymbolicLinkW(wlink, wtarget, flags)) {
        set_errno_from_last_error();
        return -1;
    }
    return 0;
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
        int fd = _open(template, flags | _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
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

// Reads the target of a symlink, returns the number of wchars or -1
static ssize_t
read_symlink_target(const char *path, wchar_t *target, size_t target_sz) {
    wchar_t wpath[32768];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) {
        errno = ENAMETOOLONG;
        return -1;
    }
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
        return -1;
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
    union {
        struct symlink_reparse_buffer rdb;
        char raw[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    } u;
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, &u, sizeof(u), &ret, NULL);
    DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok) {
        errno = err == ERROR_NOT_A_REPARSE_POINT ? EINVAL : EIO;
        return -1;
    }
    if (u.rdb.ReparseTag != IO_REPARSE_TAG_SYMLINK) {
        errno = EINVAL;
        return -1;
    }
    const wchar_t *pb = u.rdb.PathBuffer;
    const wchar_t *name = pb + u.rdb.PrintNameOffset / sizeof(wchar_t);
    size_t len = u.rdb.PrintNameLength / sizeof(wchar_t);
    if (!len) {
        name = pb + u.rdb.SubstituteNameOffset / sizeof(wchar_t);
        len = u.rdb.SubstituteNameLength / sizeof(wchar_t);
        if (len >= 4 && wcsncmp(name, L"\\??\\", 4) == 0) {
            name += 4;
            len -= 4;
        }
    }
    if (len >= target_sz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(target, name, len * sizeof(wchar_t));
    target[len] = 0;
    return (ssize_t)len;
}

ssize_t
readlink(const char *path, char *buf, size_t bufsiz) {
    wchar_t target[32768];
    ssize_t len = read_symlink_target(path, target, sizeof(target) / sizeof(target[0]));
    if (len < 0) return -1;
    char utf8[32768 * 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, target, (int)len, utf8, (int)sizeof(utf8), NULL, NULL);
    if (n <= 0) {
        errno = EIO;
        return -1;
    }
    if ((size_t)n > bufsiz) n = (int)bufsiz;
    memcpy(buf, utf8, (size_t)n);
    return n;
}

int
kitty_win32_lstat(const char *path, struct stat *st) {
    wchar_t wpath[32768];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) {
        errno = ENAMETOOLONG;
        return -1;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad) && (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        wchar_t target[32768];
        ssize_t len = read_symlink_target(path, target, sizeof(target) / sizeof(target[0]));
        if (len >= 0) {
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFLNK | S_IRUSR | S_IWUSR;
            st->st_nlink = 1;
            st->st_size = WideCharToMultiByte(CP_UTF8, 0, target, (int)len, NULL, 0, NULL, NULL);
            return 0;
        }
    }
    return stat(path, st);
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
    wchar_t wpath[32768];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) {
        errno = ENAMETOOLONG;
        return -1;
    }
    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_last_error();
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) {
        CloseHandle(h);
        errno = EMFILE;
    }
    return fd;
}

// Creates a uniquely named file in dir (which must end with a path separator)
// that the OS deletes when its last handle is closed, which also happens if
// the process crashes. It is opened without sharing, so no other process can
// open or delete it while it is in use.
static int
open_delete_on_close_file(const wchar_t *dir, const wchar_t *prefix, DWORD attributes) {
    wchar_t path[4096];
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
        int n = _snwprintf(path, sizeof(path) / sizeof(path[0]), L"%ls%ls%ls", dir, prefix, suffix);
        if (n < 0 || (size_t)n >= sizeof(path) / sizeof(path[0])) {
            errno = ENAMETOOLONG;
            return -1;
        }
        HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, attributes | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
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
    wchar_t dir[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, dir);
    if (!n || n > MAX_PATH) {
        errno = ENOENT;
        return -1;
    }
    return open_delete_on_close_file(dir, L"kitty-tmp-", FILE_ATTRIBUTE_TEMPORARY);
}

// Converts UTF-8 dir to a wide path ending with a path separator, returns its length or -1
static int
wide_dir_path(const char *dir, wchar_t *buf, size_t bufsz) {
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, buf, (int)bufsz - 1);
    if (!n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    n--; // the terminating NUL
    if (n && buf[n - 1] != L'\\' && buf[n - 1] != L'/') {
        buf[n++] = L'\\';
        buf[n] = 0;
    }
    return n;
}

int
kitty_win32_open_delete_on_close_file(const char *dir, const char *prefix) {
    wchar_t wdir[4096], wprefix[256];
    if (wide_dir_path(dir, wdir, sizeof(wdir) / sizeof(wdir[0])) < 0) return -1;
    if (!MultiByteToWideChar(CP_UTF8, 0, prefix, -1, wprefix, (int)(sizeof(wprefix) / sizeof(wprefix[0])))) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return open_delete_on_close_file(wdir, wprefix, FILE_ATTRIBUTE_NORMAL);
}

int
kitty_win32_delete_unused_files(const char *dir, const char *prefix) {
    wchar_t path[4096], wprefix[256];
    int dirlen = wide_dir_path(dir, path, sizeof(path) / sizeof(path[0]));
    if (dirlen < 0) return -1;
    int prefixlen = MultiByteToWideChar(CP_UTF8, 0, prefix, -1, wprefix, (int)(sizeof(wprefix) / sizeof(wprefix[0])));
    if (!prefixlen--) {
        errno = ENAMETOOLONG;
        return -1;
    }
    const size_t pathsz = sizeof(path) / sizeof(path[0]);
    int n = _snwprintf(path + dirlen, pathsz - dirlen, L"%ls*", wprefix);
    if (n < 0 || (size_t)n >= pathsz - dirlen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(path, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return 0;
        set_errno_from_last_error();
        return -1;
    }
    int count = 0;
    do {
        // the wildcard can also match 8.3 short names, so check the real name
        if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || wcsncmp(fd.cFileName, wprefix, prefixlen) != 0) continue;
        n = _snwprintf(path + dirlen, pathsz - dirlen, L"%ls", fd.cFileName);
        if (n < 0 || (size_t)n >= pathsz - dirlen) continue;
        // A file that is open in some process fails to delete, as it was not
        // opened with FILE_SHARE_DELETE
        if (DeleteFileW(path)) count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

int
kitty_win32_path_from_fd(int fd, char *buf, size_t bufsz) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    wchar_t wpath[32768];
    DWORD n = GetFinalPathNameByHandleW(h, wpath, (DWORD)(sizeof(wpath) / sizeof(wpath[0])), FILE_NAME_NORMALIZED);
    if (!n || n >= sizeof(wpath) / sizeof(wpath[0])) {
        if (!n) set_errno_from_last_error();
        else errno = ENAMETOOLONG;
        return -1;
    }
    const wchar_t *p = wpath;
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) {
        p += 6; // \\?\UNC\server\share -> \\server\share
        wpath[6] = L'\\';
    } else if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
    if (!WideCharToMultiByte(CP_UTF8, 0, p, -1, buf, (int)bufsz, NULL, NULL)) {
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

int
setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

int
unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    return _putenv_s(name, "") == 0 ? 0 : -1;
}

char *
realpath(const char *path, char *resolved_path) {
    char *ans = _fullpath(resolved_path, path, resolved_path ? PATH_MAX : 0);
    if (!ans) {
        errno = ENOENT;
        return NULL;
    }
    for (char *p = ans; *p; p++)
        if (*p == '\\') *p = '/';
    return ans;
}

bool
win32_exe_path(char *buf, size_t buf_sz) {
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)buf_sz);
    if (n == 0 || n >= buf_sz) return false;
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

bool
win32_add_dll_dir_of_module(const wchar_t *module_name) {
    // Extension modules are loaded with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS which
    // ignores PATH, so DLLs next to libpython (zlib1.dll, etc.) are only found
    // if that directory is explicitly registered.
    HMODULE mod = GetModuleHandleW(module_name);
    if (!mod) return false;
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(mod, path, sizeof(path) / sizeof(path[0]));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return false;
    wchar_t *sep = wcsrchr(path, L'\\');
    if (!sep) return false;
    *sep = 0;
    return AddDllDirectory(path) != NULL;
}

bool
win32_spawn_detached(char *const argv[]) {
    char exe[PATH_MAX + 1] = {0};
    if (!win32_exe_path(exe, sizeof(exe))) return false;
    char cmdline[32768];
    size_t pos = 0;
    if (!kitty_win32_append_quoted_arg(cmdline, sizeof(cmdline), &pos, exe)) return false;
    for (int i = 1; argv[i]; i++)
        if (!kitty_win32_append_quoted_arg(cmdline, sizeof(cmdline), &pos, argv[i])) return false;
    STARTUPINFOA si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(exe, cmdline, NULL, NULL, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)) {
        set_errno_from_last_error();
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static BOOL WINAPI
ignore_console_ctrl_event(DWORD type UNUSED) {
    return TRUE;
}

int
win32_spawn_and_wait(const char *exe, char *const argv[]) {
    char cmdline[32768];
    size_t pos = 0;
    if (!kitty_win32_append_quoted_arg(cmdline, sizeof(cmdline), &pos, exe)) {
        errno = E2BIG;
        return -1;
    }
    for (int i = 1; argv[i]; i++) {
        if (!kitty_win32_append_quoted_arg(cmdline, sizeof(cmdline), &pos, argv[i])) {
            errno = E2BIG;
            return -1;
        }
    }
    STARTUPINFOA si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    // Ctrl+C is delivered to every process attached to the console, let the
    // child deal with it and keep waiting for its exit code.
    SetConsoleCtrlHandler(ignore_console_ctrl_event, TRUE);
    if (!CreateProcessA(exe, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        set_errno_from_last_error();
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
