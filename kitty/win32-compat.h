/*
 * win32-compat.h
 * Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 *
 * Minimal POSIX compatibility layer for building the kitty C core with
 * MinGW-w64 on Windows. Provides the subset of <poll.h>, <sys/mman.h>,
 * <dlfcn.h>, <fcntl.h>, <signal.h> and locale extensions that the core
 * uses. Deliberately does not include <windows.h> as its macros and
 * typedefs (POINT, mouse_event, ...) collide with kitty's own names.
 */

#pragma once
#ifdef _WIN32

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <locale.h>
#include <unistd.h>
#include <signal.h>
#include <io.h>
#include <direct.h>
#include <malloc.h>

#ifndef PATH_MAX
#define PATH_MAX 260
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x40000000
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif
// Bits unused by the CRT _O_* flags, handled by openat() and masked before _open()
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x20000000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x10000000
#endif
#define KITTY_WIN32_NON_CRT_OPEN_FLAGS (O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW)
#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#define S_IWUSR _S_IWRITE
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) 0
#endif
#ifndef S_IFLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifndef ENOTSOCK
#define ENOTSOCK 128
#endif

// <fcntl.h>
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
int fcntl(int fd, int cmd, ...);
// Creates a connected loopback socket pair rather than an anonymous pipe, so
// that the result can be used with poll()
int pipe2(int fds[2], int flags);
// The *at() functions resolve dirfd to a path via GetFinalPathNameByHandle()
int openat(int dirfd, const char *path, int flags, ...);
int mkdirat(int dirfd, const char *path, mode_t mode);
int symlinkat(const char *target, int dirfd, const char *linkpath);
#define AT_FDCWD (-100)
int lockf(int fd, int cmd, off_t len);
#define F_ULOCK 0
#define F_LOCK 1
#define F_TLOCK 2
#define F_TEST 3
int mkostemp(char *template, int flags);
int mkstemp(char *template);
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
int fsync(int fd);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int ttyname_r(int fd, char *buf, size_t buflen);
void explicit_bzero(void *s, size_t n);

// <poll.h>: only sockets are pollable on Windows, pipes are handled by the
// ConPTY based child monitor.
struct pollfd {
    int fd;
    short events;
    short revents;
};
typedef unsigned long nfds_t;
#define POLLIN 0x0300
#define POLLPRI 0x0400
#define POLLOUT 0x0010
#define POLLERR 0x0001
#define POLLHUP 0x0002
#define POLLNVAL 0x0004
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

// <sys/mman.h>
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int mlock(const void *addr, size_t len);
int munlock(const void *addr, size_t len);
int shm_open(const char *name, int oflag, mode_t mode);
int shm_unlink(const char *name);
// unlink() that removes the name even while the file is open elsewhere
int kitty_win32_unlink_posix(const char *path);

// <dlfcn.h>
#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_LOCAL 0x0
#define RTLD_GLOBAL 0x100
void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
char *dlerror(void);

// <locale.h> GNU/BSD extensions
typedef _locale_t locale_t;
#define LC_ALL_MASK LC_ALL
#define LC_NUMERIC_MASK LC_NUMERIC
#define LC_CTYPE_MASK LC_CTYPE
locale_t newlocale(int category_mask, const char *locale, locale_t base);
void freelocale(locale_t locobj);
#define strtod_l _strtod_l

// <stdlib.h> extensions
int posix_memalign(void **memptr, size_t alignment, size_t size);
#define aligned_free _aligned_free
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
// resolved_path must be NULL or at least PATH_MAX bytes
char *realpath(const char *path, char *resolved_path);
// GetModuleFileName() with backslashes converted to forward slashes
bool win32_exe_path(char *buf, size_t buf_sz);
// Re-runs the current executable with the specified argv as a detached process
// (no console, own process group), returns false on failure.
bool win32_spawn_detached(char *const argv[]);
// Runs exe with argv[1:] as a child sharing this process' console and std
// handles, waits for it and returns its exit code, or -1 with errno set.
int win32_spawn_and_wait(const char *exe, char *const argv[]);
// Attaches to the console of the parent process, if it has one, and binds any
// of stdin/stdout/stderr that the parent did not redirect to it. Needed
// because GUI subsystem executables do not inherit the parent's console.
// Returns false if there is no console to attach to.
bool win32_attach_parent_console(void);
bool win32_add_dll_dir_of_module(const wchar_t *module_name);
// Appends arg to buf at *pos quoted as per CommandLineToArgvW() rules,
// returns false if buf is too small
bool kitty_win32_append_quoted_arg(char *buf, size_t buf_sz, size_t *pos, const char *arg);
void set_errno_from_last_error(void);
// Creates a connected pair of loopback TCP sockets (SOCKET handles)
int kitty_win32_socketpair(uintptr_t out[2]);
// Wraps a SOCKET handle (as returned by Python's socket.fileno()) in a CRT fd
int kitty_win32_fd_from_socket_handle(intptr_t sock);
bool kitty_win32_set_current_thread_name(const char *name);
// Read-only open that permits the file to be concurrently deleted/renamed by
// its creator, which CRT open() disallows
int kitty_win32_open_readonly_shared(const char *path);
// Read/write temp file that is deleted when its last handle is closed
int kitty_win32_open_anonymous_tmpfile(void);
// Read/write file named prefix + random suffix in dir, that is deleted when its
// last handle is closed (or the process dies) and cannot be opened by others
int kitty_win32_open_delete_on_close_file(const char *dir, const char *prefix);
// Deletes the files in dir whose names start with prefix, except those open in
// some process without FILE_SHARE_DELETE, returns the number deleted or -1
int kitty_win32_delete_unused_files(const char *dir, const char *prefix);
// UTF-8 path of an open file or directory fd
int kitty_win32_path_from_fd(int fd, char *buf, size_t bufsz);
// Whether the file's owner SID is the current process token's user or default owner
int kitty_win32_fd_owned_by_current_user(int fd, bool *owned);
// Number of active Terminal Services sessions with a logged in user
size_t kitty_win32_num_logged_in_users(void);
// Called before a socket based fd is closed by close()
extern void (*kitty_win32_on_fd_close)(int fd);

