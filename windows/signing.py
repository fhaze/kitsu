#!/usr/bin/env python
"""Authenticode signing of the Windows package.

Signing is provider agnostic. Set KITSU_SIGN_COMMAND to a command line that
signs the files it is given, written in Inno Setup's SignTool syntax: $f stands
for the quoted file names, $q for a double quote and $$ for a dollar sign. For
example, with a certificate in the user's certificate store:

    $qC:\\...\\signtool.exe$q sign /fd sha256 /sha1 <thumbprint> /tr http://time.certum.pl /td sha256 $f

`python setup.py windows-package` then signs Kitsu's own binaries before it
zips the package, and has Inno Setup sign the installer and the uninstaller
with the same command. Without KITSU_SIGN_COMMAND nothing is signed.

Only files built from this repository are signed. Third-party files (the MSYS2
runtime, Python, slangc, less, OpenConsole) are shipped as their vendors built
them, since the dependency inventory identifies MSYS2 files by their hash.

Run as a script to check the signatures of a package:

    python windows/signing.py verify windows-package/kitsu [installer.exe ...] [--thumbprint X] [--require-timestamp]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from collections.abc import Callable, Iterable, Sequence
from pathlib import Path

SIGN_COMMAND_ENV = 'KITSU_SIGN_COMMAND'
# The name the installer script knows the sign command by, see SignTool in kitty.iss
INNO_SIGN_TOOL = 'kitsusign'

# Must exist in every package, so that a build that stops producing one of them fails
REQUIRED_FILES = (
    'bin/kitsu.exe',
    'bin/kitsu.com',
    'bin/kitty.exe',
    'bin/kitty.com',
    'bin/kitten.exe',
    'bin/health-check.ps1',
    'cmd/kitsu.exe',
    'cmd/kitsu.com',
    'cmd/kitty.exe',
    'cmd/kitty.com',
    'cmd/kitten.exe',
    'lib/kitty/kitty/fast_data_types.pyd',
    'lib/kitty/kitty/glfw-win32.dll',
    'lib/kitty/kittens/transfer/rsync.pyd',
)
PE_SUFFIXES = ('.exe', '.com', '.dll', '.pyd')
# signtool's command line, like any other, is limited to 32767 characters
MAX_COMMAND_LINE = 8000


def first_party_files(package: Path) -> list[Path]:
    """The files in the package that are built from this repository, sorted."""
    missing = [name for name in REQUIRED_FILES if not (package / name).is_file()]
    if missing:
        raise ValueError(f'Files to sign are missing from {package}: {", ".join(missing)}')
    found = {package / name for name in REQUIRED_FILES}
    if (package / 'terminal-identity.msix').is_file():
        if not (package / 'bin/kitsu-handoff.exe').is_file():
            raise ValueError(f'Terminal identity launcher is missing from {package}: bin/kitsu-handoff.exe')
        found.add(package / 'terminal-identity.msix')
        found.add(package / 'bin' / 'kitsu-handoff.exe')
    # the launchers on PATH and the extension modules of the kitty package are all built here
    for directory in (package / 'cmd', package / 'lib' / 'kitty'):
        found.update(p for p in directory.rglob('*') if p.is_file() and p.suffix.lower() in PE_SUFFIXES)
    return sorted(found)


def sign_command(env: dict[str, str] | None = None) -> str:
    cmd = (os.environ if env is None else env).get(SIGN_COMMAND_ENV, '').strip()
    if cmd and '$f' not in cmd:
        raise ValueError(f'{SIGN_COMMAND_ENV} must contain $f where the files to sign go')
    return cmd


def quote(path: str) -> str:
    if '"' in path:
        raise ValueError(f'Cannot quote a path that contains a double quote: {path}')
    return f'"{path}"'


def expand(cmd: str, paths: Sequence[str]) -> str:
    """Expands a command in Inno Setup's SignTool syntax for the given files."""
    out: list[str] = []
    i = 0
    while i < len(cmd):
        if cmd[i] == '$' and i + 1 < len(cmd):
            c = cmd[i + 1]
            if c in 'fq$':
                out.append(' '.join(map(quote, paths)) if c == 'f' else ('"' if c == 'q' else '$'))
                i += 2
                continue
        out.append(cmd[i])
        i += 1
    return ''.join(out)


def batches(paths: Sequence[str], limit: int = MAX_COMMAND_LINE) -> Iterable[list[str]]:
    batch: list[str] = []
    size = 0
    for path in paths:
        if batch and size + len(path) + 3 > limit:
            yield batch
            batch, size = [], 0
        batch.append(path)
        size += len(path) + 3
    if batch:
        yield batch


def run_command(cmdline: str) -> None:
    # A string is passed to CreateProcess as is, so the quoting above is what signtool sees
    cp = subprocess.run(cmdline, shell=False)
    if cp.returncode != 0:
        raise SystemExit(f'Signing failed with exit code {cp.returncode}')


