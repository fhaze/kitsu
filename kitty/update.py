#!/usr/bin/env python
# License: GPLv3 Copyright: 2025, Kovid Goyal <kovid at kovidgoyal.net>

import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from typing import NamedTuple
from urllib.request import Request, urlopen

from .constants import Version, is_windows, kitty_base_dir, kitty_exe, str_version, version

RELEASES_URL = 'https://github.com/fhaze/kitty/releases/latest'
RELEASES_API_URL = 'https://api.github.com/repos/fhaze/kitty/releases'
# AppId from windows/kitty.iss -- must never change
INNO_UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\{7D0F3A2B-5C4E-4F19-9B6A-2E8D1C3F5A70}_is1'


class ReleaseInfo(NamedTuple):
    base: Version  # the upstream kitty version
    build: int  # the -windows.N suffix, 0 when absent
    tag: str  # full tag, e.g. 'v0.48.2-windows.12'


def parse_release_tag(tag: str) -> 'ReleaseInfo | None':
    if m := re.match(r'^v?(\d+)\.(\d+)\.(\d+)(?:-windows\.(\d+))?$', tag):
        base = Version(int(m.group(1)), int(m.group(2)), int(m.group(3)))
        build = int(m.group(4) or 0)
        return ReleaseInfo(base, build, tag)
    return None


def installed_release_info() -> ReleaseInfo:
    try:
        with open(os.path.join(kitty_base_dir, 'release-tag'), encoding='utf-8') as f:
            stamped = parse_release_tag(f.read().strip())
    except OSError:
        stamped = None
    if stamped is not None:
        # the stamp is authoritative for the build number only
        return ReleaseInfo(version, stamped.build, stamped.tag)
    return ReleaseInfo(version, 0, 'v' + str_version + '-windows.0')


def is_newer(candidate: ReleaseInfo, installed: ReleaseInfo) -> bool:
    return candidate.base > installed.base or (candidate.base == installed.base and candidate.build > installed.build)


def github_request(url: str) -> Request:
    return Request(url, headers={'User-Agent': 'kitty-update', 'Accept': 'application/vnd.github+json'})


def fetch_release(ref: str) -> dict:
    url = f'{RELEASES_API_URL}/latest' if ref == 'latest' else f'{RELEASES_API_URL}/tags/{ref}'
    try:
        return json.loads(urlopen(github_request(url), timeout=30).read())
    except Exception as e:
        raise SystemExit(f'Failed to query GitHub releases: {e}\nDownload the latest installer manually from {RELEASES_URL}')


def windows_setup_asset(release: dict) -> 'tuple[str, str, str]':
    tag = release.get('tag_name', '<unknown>')
    for asset in release.get('assets', ()):
        if re.match(r'^(?:kitsu|kitty)-.+-windows-x86_64-setup\.exe$', asset.get('name', '')):
            digest = asset.get('digest') or ''
            if not digest.startswith('sha256:'):
                raise SystemExit(f'The Kitsu installer in release {tag} has no SHA-256 digest, refusing to download without verification')
            return asset['name'], asset['browser_download_url'], digest[len('sha256:') :]
    raise SystemExit(f'Release {tag} contains no Windows Kitsu installer (kitsu-*-windows-x86_64-setup.exe)')


def inno_install_prefix() -> 'str | None':
    import winreg

    expected = os.path.normcase(os.path.normpath(os.path.dirname(os.path.dirname(kitty_exe()))))
    for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):  # ty: ignore[unresolved-attribute]
        try:
            with winreg.OpenKey(root, INNO_UNINSTALL_KEY) as key:  # ty: ignore[unresolved-attribute]
                location, _ = winreg.QueryValueEx(key, 'InstallLocation')  # ty: ignore[unresolved-attribute]
        except OSError:
            continue
        if os.path.normcase(os.path.normpath(location)) == expected:
            return expected
    return None


def download_with_progress(url: str, sha256: str, dest_dir: str, name: str) -> str:
    dest = os.path.join(dest_dir, name)
    with urlopen(github_request(url), timeout=30) as response:
        total = int(response.headers.get('Content-Length') or 0)
        hasher = hashlib.sha256()
        downloaded = 0
        last_pct = -1
        with open(dest, 'wb') as f:
            while chunk := response.read(256 * 1024):
                f.write(chunk)
                hasher.update(chunk)
                downloaded += len(chunk)
                if total:
                    pct = downloaded * 100 // total
                    if pct > last_pct:
                        last_pct = pct
                        print(f'\rDownloaded {downloaded / 1024 / 1024:.1f}/{total / 1024 / 1024:.1f} MB ({pct}%)', end='', file=sys.stderr)
        if total:
            print(file=sys.stderr)
    if hasher.hexdigest() != sha256:
        os.remove(dest)
        raise SystemExit(f'SHA-256 mismatch for {name}, the download is corrupted or tampered with, aborting')
    return dest


USAGE = """\
Usage: kitsu +update [--fetch-version <tag|latest>]

Check https://github.com/fhaze/kitty/releases for a newer Windows build of
Kitsu, download its installer (with SHA-256 verification) and launch it to
upgrade this installation in place.

--fetch-version  Which release to fetch: a tag such as v0.48.2-windows.12 or
                 latest (the default). An explicit tag skips the up-to-date
                 check, allowing downgrade/reinstall.
"""


def main(args: list[str]) -> None:
    ref = 'latest'
    rest = args[1:] if args and args[0] == 'update' else args
    i = 0
    while i < len(rest):
        arg = rest[i]
        if arg in ('-h', '--help'):
            print(USAGE)
            return
        if arg == '--fetch-version':
            i += 1
            if i >= len(rest):
                raise SystemExit('--fetch-version requires an argument: a release tag or latest\n' + USAGE)
            ref = rest[i]
        else:
            raise SystemExit(f'Unknown argument: {arg}\n' + USAGE)
        i += 1

    if not is_windows:
        raise SystemExit('kitty +update is only supported on Windows. On other platforms use: kitten update-self (standalone kitten) or your package manager.')
    run_data = getattr(sys, 'kitty_run_data', {})
    # the Windows launcher always sets sys.frozen to False, so identify
    # development/source builds via kitty_run_data instead
    if run_data.get('from_source') or not run_data.get('bundle_exe_dir'):
        raise SystemExit('kitsu +update only works in installed builds, not development/source builds.')
    if inno_install_prefix() is None:
        raise SystemExit(
            f'This Kitsu was not installed with the Kitsu installer (portable zip or unknown location). Download the latest installer from {RELEASES_URL}'
        )

    release = fetch_release(ref)
    info = parse_release_tag(release.get('tag_name', ''))
    if info is None:
        raise SystemExit(f'Release has a malformed tag: {release.get("tag_name")!r}, expected something like v0.48.2-windows.12')
    if ref == 'latest' and not is_newer(info, installed_release_info()):
        print(f'Kitsu is already up to date ({installed_release_info().tag})')
        return

    name, url, sha256 = windows_setup_asset(release)
    dest_dir = tempfile.mkdtemp(prefix='kitsu-update-')
    installer = download_with_progress(url, sha256, dest_dir, name)
    print(f'Kitsu {info.tag.lstrip("v")} downloaded. Launching the installer -- it will ask to close running Kitsu windows; re-open Kitsu when it finishes.')
    subprocess.Popen([installer])
    raise SystemExit(0)
