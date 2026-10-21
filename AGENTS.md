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
    -o build/test-win32-compat.exe -lws2_32 -lbcrypt -ladvapi32 -lwtsapi32 -lshell32 && ./build/test-win32-compat.exe
python windows/test_unicode_paths.py windows-package/kitsu
```

### Code signing

`python setup.py windows-package` Authenticode signs Kitsu's own files (the `bin\` and
`cmd\` launchers, `kitten.exe`, the `.pyd`/`.dll` files under `lib\kitty`, `health-check.ps1`)
before zipping, and Inno Setup signs `setup.exe` and `unins000.exe`, whenever
`KITSU_SIGN_COMMAND` is set. It is a command line in Inno Setup's SignTool syntax (`$f` = the
quoted files, `$q` = `"`), so any signing provider works. Third-party files (MSYS2, Python,
slangc, less, OpenConsole) stay as their vendors shipped them: the SBOM matches MSYS2 files by
hash. The list and the checks live in `windows/signing.py`:

```sh
python windows/signing.py list windows-package/kitsu
python windows/signing.py verify windows-package/kitsu windows-package/kitsu-*-setup.exe --thumbprint <sha1>
```

CI builds sign with a throwaway self-signed certificate trusted only on the runner, so every
build exercises signing. Release builds (`v*` tags) sign with the Certum open source
certificate. Its key is in Certum's cloud (SimplySign): `windows/Connect-SimplySign.ps1`
installs SimplySign Desktop on the runner and logs in with a TOTP code computed from the seed,
then `signtool` finds the certificate by thumbprint and timestamps with `http://time.certum.pl`.

One-time setup, in the repository settings under Environments, create `release-signing` with:

- **Protection:** required reviewer (yourself); deployment branches and tags: `v*` tags, plus
  `master` for the manual `Signing check` workflow.
- **Secret `CERTUM_USERNAME`:** the SimplySign account e-mail.
- **Secret `CERTUM_OTP_URI`:** the `otpauth://totp/...?secret=...` URI in the QR code you scan
  to pair the SimplySign mobile app. It is as sensitive as the private key: keep it only there
  and in the phone. If it leaks, re-pair the app in Certum's portal to get a new one.
- **Variable `CERTUM_CERT_SHA1`:** the certificate thumbprint. With SimplySign Desktop logged in
  on a PC: `Get-ChildItem Cert:\CurrentUser\My | Format-List Subject, Thumbprint, NotAfter`.

Then run the `Signing check` workflow once. Repeated failed logins can lock the Certum account
(the script stops after three), so check the secrets before retrying. When the certificate is
renewed, update `CERTUM_CERT_SHA1`. SmartScreen reputation is tied to the certificate, so expect
warnings on the first releases after a new one.
