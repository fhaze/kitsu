#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kitsu contributors

"""Explicit, per-user default-terminal integration; never changes defaults at startup."""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

CONSOLE_CLSID = '{2EACA947-7F5F-4CFA-BA87-8F7FBEEFBE69}'
TERMINAL_CLSID = '{75AB2A9B-3128-4777-9D2B-3126EC262B1D}'
AUTOMATIC_CLSID = '{00000000-0000-0000-0000-000000000000}'
STARTUP_KEY = r'Console\%%Startup'
BACKUP_KEY = r'Software\FHaze\Kitsu\DefaultTerminal'
DELEGATION_VALUES = ('DelegationConsole', 'DelegationTerminal')


def read_delegation() -> tuple[str | None, str | None]:
    if sys.platform != 'win32':
        raise OSError('Windows only')
    import winreg

    ans: list[str | None] = []
    for name in DELEGATION_VALUES:
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, STARTUP_KEY) as key:
                value, kind = winreg.QueryValueEx(key, name)
                if kind != winreg.REG_SZ:
                    raise ValueError(f'{name} is not a registry string')
                ans.append(value)
        except FileNotFoundError:
            ans.append(None)
    return ans[0], ans[1]


def write_delegation(values: tuple[str | None, str | None]) -> None:
    if sys.platform != 'win32':
        raise OSError('Windows only')
    import winreg

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, STARTUP_KEY) as key:
        for name, value in zip(DELEGATION_VALUES, values):
            if value is None:
                try:
                    winreg.DeleteValue(key, name)
                except FileNotFoundError:
                    pass
            else:
                winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)


def is_kitsu(values: tuple[str | None, str | None]) -> bool:
    return tuple((v or '').upper() for v in values) == (CONSOLE_CLSID, TERMINAL_CLSID)


def enable() -> None:
    if sys.platform != 'win32':
        raise OSError('Windows only')
    import winreg

    previous = read_delegation()
    if is_kitsu(previous):
        return
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, BACKUP_KEY) as key:
        winreg.SetValueEx(key, 'PreviousDelegation', 0, winreg.REG_SZ, json.dumps(previous))
    try:
        write_delegation((CONSOLE_CLSID, TERMINAL_CLSID))
    except OSError:
        write_delegation(previous)
        raise


def restore() -> None:
    if sys.platform != 'win32':
        raise OSError('Windows only')
    import winreg

    # Another terminal may have been selected since Kitsu: do not overwrite it.
    if not is_kitsu(read_delegation()):
        return
    previous: tuple[str | None, str | None] = (AUTOMATIC_CLSID, AUTOMATIC_CLSID)
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, BACKUP_KEY) as key:
            raw = json.loads(winreg.QueryValueEx(key, 'PreviousDelegation')[0])
        if not isinstance(raw, list) or len(raw) != 2 or any(v is not None and not isinstance(v, str) for v in raw):
            raise ValueError('Invalid Kitsu delegation backup')
        previous = raw[0], raw[1]
    except FileNotFoundError:
        pass
    write_delegation(previous)
    try:
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, BACKUP_KEY)
    except FileNotFoundError:
        pass


def package_command(action: str, root: Path) -> None:
    exe = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    # All paths are environment data, not interpolated PowerShell source.
    script = "$ErrorActionPreference='Stop'; "
    if action == 'register':
        if not (root / 'terminal-identity.msix').is_file():
            raise ValueError('This build has no signed terminal-identity.msix; use an official signed Windows package')
        script += 'Add-AppxPackage -Path (Join-Path $env:KITSU_TERMINAL_ROOT terminal-identity.msix) -ExternalLocation $env:KITSU_TERMINAL_ROOT'
    else:
        script += 'Get-AppxPackage -Name FHaze.Kitsu.Terminal | Remove-AppxPackage'
    subprocess.run([str(exe), '-NoProfile', '-NonInteractive', '-Command', script], check=True, env=dict(os.environ, KITSU_TERMINAL_ROOT=str(root)))


def main(args: list[str]) -> None:
    parser = argparse.ArgumentParser(prog='kitsu +default-terminal', description=__doc__)
    parser.add_argument('action', choices=('register', 'enable', 'restore', 'unregister', 'status'))
    action = parser.parse_args(args[1:]).action
    if sys.platform != 'win32':
        parser.error('Windows only')
    if action in ('register', 'enable') and sys.getwindowsversion().build < 22621:
        parser.error('Default-terminal integration requires Windows 11 22H2 or later')
    root = Path(sys.executable).resolve().parent.parent
    try:
        if action in ('register', 'enable'):
            package_command('register', root)
        if action == 'enable':
            enable()
        elif action == 'restore':
            restore()
        elif action == 'unregister':
            restore()
            package_command('unregister', root)
        if action == 'status':
            print(json.dumps(dict(zip(DELEGATION_VALUES, read_delegation())), indent=2))
        elif action == 'register':
            print('Registered Kitsu. Select it in Windows Settings > System > For developers > Terminal (location varies by Windows version).')
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        raise SystemExit(str(e)) from e
