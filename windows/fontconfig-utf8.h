/* Windows-only adaptations for the bundled fontconfig. */
#pragma once
#define pollfd wsa_pollfd
#include <winsock2.h>
#undef pollfd
#undef POLLIN
#undef POLLOUT
#define KITTY_WIN32_COMPAT_IMPL
#include "win32-compat.h"
#undef KITTY_WIN32_COMPAT_IMPL
#include <shlobj.h>
#include <mbstring.h>

#define open(...) kitty_win32_open(__VA_ARGS__)
#define stat(path, st) kitty_win32_stat(path, st)
#define mkdir(path, mode) kitty_win32_mkdir(path)
#define access(path, mode) kitty_win32_access(path, mode)
#define unlink(path) kitty_win32_unlink(path)
#define fopen(path, mode) kitty_win32_fopen(path, mode)
#define getenv(name) kitty_win32_getenv(name)
#define DIR kitty_win32_DIR
#define dirent kitty_win32_dirent
#define opendir(path) kitty_win32_opendir(path)
#define readdir(dir) kitty_win32_readdir(dir)
#define closedir(dir) kitty_win32_closedir(dir)

static inline DWORD
kitsu_fc_string(const wchar_t *wide, char *out, DWORD size) {
    char *s = kitty_win32_wide_to_utf8(wide);
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < size) memcpy(out, s, n + 1);
    free(s);
    return n < size ? (DWORD)n : (DWORD)n + 1;
}

static inline DWORD
kitsu_fc_module(HMODULE module, char *out, DWORD size) {
    wchar_t *wide = calloc(32768, sizeof(wchar_t));
    if (!wide) return 0;
    DWORD n = GetModuleFileNameW(module, wide, 32768);
    DWORD ans = n && n < 32768 ? kitsu_fc_string(wide, out, size) : 0;
    free(wide);
    return ans < size ? ans : 0;
}

static inline DWORD
kitsu_fc_full_path(const char *path, DWORD size, char *out, char **basename) {
    wchar_t *input = kitty_win32_utf8_to_wide(path);
    if (!input) return 0;
    DWORD n = GetFullPathNameW(input, 0, NULL, NULL), ans = 0;
    wchar_t *wide = n ? calloc(n, sizeof(wchar_t)) : NULL;
    if (wide && GetFullPathNameW(input, n, wide, NULL)) ans = kitsu_fc_string(wide, out, size);
    if (basename && ans && ans < size) {
        *basename = strrchr(out, '\\');
        *basename = *basename ? *basename + 1 : out;
    }
    free(input);
    free(wide);
    return ans < size ? ans : 0;
}

static inline UINT WINAPI
kitsu_fc_windows_dir(char *out, UINT size) {
    wchar_t wide[MAX_PATH + 1];
    return GetSystemWindowsDirectoryW(wide, MAX_PATH + 1) ? kitsu_fc_string(wide, out, size) : 0;
}

static inline HRESULT WINAPI
kitsu_fc_folder(HWND window, int folder, HANDLE token, DWORD flags, char *out) {
    wchar_t wide[MAX_PATH + 1];
    HRESULT hr = SHGetFolderPathW(window, folder, token, flags, wide);
    if (FAILED(hr)) return hr;
    DWORD n = kitsu_fc_string(wide, out, FC_MAX_FILE_LEN);
    return n && n < FC_MAX_FILE_LEN ? S_OK : E_FAIL;
}

static inline DWORD
kitsu_fc_temp(DWORD size, char *out) {
    wchar_t *wide = calloc(32768, sizeof(wchar_t));
    if (!wide) return 0;
    DWORD n = GetTempPathW(32768, wide);
    DWORD ans = n && n < 32768 ? kitsu_fc_string(wide, out, size) : 0;
    free(wide);
    return ans;
}

static inline int
kitsu_fc_chmod(const char *path, int mode) {
    wchar_t *wide = kitty_win32_wide_path(path);
    int ans = wide ? _wchmod(wide, mode) : -1;
    free(wide);
    return ans;
}

static inline int
kitsu_fc_rmdir(const char *path) {
    wchar_t *wide = kitty_win32_wide_path(path);
    int ans = wide ? _wrmdir(wide) : -1;
    free(wide);
    return ans;
}

static inline int
kitsu_fc_rename(const char *src, const char *dest) {
    wchar_t *a = kitty_win32_wide_path(src), *b = kitty_win32_wide_path(dest);
    int ans = a && b ? _wrename(a, b) : -1;
    free(a);
    free(b);
    return ans;
}

#ifdef KITSU_FC_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H

static void
kitsu_fc_close_stream(FT_Stream stream) {
    kitty_win32_unmap_file(stream->base);
    free(stream);
}

static inline FT_Error
kitsu_fc_new_face(FT_Library library, const char *path, FT_Long index, FT_Face *face) {
    size_t size = 0;
    void *addr = kitty_win32_map_file(path, &size);
    if (!addr) return FT_Err_Cannot_Open_Resource;
    FT_Stream stream = size <= ULONG_MAX ? calloc(1, sizeof(FT_StreamRec)) : NULL;
    if (!stream) {
        kitty_win32_unmap_file(addr);
        return FT_Err_Out_Of_Memory;
    }
    stream->base = addr;
    stream->size = (unsigned long)size;
    stream->close = kitsu_fc_close_stream;
    FT_Open_Args args = {.flags = FT_OPEN_STREAM, .stream = stream};
    return FT_Open_Face(library, &args, index, face);
}
#define FT_New_Face kitsu_fc_new_face
#endif

#undef GetModuleFileName
#define GetModuleFileName kitsu_fc_module
#define GetModuleFileNameA kitsu_fc_module
#undef GetFullPathName
#define GetFullPathName kitsu_fc_full_path
#define GetFullPathNameA kitsu_fc_full_path
#undef GetTempPath
#define GetTempPath kitsu_fc_temp
#define chmod kitsu_fc_chmod
#define rmdir kitsu_fc_rmdir
#define rename kitsu_fc_rename
#define _mbsrchr(s, c) ((unsigned char *)strrchr((const char *)(s), (c)))
