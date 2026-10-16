#!/usr/bin/env bash
# Build Kitsu's Windows package (MSYS2 UCRT64).
#
# Usage:
#   ./build-windows.sh                  build the package
#   ./build-windows.sh --install-deps   install toolchain + deps first, then build
#   ./build-windows.sh --debug          extra args are passed to setup.py
#
# Run it from Git Bash or an MSYS2 shell; it re-enters the MSYS2 UCRT64
# shell with the correct environment on its own.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIMDE_VERSION=0.8.2
SLANG_VERSION=2026.14.1
NERD_FONT=SymbolsNerdFontMono-Regular.ttf
PACMAN_PACKAGES="make git ncurses unzip
  mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf
  mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-freetype
  mingw-w64-ucrt-x86_64-harfbuzz mingw-w64-ucrt-x86_64-libpng
  mingw-w64-ucrt-x86_64-lcms2 mingw-w64-ucrt-x86_64-xxhash
  mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-zlib
  mingw-w64-ucrt-x86_64-cairo mingw-w64-ucrt-x86_64-fontconfig
  mingw-w64-ucrt-x86_64-mesa mingw-w64-ucrt-x86_64-libsystre"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- inner phase

inner_build() {
    local localappdata="$1" userprofile="$2" deps_dir="$3"
    shift 3

    export MSYSTEM=UCRT64
    export LOCALAPPDATA="$(cygpath -m "$localappdata")"
    export USERPROFILE="$(cygpath -m "$userprofile")"
    export PYTHONUTF8=1
    export CPPFLAGS="-I$(cygpath -m "$deps_dir/simde-$SIMDE_VERSION")"
    export SLANGC="$(cygpath -m "$deps_dir/slang/bin/slangc.exe")"

    if ! command -v go >/dev/null && [ -x "/c/Program Files/Go/bin/go.exe" ]; then
        export PATH="/c/Program Files/Go/bin:$PATH"
    fi

    log "Checking build tools"
    command -v gcc >/dev/null      || die "gcc not found — run: $0 --install-deps"
    command -v pkgconf >/dev/null  || die "pkgconf not found — run: $0 --install-deps"
    command -v go >/dev/null       || die "go not found — run: $0 --install-deps"
    python -c 'import sys; sys.exit(sys.platform != "win32")' \
        || die "not running under MSYS2 UCRT64 python"
    [ -f "$deps_dir/simde-$SIMDE_VERSION/simde/simde-common.h" ] \
        || die "SIMDe headers missing in $deps_dir — run: $0 --install-deps"
    [ -f "$deps_dir/slang/bin/slangc.exe" ] \
        || die "slangc missing in $deps_dir — run: $0 --install-deps"
    [ -f "$localappdata/Microsoft/Windows/Fonts/$NERD_FONT" ] \
        || die "$NERD_FONT not installed — run: $0 --install-deps"

    log "Building (gcc $(gcc -dumpversion), go $(go env GOVERSION), python $(python -c 'import sys; print(sys.version.split()[0])'))"
    cd "$REPO_ROOT"
    python setup.py windows-package "$@"
    log "Done: $(ls "$REPO_ROOT"/windows-package/kitsu-*-windows-*.zip | tail -1)"
}

# --------------------------------------------------------- dependency install

find_msys2() {
    local root
    for root in "${MSYS2_ROOT:-}" /c/msys64 "/c/Program Files/msys2" /c/tools/msys64; do
        [ -n "$root" ] && [ -x "$root/usr/bin/bash.exe" ] && { echo "$root"; return; }
    done
    return 1
}

win_path() { # resolve a Windows per-user path even when MSYS2 stripped the env var
    local value="" folder=UserProfile
    case "$1" in
        USERPROFILE)  value="${USERPROFILE:-}" ;;
        LOCALAPPDATA) value="${LOCALAPPDATA:-}"; folder=LocalApplicationData ;;
    esac
    if [ -n "$value" ]; then cygpath -u "$value"; return; fi
    powershell.exe -NoProfile -Command "[Environment]::GetFolderPath('$folder')" \
        | tr -d '\r\n' | { read -r v; [ -n "$v" ] && cygpath -u "$v"; }
}

