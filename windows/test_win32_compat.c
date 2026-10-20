/*
 * Unicode and long path tests for the POSIX shims in kitty/win32-compat.c,
 * built on its own:
 *
 *   gcc -std=gnu11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 -Ikitty windows/test_win32_compat.c kitty/win32-compat.c \
 *       -o build/test-win32-compat.exe -lws2_32 -lbcrypt -ladvapi32 -lwtsapi32 -lshell32
 *
 * The executable has no application manifest, so the ANSI code page is that
 * of the system and anything still going through the narrow APIs fails for
 * names outside of it. Paths are made longer than MAX_PATH to test the \\?\
 * handling, as long paths are not enabled system wide by default.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "win32-compat.h"
#include <string.h>

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                                                       \
    do {                                                                                                                  \
        checks++;                                                                                                         \
        if (!(cond)) {                                                                                                    \
            int e_ = errno;                                                                                               \
            failures++;                                                                                                   \
            fprintf(stderr, "%s:%d: %s: check failed: %s (errno: %d %s)\n", __FILE__, __LINE__, __func__, #cond, e_, strerror(e_)); \
        }                                                                                                                 \
    } while (0)

// Names outside of any single ANSI code page: CJK, Latin-1, Cyrillic and an
// emoji (a UTF-16 surrogate pair, four bytes of UTF-8)
#define UNICODE_NAME "日本語 é Ж \xf0\x9f\x98\x80"

static char *
join(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *ans = malloc(la + lb + 2);
    memcpy(ans, a, la);
    ans[la] = '/';
    memcpy(ans + la + 1, b, lb + 1);
    return ans;
}

static bool
write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, strlen(data), f) == strlen(data);
    return fclose(f) == 0 && ok;
}

static bool
file_has_contents(const char *path, const char *expected) {
    char buf[256] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return n == strlen(expected) && memcmp(buf, expected, n) == 0;
}

static bool
same_path(const char *a, const char *b) {
    // compares ignoring the kind of path separator and ASCII case
    for (; *a && *b; a++, b++) {
        char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return *a == *b;
}

static void
test_dir_and_file(const char *dir) {
    struct stat st;
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(mkdir(dir, 0700) == -1 && errno == EEXIST);
    CHECK(stat(dir, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(lstat(dir, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(access(dir, F_OK) == 0);
    CHECK(access(dir, R_OK | W_OK | X_OK) == 0);

    char *file = join(dir, "ファイル " UNICODE_NAME ".txt");
    CHECK(write_file(file, "contents"));
    CHECK(file_has_contents(file, "contents"));
    CHECK(stat(file, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 8);
    CHECK(lstat(file, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 8);
    CHECK(access(file, R_OK) == 0);
    int fd = open(file, O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        char buf[16] = {0};
        CHECK(read(fd, buf, sizeof(buf)) == 8 && memcmp(buf, "contents", 8) == 0);
        close(fd);
    }
    fd = kitty_win32_open_readonly_shared(file);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);

    size_t sz = 0;
    void *addr = kitty_win32_map_file(file, &sz);
    CHECK(addr && sz == 8 && memcmp(addr, "contents", 8) == 0);
    kitty_win32_unmap_file(addr);

    char *real = realpath(file, NULL), *real_dir = realpath(dir, NULL);
    CHECK(real && strstr(real, UNICODE_NAME ".txt"));
    CHECK(real_dir && real && strncmp(real, real_dir, strlen(real_dir)) == 0);
    char buf[PATH_MAX];
    CHECK(realpath(file, buf) == buf && real && strcmp(buf, real) == 0);
    free(real);
    free(real_dir);

    CHECK(unlink(file) == 0);
    CHECK(stat(file, &st) == -1 && errno == ENOENT);
    CHECK(access(file, F_OK) == -1);
    CHECK(realpath(file, NULL) == NULL && errno == ENOENT);
    CHECK(fopen(file, "rb") == NULL && errno == ENOENT);
    free(file);
}

static void
test_at_functions(const char *dir) {
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY);
    CHECK(dirfd >= 0);
    if (dirfd < 0) return;
    int fd = openat(dirfd, "子 " UNICODE_NAME, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, "at", 2) == 2);
        close(fd);
    }
    fd = openat(dirfd, "子 " UNICODE_NAME, O_RDONLY | O_NOFOLLOW);
    CHECK(fd >= 0);
    if (fd >= 0) {
        char buf[4] = {0};
        CHECK(read(fd, buf, sizeof(buf)) == 2 && memcmp(buf, "at", 2) == 0);
        close(fd);
    }
    CHECK(openat(dirfd, "子 " UNICODE_NAME, O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR);
    CHECK(mkdirat(dirfd, "サブ " UNICODE_NAME, 0700) == 0);
    struct stat st;
    char *sub = join(dir, "サブ " UNICODE_NAME);
    CHECK(stat(sub, &st) == 0 && S_ISDIR(st.st_mode));
    int subfd = openat(dirfd, "サブ " UNICODE_NAME, O_RDONLY | O_DIRECTORY);
    CHECK(subfd >= 0);
    if (subfd >= 0) close(subfd);

    char from_fd[PATH_MAX];
    char *real = realpath(dir, NULL);
    CHECK(kitty_win32_path_from_fd(dirfd, from_fd, sizeof(from_fd)) == 0 && real && same_path(from_fd, real));
    free(real);
    close(dirfd);
    free(sub);
}

static void
test_readdir(const char *parent) {
    static const char *names[] = {"α", "日本", UNICODE_NAME};
    bool found[3] = {0};
    char *dir = join(parent, "readdir");
    CHECK(mkdir(dir, 0700) == 0);
    for (int i = 0; i < 3; i++) {
        char *p = join(dir, names[i]);
        CHECK(write_file(p, names[i]));
        free(p);
    }
    DIR *d = opendir(dir);
    CHECK(d != NULL);
    if (!d) {
        free(dir);
        return;
    }
    struct dirent *de;
    int count = 0;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        count++;
        for (int i = 0; i < 3; i++)
            if (strcmp(de->d_name, names[i]) == 0) found[i] = true;
    }
    closedir(d);
    CHECK(count == 3 && found[0] && found[1] && found[2]);
    char *missing = join(dir, "missing");
    CHECK(opendir(missing) == NULL && errno == ENOENT);
    free(missing);
    for (int i = 0; i < 3; i++) {
        char *p = join(dir, names[i]);
        CHECK(unlink(p) == 0);
        free(p);
    }
    free(dir);
}

static void
test_temp_files(const char *dir) {
    char *template = join(dir, "tmp-" UNICODE_NAME "-XXXXXX");
    int fd = mkstemp(template);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
    struct stat st;
    CHECK(stat(template, &st) == 0 && S_ISREG(st.st_mode));
    CHECK(unlink(template) == 0);
    free(template);

    fd = kitty_win32_open_delete_on_close_file(dir, "doc-" UNICODE_NAME "-");
    CHECK(fd >= 0);
    // in use, so not deleted
    CHECK(kitty_win32_delete_unused_files(dir, "doc-" UNICODE_NAME "-") == 0);
    if (fd >= 0) close(fd);
    CHECK(kitty_win32_delete_unused_files(dir, "doc-" UNICODE_NAME "-") == 0);
}

static void
test_symlinks(const char *dir) {
    char *target = join(dir, "target " UNICODE_NAME), *link = join(dir, "link " UNICODE_NAME);
    CHECK(write_file(target, "target"));
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY);
    CHECK(dirfd >= 0);
    // needs Developer Mode or the privilege to create symlinks
    if (symlinkat(target, dirfd, "link " UNICODE_NAME) == 0) {
        struct stat st;
        char buf[PATH_MAX] = {0};
        CHECK(lstat(link, &st) == 0 && S_ISLNK(st.st_mode) && (size_t)st.st_size == strlen(target));
        CHECK(stat(link, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 6);
        CHECK(readlink(link, buf, sizeof(buf)) == (ssize_t)strlen(target) && same_path(buf, target));
        int fd = openat(dirfd, "link " UNICODE_NAME, O_RDONLY | O_NOFOLLOW);
        CHECK(fd == -1 && errno == ELOOP);
        char *real_link = realpath(link, NULL), *real_target = realpath(target, NULL);
        CHECK(real_link && real_target && strcmp(real_link, real_target) == 0);
        free(real_link);
        free(real_target);
        CHECK(unlink(link) == 0);
    } else {
        CHECK(errno == EPERM || errno == EACCES);
        printf("symlinks cannot be created here, skipped testing them in: %s\n", dir);
    }
    if (dirfd >= 0) close(dirfd);
    CHECK(unlink(target) == 0);
    free(target);
    free(link);
}

static void
test_path_functions(const char *dir) {
    test_dir_and_file(dir);
    test_at_functions(dir);
    test_readdir(dir);
    test_temp_files(dir);
    test_symlinks(dir);
}

static void
test_environment(void) {
    const char *value = "値 " UNICODE_NAME;
    CHECK(setenv("KITTY_WIN32_TEST", value, 1) == 0);
    const char *v = getenv("KITTY_WIN32_TEST");
    CHECK(v && strcmp(v, value) == 0);
    const wchar_t *wv = _wgetenv(L"KITTY_WIN32_TEST");
    CHECK(wv && wcscmp(wv, L"値 日本語 é Ж \U0001F600") == 0);
    CHECK(setenv("KITTY_WIN32_TEST", "other", 0) == 0);
    CHECK(strcmp(getenv("KITTY_WIN32_TEST"), value) == 0);
    CHECK(setenv("KITTY_WIN32_TEST", "other", 1) == 0);
    CHECK(strcmp(getenv("KITTY_WIN32_TEST"), "other") == 0);
    // earlier results stay valid
    CHECK(strcmp(v, value) == 0);
    CHECK(SetEnvironmentVariableW(L"KITTY_WIN32_TEST", L"wide \u00e9"));
    CHECK(strcmp(getenv("KITTY_WIN32_TEST"), "wide é") == 0);
    CHECK(unsetenv("KITTY_WIN32_TEST") == 0);
    CHECK(getenv("KITTY_WIN32_TEST") == NULL);
}

static void
test_cwd(const char *dir) {
    wchar_t *saved = _wgetcwd(NULL, 0), *wdir = kitty_win32_utf8_to_wide(dir);
    CHECK(saved && wdir && _wchdir(wdir) == 0);
    char buf[PATH_MAX], *cwd = getcwd(NULL, 0);
    CHECK(cwd && same_path(cwd, dir));
    CHECK(getcwd(buf, sizeof(buf)) == buf && same_path(buf, dir));
    CHECK(getcwd(buf, 4) == NULL && errno == ERANGE);
    // relative paths resolve against the cwd
    CHECK(write_file("relative " UNICODE_NAME, "relative"));
    char *real = realpath("relative " UNICODE_NAME, NULL), *real_dir = realpath(dir, NULL);
    char *expected = real_dir ? join(real_dir, "relative " UNICODE_NAME) : NULL;
    CHECK(real && expected && same_path(real, expected));
    CHECK(unlink("relative " UNICODE_NAME) == 0);
    free(real);
    free(real_dir);
    free(expected);
    free(cwd);
    if (saved) _wchdir(saved);
    free(saved);
    free(wdir);
}

static void
test_shm(const char *dir) {
    // shm objects live in the temp dir, which can be anywhere
    wchar_t *saved = _wgetenv(L"TMP") ? _wcsdup(_wgetenv(L"TMP")) : NULL;
    CHECK(setenv("TMP", dir, 1) == 0);
    int fd = shm_open("/kitty-win32-test-" UNICODE_NAME, O_CREAT | O_RDWR, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, "shm", 3) == 3);
        close(fd);
    }
    char *path = join(dir, "kitty-shm/kitty-win32-test-" UNICODE_NAME);
    CHECK(file_has_contents(path, "shm"));
    CHECK(shm_unlink("/kitty-win32-test-" UNICODE_NAME) == 0);
    struct stat st;
    CHECK(stat(path, &st) == -1 && errno == ENOENT);
    fd = kitty_win32_open_anonymous_tmpfile();
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
    char *shm_dir = join(dir, "kitty-shm");
    CHECK(_wrmdir(kitty_win32_wide_path(shm_dir)) == 0);
    free(shm_dir);
    free(path);
    if (saved) _wputenv_s(L"TMP", saved);
    free(saved);
}

static void
test_devices(void) {
    struct stat st;
    CHECK(stat("NUL", &st) == 0 && S_ISCHR(st.st_mode));
    CHECK(lstat("NUL", &st) == 0 && S_ISCHR(st.st_mode));
    FILE *f = fopen("NUL", "wb");
    CHECK(f != NULL);
    if (f) fclose(f);
}

static void
test_invalid_utf8(void) {
    CHECK(kitty_win32_wide_path("invalid \xff utf-8") == NULL && errno == EILSEQ);
    CHECK(fopen("invalid \xff utf-8", "rb") == NULL && errno == EILSEQ);
    struct stat st;
    CHECK(stat("invalid \xff utf-8", &st) == -1 && errno == EILSEQ);
}

static const char *child_args[] = {"--child", "é", "日本 語", "\xf0\x9f\x98\x80", "a\"b", "back\\slash\\", "", "trailing\\", NULL};

static int
run_child(int argc, char **argv) {
    // argv is the C runtime's, in the ANSI code page, use the wide command line
    char **uargv = kitty_win32_utf8_argv(&argc);
    (void)argv;
    if (!uargv) return 2;
    int n = 0;
    while (child_args[n]) n++;
    if (argc != n + 1) return 3;
    for (int i = 0; i < n; i++)
        if (strcmp(uargv[i + 1], child_args[i]) != 0) {
            fprintf(stderr, "argument %d mismatch: %s != %s\n", i, uargv[i + 1], child_args[i]);
            return 4;
        }
    char exe[PATH_MAX];
    if (!win32_exe_path(exe, sizeof(exe))) return 5;
    const char *expected = getenv("KITTY_WIN32_TEST_EXE");
    if (!expected || !same_path(exe, expected)) {
        fprintf(stderr, "exe path mismatch: %s != %s\n", exe, expected ? expected : "(null)");
        return 6;
    }
    return 0;
}

static int
run_crt_rules_child(void) {
    // Launched with the raw arguments --crt-rules "a""b c" d: for the C runtime
    // "" inside double quotes is a literal " and the quotes continue, while
    // CommandLineToArgvW() would split this into a"b and c".
    int argc = 0;
    char **uargv = kitty_win32_utf8_argv(&argc);
    if (!uargv) return 2;
    if (argc != 4 || strcmp(uargv[2], "a\"b c") != 0 || strcmp(uargv[3], "d") != 0) {
        for (int i = 0; i < argc; i++) fprintf(stderr, "argv[%d] = %s\n", i, uargv[i]);
        return 3;
    }
    return 0;
}

static void
test_crt_argv_rules(void) {
    // Batch files receive arguments quoted by kitty_win32_append_batch_arg(),
    // which kitty.exe must split with the C runtime's rules when they are
    // passed on with %*
    const DWORD sz = 32768;
    wchar_t *self = malloc(sizeof(wchar_t) * sz), *cmdline = malloc(sizeof(wchar_t) * (sz + 64));
    CHECK(self && cmdline);
    if (!self || !cmdline) return;
    DWORD n = GetModuleFileNameW(NULL, self, sz);
    CHECK(n && n < sz);
    wcscpy(cmdline, L"\"");
    wcscat(cmdline, self);
    wcscat(cmdline, L"\" --crt-rules \"a\"\"b c\" d");
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    CHECK(CreateProcessW(self, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    if (pi.hProcess) {
        DWORD code = 1;
        WaitForSingleObject(pi.hProcess, INFINITE);
        CHECK(GetExitCodeProcess(pi.hProcess, &code) && code == 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    free(self);
    free(cmdline);
}

static void
test_spawn(const char *dir) {
    // Runs a copy of this executable from dir, which checks its arguments and exe path
    char self[PATH_MAX];
    CHECK(win32_exe_path(self, sizeof(self)));
    char *copy = join(dir, "テスト " UNICODE_NAME ".exe");
    wchar_t *wself = kitty_win32_wide_path(self), *wcopy = kitty_win32_wide_path(copy);
    CHECK(wself && wcopy && CopyFileW(wself, wcopy, FALSE));
    CHECK(setenv("KITTY_WIN32_TEST_EXE", copy, 1) == 0);
    char *argv[sizeof(child_args) / sizeof(child_args[0]) + 1] = {copy};
    for (size_t i = 0; child_args[i]; i++) argv[i + 1] = (char *)child_args[i];
    CHECK(win32_spawn_and_wait(copy, argv) == 0);
    CHECK(unlink(copy) == 0);
    unsetenv("KITTY_WIN32_TEST_EXE");
    free(copy);
    free(wself);
    free(wcopy);
}

static void
remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char *child = join(path, de->d_name);
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
            else unlink(child);
            free(child);
        }
        closedir(d);
    }
    wchar_t *w = kitty_win32_wide_path(path);
    if (w) RemoveDirectoryW(w);
    free(w);
}

int
main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--child") == 0) return run_child(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--crt-rules") == 0) return run_crt_rules_child();
    SetConsoleOutputCP(CP_UTF8);
    printf("ANSI code page: %u\n", GetACP());
    wchar_t wtmp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, wtmp);
    if (!n || n > MAX_PATH) return 1;
    wtmp[n - 1] = 0; // the trailing backslash
    char *tmp = kitty_win32_wide_to_utf8(wtmp);
    char name[128];
    snprintf(name, sizeof(name), "kitty-win32-test-%lu " UNICODE_NAME, GetCurrentProcessId());
    char *root = join(tmp, name);
    printf("Testing in: %s\n", root);
    CHECK(mkdir(root, 0700) == 0);

    char *dir = join(root, "dir " UNICODE_NAME);
    test_path_functions(dir);
    test_environment();
    test_cwd(dir);
    test_shm(dir);
    test_devices();
    test_invalid_utf8();
    test_crt_argv_rules();
    test_spawn(dir);

    // A directory whose path is longer than MAX_PATH, both in UTF-16 units and
    // bytes of UTF-8. Each component is within the 255 unit limit.
    char *deep = strdup(root);
    const char *component = "長いディレクトリ名 long directory name " UNICODE_NAME " 長いディレクトリ名 long directory name";
    for (int i = 0; i < 4; i++) {
        char *next = join(deep, component);
        free(deep);
        deep = next;
        // the last one is created by test_path_functions()
        if (i < 3) CHECK(mkdir(deep, 0700) == 0);
    }
    wchar_t *wdeep = kitty_win32_utf8_to_wide(deep);
    printf("Long path is %zu UTF-16 units\n", wdeep ? wcslen(wdeep) : 0);
    CHECK(wdeep && wcslen(wdeep) > MAX_PATH);
    free(wdeep);
    test_path_functions(deep);
    test_spawn(deep);

    remove_tree(root);
    struct stat st;
    CHECK(stat(root, &st) == -1 && errno == ENOENT);
    printf("%d checks, %d failures\n", checks, failures);
    free(deep);
    free(dir);
    free(root);
    free(tmp);
    return failures ? 1 : 0;
}
