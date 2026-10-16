#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

# Install the Linux kitten binary shipped with the Windows package into the
# user's WSL distributions, so that kitten and the shell integration work
# inside WSL just as they do on Linux. Runs windows/wsl/install-kitten.sh in
# each distribution, driven by kitty +wsl-setup and by the Windows installer.

import codecs
import os
import subprocess
import sys
from collections.abc import Sequence

from .constants import helper_process_popen_kwargs, is_windows, kitty_base_dir

wsl_dir = os.path.join(kitty_base_dir, 'windows', 'wsl')
install_script = os.path.join(wsl_dir, 'install-kitten.sh')
# distributions that are implementation details of other programs
utility_distro_prefixes = ('docker-desktop', 'rancher-desktop', 'podman-machine')
distro_timeout = 180

usage = """\
Usage: kitsu +wsl-setup [options] [distribution ...]

Install the kitten binary into WSL distributions and add it to the PATH of the
login shell in each. All distributions are set up when none are specified.

Options:
  --uninstall  Remove kitten and the PATH entry instead of installing them
  --no-path    Install kitten but do not touch shell startup files
  --help       Show this message
"""


def wsl_exe() -> str:
    return os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'wsl.exe')


def decode_wsl_output(data: bytes) -> str:
    # wsl.exe writes UTF-16LE when its output is not a console
    if data.startswith(codecs.BOM_UTF16_LE) or (len(data) > 1 and data[1] == 0):
        return data.decode('utf-16-le', 'replace').lstrip('\ufeff')
    return data.decode('utf-8', 'replace')


def parse_distro_list(output: str) -> list[str]:
    ans = []
    for line in output.splitlines():
        name = line.strip()
        if name and not name.lower().startswith(utility_distro_prefixes):
            ans.append(name)
    return ans


def list_distros() -> list[str]:
    exe = wsl_exe()
    if not os.path.exists(exe):
        return []
    try:
        cp = subprocess.run([exe, '--list', '--quiet'], capture_output=True, timeout=60, **helper_process_popen_kwargs())
    except subprocess.TimeoutExpired:
        print('Timed out listing WSL distributions', file=sys.stderr)
        return []
    if cp.returncode != 0:
        # wsl.exe exits non-zero when WSL is not enabled or has no distributions
        return []
    return parse_distro_list(decode_wsl_output(cp.stdout))


def wsl_command(distro: str, script_args: Sequence[str]) -> list[str]:
    return [wsl_exe(), '--distribution', distro, '--exec', 'sh', '-s', '--', *script_args]


def run_in_distro(distro: str, uninstall: bool = False, update_path: bool = True) -> bool:
    script_args = []
    if uninstall:
        script_args.append('--uninstall')
    else:
        if not update_path:
            script_args.append('--no-path')
        script_args.append(wsl_dir)
    with open(install_script, 'rb') as f:
        script = f.read().replace(b'\r\n', b'\n')
    try:
        cp = subprocess.run(wsl_command(distro, script_args), input=script, capture_output=True, timeout=distro_timeout, **helper_process_popen_kwargs())
    except subprocess.TimeoutExpired:
        print(f'[{distro}] timed out after {distro_timeout} seconds', file=sys.stderr)
        return False
    for stream, data in ((sys.stdout, cp.stdout), (sys.stderr, cp.stderr)):
        for line in decode_wsl_output(data).splitlines():
            if line.strip():
                print(f'[{distro}] {line.rstrip()}', file=stream)
    if cp.returncode != 0:
        print(f'[{distro}] failed with exit code {cp.returncode}', file=sys.stderr)
        return False
    return True


def main(args: Sequence[str]) -> None:
    uninstall = False
    update_path = True
    distros: list[str] = []
    for arg in args[1:]:
        if arg == '--uninstall':
            uninstall = True
        elif arg == '--no-path':
            update_path = False
        elif arg in ('-h', '--help'):
            print(usage)
            return
        elif arg.startswith('-'):
            raise SystemExit(f'Unknown option: {arg}\n\n{usage}')
        else:
            distros.append(arg)
    if not is_windows:
        raise SystemExit('kitty +wsl-setup only works on Windows')
    if not os.path.exists(install_script):
        raise SystemExit(f'{install_script} is missing, this Kitsu installation was not packaged for Windows')
    if not distros:
        distros = list_distros()
        if not distros:
            print('No WSL distributions found, nothing to do')
            return
    failed = [d for d in distros if not run_in_distro(d, uninstall=uninstall, update_path=update_path)]
    if failed:
        raise SystemExit(f'Setting up WSL failed for: {", ".join(failed)}')
