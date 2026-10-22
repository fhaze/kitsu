# AGENTS.md

## Building on Windows (local dev)

This fork builds natively on Windows via MSYS2 UCRT64. The canonical recipe is the
`Build Kitsu (MSYS2 UCRT64)` job in `.github/workflows/windows-build.yml`, which runs
`python setup.py windows-package`.

Use the helper script instead of invoking anything by hand:

```sh
./build-windows.sh                  # build → windows-package/kitsu-*-windows-x86_64.zip
./build-windows.sh --install-deps   # install toolchain + deps on a fresh machine
```

### Gotchas

- **MSYS2 UCRT64, not Git Bash or MSYS.** The build must run under the UCRT64 shell
  (`MSYSTEM=UCRT64`) with the mingw Python (`sys.platform == 'win32'`).
  `python setup.py windows-package` refuses to run otherwise.
- **MSYS2 login shells strip `LOCALAPPDATA` and `USERPROFILE`.** `setup.py` needs
  `LOCALAPPDATA` to find the Symbols Nerd Font; pass it explicitly or the build dies
  with "The font 'Symbols NERD Font Mono' was not found" even when the font is installed.
- **Go is Windows-native, not an MSYS2 package.** Install via winget (`GoLang.Go`);
  `go.mod` requires >= 1.26. Add `C:\Program Files\Go\bin` to `PATH` inside MSYS2 —
  Windows PATH changes don't reach already-running shells.
- **SIMDe is not packaged by MSYS2.** Download the v0.8.2 tarball and point
  `CPPFLAGS=-I<dir>` at it.
- **slangc is a separate download** (shader-slang v2026.14.1 windows zip); point the
  `SLANGC` env var at `bin/slangc.exe`.
- **Symbols Nerd Font Mono must be in `%LOCALAPPDATA%\Microsoft\Windows\Fonts`**
  (or `C:\Windows\Fonts`) at build time; it gets copied into the package.
- **`start /b /wait kitty.exe` can fail with "Access is denied"** outside CI. For
  smoke tests invoke the exes directly: `kitty.exe --version`, `kitten.exe --version`,
  `kitty.exe +runpy "import ssl, sqlite3, kitty.fast_data_types"`.
- Build deps are cached in `~/build-deps` by default (override with
  `KITTY_BUILD_DEPS`).

### Unicode and long paths

The narrow (ANSI) Windows APIs and the C runtime functions built on them use the
ANSI code page, so C code must not pass paths or strings to them directly. Use the
UTF-8 wrappers in `kitty/win32-compat.h` (`win32-compat.h` redirects `fopen`, `stat`,
`getenv`, `opendir`, ... to them), which call the W APIs and add `\\?\` to paths
longer than `MAX_PATH`. The executables embed `kitty/launcher/windows-app.manifest`
(UTF-8 active code page, long path aware), which third party libraries such as
fontconfig rely on. Tests, run against a built package:

```sh
gcc -std=gnu11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 -Ikitty windows/test_win32_compat.c kitty/win32-compat.c \
    -o build/test-win32-compat.exe -lws2_32 -liphlpapi -lbcrypt -ladvapi32 -lwtsapi32 -lshell32 && ./build/test-win32-compat.exe
python windows/test_unicode_paths.py windows-package/kitsu
```
