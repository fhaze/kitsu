#!/usr/bin/env python
# License: GPLv3 Copyright: 2025, Kovid Goyal <kovid at kovidgoyal.net>

import base64
import json
import os
import re
import struct
import subprocess
import sys
from typing import TYPE_CHECKING, NamedTuple

if TYPE_CHECKING:
    from urllib.request import Request

from .constants import Version, is_windows, kitty_base_dir, kitty_exe, str_version, version

RELEASES_URL = 'https://github.com/fhaze/kitsu/releases/latest'
RELEASES_API_URL = 'https://api.github.com/repos/fhaze/kitsu/releases'
# AppId from windows/kitty.iss -- must never change
INNO_UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\{7D0F3A2B-5C4E-4F19-9B6A-2E8D1C3F5A70}_is1'
# ECDSA P-256 public keys (base64 DER SubjectPublicKeyInfo, as printed by
# openssl pkey -pubout) trusted to sign installers. release.yml signs with the
# private key in the KITSU_UPDATE_SIGNING_KEY secret, which must match one of these.
UPDATE_SIGNING_KEYS: tuple[str, ...] = (
    # Primary, in the KITSU_UPDATE_SIGNING_KEY secret
    'MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAENGTCzJT9JTy65u7CPJJu01Q0F6+3MrusZ0ALSiRT3Slg76RyPUOehFHRaKVcPTxWkY0YbC802PhcJ6NLvPGN0Q==',
    # Offline backup, for rotation if the primary is lost
    'MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEte+DD6xVfqUKcOHMAdoXJFgG6l/3q7k7trsn6H0f4EapgJWp0iMLC5asHhf0CJmR3ITTVAKx0Y89i0EhZg6V9A==',
)
P256_SPKI_PREFIX = bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200')
MAX_SIGNATURE_SIZE = 1024


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


def github_request(url: str) -> 'Request':
    from urllib.request import Request

    return Request(url, headers={'User-Agent': 'kitty-update', 'Accept': 'application/vnd.github+json'})


def fetch_release(ref: str) -> dict:
    from urllib.request import urlopen

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


def installer_signature_url(release: dict, installer_name: str) -> str:
    for asset in release.get('assets', ()):
        if asset.get('name') == installer_name + '.sig':
            return asset['browser_download_url']
    raise SystemExit(f'Release {release.get("tag_name", "<unknown>")} has no signature for {installer_name}, refusing to install an unsigned update')


def signed_manifest(tag: str, name: str, sha256: str) -> bytes:
    # must match the manifest written by the signing step in .github/workflows/release.yml
    return f'kitsu-update-v1\ntag={tag}\nname={name}\nsha256={sha256}\n'.encode()


def p256_public_point(spki_b64: str) -> bytes:
    der = base64.b64decode(spki_b64, validate=True)
    if len(der) != len(P256_SPKI_PREFIX) + 65 or not der.startswith(P256_SPKI_PREFIX) or der[len(P256_SPKI_PREFIX)] != 4:
        raise ValueError('Not an uncompressed ECDSA P-256 public key')
    return der[len(P256_SPKI_PREFIX) + 1 :]


def ecdsa_der_to_raw(sig: bytes) -> bytes:
    # DER ECDSA-Sig-Value (SEQUENCE { INTEGER r, INTEGER s }) to the 64 byte r || s that CNG expects
    if len(sig) < 8 or sig[0] != 0x30 or sig[1] != len(sig) - 2:
        raise ValueError('Malformed ECDSA signature')
    pos, parts = 2, []
    for _ in range(2):
        if pos + 2 > len(sig) or sig[pos] != 2 or not 1 <= sig[pos + 1] <= 33 or pos + 2 + sig[pos + 1] > len(sig):
            raise ValueError('Malformed ECDSA signature')
        value = sig[pos + 2 : pos + 2 + sig[pos + 1]].lstrip(b'\0')
        if len(value) > 32:
            raise ValueError('Malformed ECDSA signature')
        parts.append(value.rjust(32, b'\0'))
        pos += 2 + sig[pos + 1]
    if pos != len(sig):
        raise ValueError('Malformed ECDSA signature')
    return parts[0] + parts[1]