install_deps() {
    local msys2_root="$1" deps_dir="$2" localappdata="$3"

    if [ -z "$msys2_root" ]; then
        log "Installing MSYS2 via winget"
        winget.exe install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements --silent
        msys2_root="$(find_msys2)" || die "MSYS2 still not found after install"
    fi

    if ! [ -x "/c/Program Files/Go/bin/go.exe" ] && ! command -v go >/dev/null; then
        log "Installing Go via winget"
        winget.exe install --id GoLang.Go -e --accept-source-agreements --accept-package-agreements --silent
    fi

    log "Installing MSYS2/UCRT64 packages"
    local pkgs
    pkgs=$(echo $PACMAN_PACKAGES) # collapse newlines for the -lc string
    "$msys2_root/usr/bin/bash.exe" -lc "pacman -Syu --noconfirm && pacman -S --needed --noconfirm $pkgs"

    mkdir -p "$deps_dir"
    if ! [ -f "$deps_dir/simde-$SIMDE_VERSION/simde/simde-common.h" ]; then
        log "Downloading SIMDe $SIMDE_VERSION"
        curl -fsSL https://github.com/simd-everywhere/simde/archive/refs/tags/v$SIMDE_VERSION.tar.gz \
            | tar -xz -C "$deps_dir"
    fi

    if ! [ -f "$deps_dir/slang/bin/slangc.exe" ]; then
        log "Downloading slang $SLANG_VERSION"
        curl -fsSL -o "$deps_dir/slang.zip" "https://github.com/shader-slang/slang/releases/download/v$SLANG_VERSION/slang-$SLANG_VERSION-windows-x86_64.zip"
        unzip -qo "$deps_dir/slang.zip" -d "$deps_dir/slang"
        rm "$deps_dir/slang.zip"
    fi

    if ! [ -f "$localappdata/Microsoft/Windows/Fonts/$NERD_FONT" ]; then
        log "Installing $NERD_FONT"
        mkdir -p "$localappdata/Microsoft/Windows/Fonts"
        curl -fsSL -o "$deps_dir/nerd.zip" https://github.com/ryanoasis/nerd-fonts/releases/latest/download/NerdFontsSymbolsOnly.zip
        unzip -qo -j "$deps_dir/nerd.zip" "$NERD_FONT" -d "$localappdata/Microsoft/Windows/Fonts"
        rm "$deps_dir/nerd.zip"
    fi
    "$msys2_root/usr/bin/bash.exe" -lc "fc-cache -f >/dev/null" || true
}

# -------------------------------------------------------------------- main

if [ "${1:-}" = "--inner" ]; then
    shift
    inner_build "$@"
    exit 0
fi

install=0
args=()
for a in "$@"; do
    if [ "$a" = "--install-deps" ]; then install=1; else args+=("$a"); fi
done

msys2_root="$(find_msys2 || true)"
[ -n "$msys2_root" ] || [ "$install" = 1 ] \
    || die "MSYS2 not found — run: $0 --install-deps"

userprofile="$(win_path USERPROFILE)"
localappdata="$(win_path LOCALAPPDATA)"
[ -n "$localappdata" ] || die "could not resolve LOCALAPPDATA"
deps_dir="${KITTY_BUILD_DEPS:-$userprofile/build-deps}"

if [ "$install" = 1 ]; then
    install_deps "$msys2_root" "$deps_dir" "$localappdata"
    msys2_root="$(find_msys2)" || die "MSYS2 not found after install"
fi

log "Entering MSYS2 UCRT64 shell ($msys2_root)"
MSYSTEM=UCRT64 MSYS2_PATH_TYPE=inherit exec "$msys2_root/usr/bin/bash.exe" -l \
    "$REPO_ROOT/build-windows.sh" --inner "$localappdata" "$userprofile" "$deps_dir" ${args[@]+"${args[@]}"}
