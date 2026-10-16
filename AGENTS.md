# AGENTS.md

## Building on Windows (local dev)

This fork builds natively on Windows via MSYS2 UCRT64. The canonical recipe is the
`Build kitty.exe (MSYS2 UCRT64)` job in `.github/workflows/windows-build.yml`, which runs
`python setup.py windows-package`.

Use the helper script instead of invoking anything by hand:

```sh
./build-windows.sh                  # build → windows-package/kitty-*-windows-x86_64.zip
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