def ecdsa_p256_verify(public_point: bytes, digest: bytes, raw_signature: bytes) -> bool:
    import ctypes as c

    STATUS_INVALID_SIGNATURE = 0xC000A000
    bcrypt = c.WinDLL('bcrypt')  # ty: ignore[unresolved-attribute]
    bcrypt.BCryptOpenAlgorithmProvider.argtypes = (c.POINTER(c.c_void_p), c.c_wchar_p, c.c_wchar_p, c.c_ulong)
    bcrypt.BCryptImportKeyPair.argtypes = (c.c_void_p, c.c_void_p, c.c_wchar_p, c.POINTER(c.c_void_p), c.c_char_p, c.c_ulong, c.c_ulong)
    bcrypt.BCryptVerifySignature.argtypes = (c.c_void_p, c.c_void_p, c.c_char_p, c.c_ulong, c.c_char_p, c.c_ulong, c.c_ulong)
    bcrypt.BCryptDestroyKey.argtypes = (c.c_void_p,)
    bcrypt.BCryptCloseAlgorithmProvider.argtypes = (c.c_void_p, c.c_ulong)
    for f in ('BCryptOpenAlgorithmProvider', 'BCryptImportKeyPair', 'BCryptVerifySignature', 'BCryptDestroyKey', 'BCryptCloseAlgorithmProvider'):
        getattr(bcrypt, f).restype = c.c_ulong
    alg, key = c.c_void_p(), c.c_void_p()
    if status := bcrypt.BCryptOpenAlgorithmProvider(c.byref(alg), 'ECDSA_P256', None, 0):
        raise OSError(f'BCryptOpenAlgorithmProvider failed: 0x{status:08x}')
    try:
        blob = struct.pack('<II', 0x31534345, 32) + public_point  # BCRYPT_ECDSA_PUBLIC_P256_MAGIC
        if status := bcrypt.BCryptImportKeyPair(alg, None, 'ECCPUBLICBLOB', c.byref(key), blob, len(blob), 0):
            raise OSError(f'BCryptImportKeyPair failed: 0x{status:08x}')
        try:
            status = bcrypt.BCryptVerifySignature(key, None, digest, len(digest), raw_signature, len(raw_signature), 0)
        finally:
            bcrypt.BCryptDestroyKey(key)
    finally:
        bcrypt.BCryptCloseAlgorithmProvider(alg, 0)
    if status not in (0, STATUS_INVALID_SIGNATURE):
        raise OSError(f'BCryptVerifySignature failed: 0x{status:08x}')
    return status == 0


def is_validly_signed(tag: str, name: str, sha256: str, signature: bytes, keys: 'tuple[str, ...] | None' = None) -> bool:
    import hashlib

    try:
        raw = ecdsa_der_to_raw(signature)
    except ValueError:
        return False
    digest = hashlib.sha256(signed_manifest(tag, name, sha256)).digest()
    return any(ecdsa_p256_verify(p256_public_point(k), digest, raw) for k in (UPDATE_SIGNING_KEYS if keys is None else keys))


def fetch_signature(url: str) -> bytes:
    from urllib.request import urlopen

    try:
        with urlopen(github_request(url), timeout=30) as response:
            data = response.read(MAX_SIGNATURE_SIZE + 1)
    except Exception as e:
        raise SystemExit(f'Failed to download the installer signature: {e}')
    if len(data) > MAX_SIGNATURE_SIZE:
        raise SystemExit('The installer signature is too large, aborting')
    return data


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
    import hashlib
    from urllib.request import urlopen

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

Check https://github.com/fhaze/kitsu/releases for a newer Windows build of
Kitsu, download its installer, verify its signature against the Kitsu release
signing key and launch it to upgrade this installation in place.

--fetch-version  Which release to fetch: a tag such as v0.48.2-windows.12 or
                 latest (the default). An explicit tag skips the up-to-date
                 check, allowing downgrade/reinstall.
"""


def main(args: list[str]) -> None:
    import tempfile

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

    if not UPDATE_SIGNING_KEYS:
        raise SystemExit(f'This Kitsu build has no update signing key, so updates cannot be verified. Download the latest installer from {RELEASES_URL}')
    name, url, sha256 = windows_setup_asset(release)
    signature = fetch_signature(installer_signature_url(release, name))
    dest_dir = tempfile.mkdtemp(prefix='kitsu-update-')
    installer = download_with_progress(url, sha256, dest_dir, name)
    # download_with_progress() has checked that sha256 is the hash of the downloaded file
    if not is_validly_signed(release['tag_name'], name, sha256, signature):
        os.remove(installer)
        raise SystemExit(f'{name} is not signed by the Kitsu release signing key, refusing to install it')
    print(f'Kitsu {info.tag.lstrip("v")} downloaded. Launching the installer -- it will ask to close running Kitsu windows; re-open Kitsu when it finishes.')
    subprocess.Popen([installer])
    raise SystemExit(0)