// <sys/stat.h> extensions
#define mkdir(path, mode) _mkdir(path)
int kitty_win32_lstat(const char *path, struct stat *st);
#define lstat kitty_win32_lstat

// <dirent.h>: MinGW dirent has no d_type
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

// <sys/socket.h>: sockets are wrapped in CRT file descriptors so they can be
// used interchangeably with the fds returned by pipe2() and open().
typedef int socklen_t;
// pyconfig.h defines uid_t/gid_t as macros expanding to int, match that so
// translation units with and without Python.h agree on the type
#ifndef uid_t
#define uid_t int
#endif
#ifndef gid_t
#define gid_t int
#endif
static inline uid_t
geteuid(void) {
    return 0;
}
static inline gid_t
getegid(void) {
    return 0;
}
struct sockaddr;
#ifndef SHUT_RD
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
ssize_t kitty_win32_recv(int fd, void *buf, size_t len, int flags);
ssize_t kitty_win32_send(int fd, const void *buf, size_t len, int flags);
int kitty_win32_accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int kitty_win32_connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int kitty_win32_bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int kitty_win32_shutdown(int fd, int how);
// read/write/close on a socket fd must go through Winsock
ssize_t kitty_win32_read(int fd, void *buf, size_t count);
ssize_t kitty_win32_write(int fd, const void *buf, size_t count);
int kitty_win32_close(int fd);
// Whether connections to this listening socket should be peer-verified.
// Windows local sockets have no peer credentials, but AF_UNIX sockets are
// reported as verifiable so that callers fail closed (deny the peer)
// rather than silently trusting it
bool kitty_win32_peer_credentials_are_available(int fd);
// open() honoring O_DIRECTORY and (for read-only opens) O_NOFOLLOW
int kitty_win32_open(const char *path, int flags, ...);
#ifndef KITTY_WIN32_COMPAT_IMPL
#define read(fd, buf, count) kitty_win32_read(fd, buf, count)
#define write(fd, buf, count) kitty_win32_write(fd, buf, count)
#define close(fd) kitty_win32_close(fd)
#define open(...) kitty_win32_open(__VA_ARGS__)
#define recv(fd, buf, len, flags) kitty_win32_recv(fd, buf, len, flags)
#define send(fd, buf, len, flags) kitty_win32_send(fd, buf, len, flags)
#define accept(fd, addr, addrlen) kitty_win32_accept(fd, addr, addrlen)
#define connect(fd, addr, addrlen) kitty_win32_connect(fd, addr, addrlen)
#define bind(fd, addr, addrlen) kitty_win32_bind(fd, addr, addrlen)
#define shutdown(fd, how) kitty_win32_shutdown(fd, how)
#endif

// <sys/ioctl.h>
struct winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};

// <termios.h>: the controlling terminal is the Windows console. The saved
// state is the pair of console input/output modes.
struct termios {
    unsigned long input_mode, output_mode;
    bool read_with_timeout;
};
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
void kitty_win32_beep(void);
int kitty_win32_open_console(int flags);
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int optional_actions, const struct termios *t);
int kitty_win32_console_raw_mode(int fd, const struct termios *saved, bool read_with_timeout);

// <sys/wait.h> / <signal.h>: signals do not exist on Windows in any
// meaningful way, these are here only so that the signal handling plumbing
// compiles. The Windows child monitor never reports any of them and kill()
// only knows how to terminate processes.
union sigval {
    int sival_int;
    void *sival_ptr;
};
typedef struct {
    int si_signo;
    int si_code;
    pid_t si_pid;
    uid_t si_uid;
    int si_status;
    void *si_addr;
    union sigval si_value;
} siginfo_t;
typedef unsigned int sigset_t;
#define SA_SIGINFO 4
#define SA_RESTART 0x10000000
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
};
int sigemptyset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
// Only SIGINT, SIGTERM and SIGHUP are ever delivered, via the console control
// handler (Ctrl+C, Ctrl+Break, console close/logoff/shutdown).
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
// Invokes the handler installed via sigaction() for sig, used to synthesize
// SIGCHLD from process wait callbacks
void kitty_win32_deliver_signal(int sig, pid_t pid, int status);
int kill(pid_t pid, int sig);
pid_t waitpid(pid_t pid, int *status, int options);
#define WNOHANG 1
#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WIFSIGNALED(status) (((status) & 0x7f) != 0 && ((status) & 0x7f) != 0x7f)
#define WTERMSIG(status) ((status) & 0x7f)
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef CLD_EXITED
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_STOPPED 5
#define CLD_CONTINUED 6
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGUSR2
#define SIGUSR2 12
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGWINCH
#define SIGWINCH 28
#endif
#ifndef SIGTSTP
#define SIGTSTP 20
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif
#ifndef SIGCONT
#define SIGCONT 18
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGTTIN
#define SIGTTIN 21
#endif
#ifndef SIGTTOU
#define SIGTTOU 22
#endif

bool secure_random_bytes_win32(void *buf, size_t nbytes);
void win32_compat_init(void);

#endif