def sign_files(paths: Sequence[Path], cmd: str, run: Callable[[str], None] = run_command) -> None:
    for batch in batches([os.path.abspath(p) for p in paths]):
        run(expand(cmd, batch))


def sign_package(package: Path, cmd: str | None = None, run: Callable[[str], None] = run_command) -> list[Path]:
    """Signs the first-party files of the package when a sign command is configured."""
    cmd = sign_command() if cmd is None else cmd
    if not cmd:
        return []
    files = first_party_files(package)
    aliases = {package / 'bin' / ('kitty' + ext): package / 'bin' / ('kitsu' + ext) for ext in ('.exe', '.com')}
    for alias, source in aliases.items():
        if alias.read_bytes() != source.read_bytes():
            raise ValueError(f'Compatibility alias {alias} differs from {source} before signing')
    # Timestamped signatures differ between invocations. Copy the signed launchers so their
    # compatibility aliases remain identical, including their valid signature and timestamp.
    to_sign = [p for p in files if p not in aliases]
    print(f'Signing {len(to_sign)} files in {package}', flush=True)
    sign_files(to_sign, cmd, run)
    for alias, source in aliases.items():
        shutil.copy2(source, alias)
    return files


def iscc_arguments(cmd: str | None = None) -> list[str]:
    """Arguments that make ISCC sign the installer and uninstaller, empty when not signing."""
    cmd = sign_command() if cmd is None else cmd
    if not cmd:
        return []
    # ISCC does not understand backslash escaped quotes on its command line, $q is its own way
    return [f'/DSignTool={INNO_SIGN_TOOL}', f'/S{INNO_SIGN_TOOL}={cmd.replace(chr(34), "$q")}']


# Verification {{{

VERIFY_SCRIPT = r'''
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$paths = $env:KITSU_VERIFY_PATHS | ConvertFrom-Json
$ans = foreach ($p in $paths) {
    $s = Get-AuthenticodeSignature -LiteralPath $p
    [pscustomobject]@{
        path = $p
        status = [string]$s.Status
        message = [string]$s.StatusMessage
        thumbprint = if ($s.SignerCertificate) { $s.SignerCertificate.Thumbprint } else { '' }
        subject = if ($s.SignerCertificate) { $s.SignerCertificate.Subject } else { '' }
        timestamped = [bool]$s.TimeStamperCertificate
    }
}
ConvertTo-Json -InputObject @($ans) -Compress
'''


def read_signatures(paths: Sequence[Path]) -> list[dict[str, object]]:
    env = dict(os.environ, KITSU_VERIFY_PATHS=json.dumps([os.path.abspath(p) for p in paths]))
    out = subprocess.run(
        ['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', VERIFY_SCRIPT],
        env=env, check=True, stdout=subprocess.PIPE,
    ).stdout
    return json.loads(out.decode('utf-8'))


def check_signatures(
    signatures: Iterable[dict[str, object]], thumbprint: str = '', require_timestamp: bool = False
) -> list[str]:
    """Returns a description of every problem, empty when all signatures are good."""
    thumbprint = ''.join(c for c in thumbprint if c.isalnum()).upper()
    problems = []
    for s in signatures:
        path = s['path']
        if s['status'] != 'Valid':
            problems.append(f'{path}: {s["status"]}' + (f': {s["message"]}' if s['message'] else ''))
        elif thumbprint and str(s['thumbprint']).upper() != thumbprint:
            problems.append(f'{path}: signed by {s["subject"]} ({s["thumbprint"]}), not by {thumbprint}')
        elif require_timestamp and not s['timestamped']:
            problems.append(f'{path}: the signature has no timestamp, it stops being valid when the certificate expires')
    return problems


def main(argv: Sequence[str] = sys.argv[1:]) -> int:
    p = argparse.ArgumentParser(description='Check the Authenticode signatures of a Kitsu Windows package')
    sub = p.add_subparsers(dest='action', required=True)
    v = sub.add_parser('verify', help='Check that the first-party files of a package, and the given installers, are signed')
    v.add_argument('package', type=Path, help='The package directory, for example windows-package/kitsu')
    v.add_argument('extra', type=Path, nargs='*', help='More files to check, for example the installer')
    v.add_argument('--thumbprint', default='', help='The SHA-1 thumbprint of the certificate that must have signed every file')
    v.add_argument('--require-timestamp', action='store_true', help='Require an RFC 3161 timestamp on every signature')
    ls = sub.add_parser('list', help='List the files of a package that get signed')
    ls.add_argument('package', type=Path)
    args = p.parse_args(argv)
    files = first_party_files(args.package)
    if args.action == 'list':
        print('\n'.join(str(f.relative_to(args.package).as_posix()) for f in files))
        return 0
    files += args.extra
    problems = check_signatures(read_signatures(files), args.thumbprint, args.require_timestamp)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(f'{len(problems)} of {len(files)} files are not signed as required', file=sys.stderr)
        return 1
    print(f'All {len(files)} files are signed as required')
    return 0


# }}}

if __name__ == '__main__':
    raise SystemExit(main())
