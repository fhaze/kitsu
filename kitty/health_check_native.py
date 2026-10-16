#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kitty contributors

import base64
import json
import os
import secrets
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from .health_check import Deadline, Probe, Worker, command, require

RC_PASSWORD_ENV = 'KITTY_HEALTH_CHECK_RC_PASSWORD'
RC_ACTIONS = 'ls get-text launch close-window close-tab focus-window focus-tab resize-os-window screenshot kitten'

NATIVE = (
    Probe('native.console'),
    Probe('native.startup', prerequisites=('runtime.identity', 'runtime.imports', 'runtime.glfw', 'runtime.shader')),
    Probe('native.authentication', prerequisites=('native.startup',)),
    Probe('native.terminal', prerequisites=('native.authentication',)),
    Probe('native.shells', prerequisites=('native.authentication',)),
    Probe('native.layout', prerequisites=('native.authentication',)),
    Probe('native.graphics', prerequisites=('native.authentication',)),
    Probe('native.renderer', prerequisites=('native.authentication',)),
    Probe('native.cleanup'),
)


def write_json(path: Path, value: Any) -> None:
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(value), encoding='utf-8')
    tmp.replace(path)


def gui_worker() -> None:
    password = os.environ.pop(RC_PASSWORD_ENV)
    from kitty.main import main

    getattr(sys, 'kitty_run_data').pop('cli_flags', None)
    sys.argv[0] = 'kitty'
    # Python-only argv: the credential never appears in the OS process command line.
    sys.argv[1:1] = ['--override', f'remote_control_password={password} {RC_ACTIONS}']
    main()


def terminal_fixture() -> None:
    root = Path(sys.argv[1])
    try:
        run_terminal_fixture(root)
    except Exception as err:
        write_json(root / 'fixture.json', {'error': type(err).__name__})
        raise


def run_terminal_fixture(root: Path) -> None:
    from kitty.constants import kitten_exe

    seq = -1
    evidence: dict[str, Any] = {}
    # All output in this fixture belongs to this run, never to a user shell.
    while True:
        try:
            request = json.loads((root / 'request.json').read_text(encoding='utf-8'))
        except FileNotFoundError:
            request = {'seq': 0, 'action': 'initial'}
        if request['seq'] != seq:
            seq = request['seq']
            action = request['action']
            data = b''
            if action in ('initial', 'screen'):
                data = b'\x1bc\x1b[2J\x1b[H\x1b[31mHC_RED\x1b[0m\r\nHC_BAD\r\x1b[2KHC_SCREEN_OK'
            elif action == 'wrap':
                data = b'\x1b[2J\x1b[H' + b'X' * console_size()[0] + b'WRAP_OK'
            elif action == 'scroll':
                data = b'\x1b[2J\x1b[HSCROLLBACK_FIRST\r\n' + b'line\r\n' * 100 + b'SCROLLBACK_LAST'
            elif action == 'alternate':
                data = b'\x1b[?1049h\x1b[2J\x1b[HALTERNATE_OK'
            elif action == 'restore':
                data = b'\x1b[?1049l'
            elif action == 'reset':
                data = b'\x1bcRESET_OK'
            elif action == 'graphics':
                from kitty.fast_data_types import png_from_32bit_rgba_data

                data = b'\x1b[2J\x1b[H'
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                data = b''
                image = root / 'fixture.png'
                # Saturated magenta/green, neither is in the text fixture or background.
                image.write_bytes(png_from_32bit_rgba_data((b'\xff\x00\xff\xff' * 32 + b'\x00\xff\x00\xff' * 32) * 64, 64, 64))
                cp = subprocess.run(
                    [kitten_exe(), 'icat', '--stdin=no', '--transfer-mode=stream', '--scale-up', '--place=12x6@0x2', str(image)],
                    timeout=10,
                    stderr=subprocess.DEVNULL,
                )
                evidence['icat_exit_code'] = cp.returncode
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
        size = console_size()
        write_json(
            root / 'fixture.json',
            {
                'seq': seq,
                'size': list(size),
                'pid': os.getpid(),
                'endpoint': os.environ.get('KITTY_LISTEN_ON', ''),
                'window_id': os.environ.get('KITTY_WINDOW_ID', ''),
                'public_key': os.environ.get('KITTY_PUBLIC_KEY', ''),
                'credential_in_environment': RC_PASSWORD_ENV in os.environ,
                **evidence,
            },
        )
        time.sleep(0.05)


def console_size() -> tuple[int, int]:
    import ctypes as c
    from ctypes import wintypes as w

    class Info(c.Structure):
        _fields_ = [('size', w._COORD), ('cursor', w._COORD), ('attributes', w.WORD), ('window', w.SMALL_RECT), ('max_size', w._COORD)]

    api = getattr(c, 'WinDLL')('kernel32', use_last_error=True)
    api.CreateFileW.restype = w.HANDLE
    api.CreateFileW.argtypes = (w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE)
    api.GetConsoleScreenBufferInfo.argtypes = (w.HANDLE, c.POINTER(Info))
    api.CloseHandle.argtypes = (w.HANDLE,)
    handle = api.CreateFileW('CONOUT$', 0x80000000 | 0x40000000, 3, None, 3, 0, None)
    if handle == w.HANDLE(-1).value:
        raise OSError('Cannot query owned console')
    try:
        info = Info()
        if not api.GetConsoleScreenBufferInfo(handle, c.byref(info)):
            raise OSError('Cannot query owned console size')
        return info.window.Right - info.window.Left + 1, info.window.Bottom - info.window.Top + 1
    finally:
        api.CloseHandle(handle)


def available_shells() -> list[str]:
    system = Path(os.environ['SystemRoot']) / 'System32'
    shells = [str(system / 'cmd.exe').replace('/', '\\')]
    for path in (system / 'WindowsPowerShell/v1.0/powershell.exe', Path(shutil.which('pwsh.exe') or '/nonexistent-pwsh')):
        if path.is_file():
            shells.append(str(path).replace('/', '\\'))
    return shells


def ps_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def ps_args(shell: str, code: str) -> list[str]:
    return [shell, '-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', base64.b64encode(code.encode('utf-16le')).decode('ascii')]


def console_worker() -> None:
    from kitty.constants import kitty_exe

    shim = str(Path(kitty_exe()).with_suffix('.com')).replace('/', '\\')
    code = (
        'import sys,time; data=sys.stdin.buffer.read(); time.sleep(0.2); '
        "sys.stdout.buffer.write(data+b'|'+sys.argv[1].encode()); sys.stderr.buffer.write(b'HC_STDERR'); sys.exit(17)"
    )
    start = time.monotonic()
    cp = command([shim, '+runpy', code, 'space argument'], input=b'HC_STDIN')
    require(
        cp.returncode == 17 and cp.stdout == b'HC_STDIN|space argument' and cp.stderr == b'HC_STDERR',
        'Console shim did not forward stdio/arguments/exit status',
    )
    require(time.monotonic() - start >= 0.2, 'Console shim did not wait')
    evidence: dict[str, Any] = {'stdio': True, 'exit_code': 17, 'waited': True, 'shells': []}
    shell = available_shells()[0]
    out, err = Path('redirected output.txt').resolve(), Path('redirected error.txt').resolve()
    line = f'""{shim}" +runpy "{code}" "space argument" >"{out}" 2>"{err}""'
    cp = subprocess.run(f'"{shell}" /d /s /c {line}', input=b'HC_STDIN', capture_output=True, timeout=10)
    require(
        cp.returncode == 17 and out.read_bytes() == b'HC_STDIN|space argument' and err.read_bytes() == b'HC_STDERR',
        'cmd redirection/wait/exit status failed',
    )
    evidence['shells'].append('cmd.exe')
    print(json.dumps(evidence))


class NativeSession:
    def __init__(self, exe: str, root: Path) -> None:
        self.exe, self.root = exe, root
        self.kitten = str(Path(exe).parent / 'kitten.exe')
        self.password = 'health_' + secrets.token_urlsafe(32)
        self.worker: Worker | None = None
        self.endpoint = ''
        self.window_id = ''
        self.seq = 0
        self.env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('KITTY_', 'XDG_', 'PYTHON'))}
        self.env.update(KITTY_CONFIG_DIRECTORY=str(root), KITTY_CACHE_DIRECTORY=str(root / 'cache'), KITTY_RUNTIME_DIRECTORY=str(root / 'run'))
        from kitty.constants import kitty_run_data

        if not kitty_run_data.get('from_source'):
            self.env['SLANGC'] = str(Path(exe).parent / 'slangc.exe')

    def close(self) -> None:
        if self.worker is not None:
            worker, self.worker = self.worker, None
            worker.close()

    def state(self, deadline: Deadline, seq: int | None = None) -> dict[str, Any]:
        while True:
            deadline.remaining()
            if self.worker and self.worker.process and self.worker.process.poll() is not None:
                raise AssertionError('Owned Kitty process exited unexpectedly')
            try:
                data = json.loads((self.root / 'fixture.json').read_text(encoding='utf-8'))
                require('error' not in data, f'Terminal fixture failed ({data.get("error")})')
                if seq is None or data['seq'] == seq:
                    return data
            except (FileNotFoundError, PermissionError):
                pass
            time.sleep(0.04)

    def action(self, action: str, deadline: Deadline) -> dict[str, Any]:
        self.seq += 1
        write_json(self.root / 'request.json', {'seq': self.seq, 'action': action})
        return self.state(deadline, self.seq)

    def rc(self, args: list[str], deadline: Deadline, *, authenticated: bool = True) -> subprocess.CompletedProcess[bytes]:
        require(self.endpoint.startswith('tcp:127.0.0.1:'), 'Refusing non-private remote control endpoint')
        auth = ['--use-password=always', '--password-file', '-'] if authenticated else ['--use-password=never']
        cp = command(
            [self.kitten, '@', '--to', self.endpoint, *auth, *args],
            timeout=deadline.remaining(),
            env=self.env,
            cwd=self.root,
            input=self.password.encode('utf-8') if authenticated else b'',
        )
        if authenticated:
            require(cp.returncode == 0, f'Owned-session remote command failed: {args[0]} (raw response withheld)')
        return cp

    def windows(self, deadline: Deadline) -> list[dict[str, Any]]:
        return json.loads(self.rc(['ls'], deadline).stdout)

    def text(self, deadline: Deadline, marker: str, *, extent: str = 'screen', ansi: bool = False, wraps: bool = False) -> str:
        while True:
            args = ['get-text', '--match', f'id:{self.window_id}', '--extent', extent]
            if ansi:
                args.append('--ansi')
            if wraps:
                args.append('--add-wrap-markers')
            text = self.rc(args, deadline).stdout.decode('utf-8', 'replace')
            if marker in text:
                return text
            time.sleep(0.04)

    def launch(self, args: list[str], deadline: Deadline, *, tab: bool = False, hold: bool = False) -> str:
        opts = ['--type=tab' if tab else '--type=window']
        if hold:
            opts.append('--hold')
        return self.rc(['launch', *opts, '--cwd', str(self.root), '--', *args], deadline).stdout.decode('ascii').strip()

    def probe(self, probe_id: str, deadline: Deadline) -> tuple[str, str, dict[str, Any]]:
        evidence: dict[str, Any] = {}
        if probe_id == 'native.console':
            worker = Worker(self.exe, 'from kitty.health_check_native import console_worker; console_worker()', [], self.root, self.env)
            try:
                assert worker.process is not None
                data, _ = worker.process.communicate(timeout=deadline.remaining())
                require(worker.process.returncode == 0, 'Console shim check failed; shell/stdio/exit status mismatch')
                evidence = json.loads(data)
            finally:
                worker.close()
        elif probe_id == 'native.startup':
            config = self.root / 'health.conf'
            config.write_text(
                'allow_remote_control password\n'
                'shell_integration disabled\nupdate_check_interval 0\nconfirm_os_window_close 0\n'
                'remember_window_size no\ninitial_window_width 80c\ninitial_window_height 24c\n'
                'background #000000\nforeground #ffffff\nwindow_padding_width 0\nwindow_border_width 0\n'
                'enabled_layouts tall,stack\n',
                encoding='utf-8',
            )
            args = [
                '--config',
                str(config),
                '--listen-on=tcp:127.0.0.1:0',
                '--title=Kitty health check (disposable)',
                str(Path(self.exe).with_suffix('.com')),
                '+runpy',
                'from kitty.health_check_native import terminal_fixture; terminal_fixture()',
                str(self.root),
            ]
            env = {**self.env, RC_PASSWORD_ENV: self.password}
            self.worker = Worker(self.exe, 'from kitty.health_check_native import gui_worker; gui_worker()', args, self.root, env)
            state = self.state(deadline)
            require(not state['credential_in_environment'], 'Private credential reached the terminal child environment')
            self.endpoint, self.window_id = state['endpoint'], state['window_id']
            self.env['KITTY_PUBLIC_KEY'] = state['public_key']
            require(self.endpoint.startswith('tcp:127.0.0.1:') and self.window_id.isdecimal(), 'Owned-session handshake invalid')
            evidence = {'child_started': True, 'size': state['size'], 'private_loopback': True}
        elif probe_id == 'native.authentication':
            cp = self.rc(['ls'], deadline, authenticated=False)
            require(cp.returncode != 0 and b'remote control is disabled' in cp.stderr.lower(), 'Unauthenticated request was not explicitly rejected')
            listing = self.windows(deadline)
            require(len(listing) == 1 and len(listing[0]['tabs']) == 1, 'Remote response is not the isolated session')
            evidence = {'unauthenticated_rejected': True, 'authenticated_listing': True}
        elif probe_id == 'native.terminal':
            self.action('screen', deadline)
            text = self.text(deadline, 'HC_SCREEN_OK', ansi=True)
            require('HC_BAD' not in text and '\x1b[31m' in text, 'SGR/cursor/erase output differs')
            self.action('wrap', deadline)
            self.text(deadline, '\rWRAP_OK', wraps=True)
            self.action('scroll', deadline)
            self.text(deadline, 'SCROLLBACK_LAST')
            require('SCROLLBACK_FIRST' in self.text(deadline, 'SCROLLBACK_LAST', extent='all'), 'Scrollback not retained')
            self.action('alternate', deadline)
            require('SCROLLBACK_LAST' not in self.text(deadline, 'ALTERNATE_OK'), 'Alternate screen did not isolate content')
            self.action('restore', deadline)
            self.text(deadline, 'SCROLLBACK_LAST')
            self.action('reset', deadline)
            require('SCROLLBACK_LAST' not in self.text(deadline, 'RESET_OK'), 'Reset did not clear the screen')
            evidence = {'sgr': True, 'cursor_erase': True, 'wrap': True, 'scrollback': True, 'alternate_restore': True, 'reset': True}
        elif probe_id == 'native.shells':
            evidence['shells'] = []
            shim = str(Path(self.exe).with_suffix('.com')).replace('/', '\\')
            for shell in available_shells():
                name = Path(shell).name.lower()
                # Exercise a child executable with a spaced path and arguments through each real shell/ConPTY.
                script = self.root / 'shell fixture.py'
                script.write_text(
                    "import sys\nsys.stdout.buffer.write(('SHELL_OK|'+sys.argv[1]+'|\\u03bb\\n').encode('utf-8'))\nsys.stdout.buffer.flush()\nsys.exit(17)\n",
                    encoding='utf-8',
                )
                if name == 'cmd.exe':
                    batch = self.root / 'shell-check.cmd'
                    batch.write_text(
                        f'@echo off\r\nchcp 65001 >nul\r\n"{shim}" +launch "{script}" "space argument"\r\necho EXIT=%errorlevel%\r\n',
                        encoding='utf-8',
                    )
                    args = [shell, '/d', '/k', str(batch).replace('/', '\\')]
                else:
                    line = (
                        '[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new(); '
                        f'& {ps_quote(shim)} +launch {ps_quote(str(script))} {ps_quote("space argument")}; '
                        'Write-Output "EXIT=$LASTEXITCODE"; Start-Sleep -Seconds 120'
                    )
                    args = ps_args(shell, line)
                for _ in range(2):
                    wid = self.launch(args, deadline)
                    try:
                        while True:
                            data = self.rc(['get-text', '--match', f'id:{wid}'], deadline).stdout.decode('utf-8', 'replace')
                            if 'EXIT=' in data:
                                break
                            time.sleep(0.04)
                        require('EXIT=17' in data and 'SHELL_OK|space argument|λ' in data, 'Shell Unicode/quoting/child exit-status round trip failed')
                    finally:
                        cleanup = Deadline(3)
                        if any(w['id'] == int(wid) for osw in self.windows(cleanup) for t in osw['tabs'] for w in t['windows']):
                            self.rc(['close-window', '--match', f'id:{wid}'], cleanup)
                evidence['shells'].append(name)
            evidence['launches_per_shell'] = 2
            evidence['powershell_7_available'] = any(name == 'pwsh.exe' for name in evidence['shells'])
        elif probe_id == 'native.layout':
            old_size = self.state(deadline)['size']
            idle = [self.exe, '+runpy', 'import time; time.sleep(120)']
            pane = self.launch(idle, deadline)
            try:
                listing = self.windows(deadline)
                require(len(listing[0]['tabs'][0]['windows']) == 2, 'Pane creation failed')
                self.rc(['focus-window', '--match', f'id:{pane}'], deadline)
                require(any(w['id'] == int(pane) and w['is_focused'] for w in self.windows(deadline)[0]['tabs'][0]['windows']), 'Pane focus failed')
            finally:
                self.rc(['close-window', '--match', f'id:{pane}'], deadline)
            tab_window = self.launch(idle, deadline, tab=True)
            try:
                require(len(self.windows(deadline)[0]['tabs']) == 2, 'Tab creation failed')
            finally:
                self.rc(['close-window', '--match', f'id:{tab_window}'], deadline)
            self.rc(['focus-window', '--match', f'id:{self.window_id}'], deadline)
            self.rc(['resize-os-window', '--match', f'id:{self.window_id}', '--unit=cells', '--width=94', '--height=29'], deadline)
            while True:
                size = self.state(deadline)['size']
                if size != old_size and size[0] == 94:
                    break
                time.sleep(0.04)
            require(len(self.windows(deadline)[0]['tabs']) == 1, 'Tab close did not propagate')
            evidence = {'pane_create_focus_close': True, 'tab_create_close': True, 'child_size_before': old_size, 'child_size_after': size, 'child_alive': True}
        elif probe_id == 'native.graphics':
            state = self.action('graphics', deadline)
            require(state.get('icat_exit_code') == 0, 'Graphics client failed')
            from kitty.fast_data_types import load_png_data

            while True:
                path = self.root / 'screenshot.png'
                self.rc(['screenshot', '--match', f'id:{self.window_id}', str(path)], deadline)
                pixels, width, height = load_png_data(path.read_bytes())
                if colored_fixture_present(pixels, width, height):
                    break
                time.sleep(0.05)
            evidence = {'streamed_graphics': True, 'framebuffer_pixel_regions': True, 'screenshot_size': [width, height], 'physical_display_tested': False}
        elif probe_id == 'native.renderer':
            script = self.root / 'renderer.py'
            script.write_text(
                'import json\nfrom kittens.tui.handler import result_handler\n'
                'def main(args): pass\n@result_handler(no_ui=True)\n'
                'def handle_result(args, data, target_window_id, boss):\n'
                ' from kitty.fast_data_types import gpu_driver_version_string, gpu_renderer_string, win32_conpty_provider\n'
                ' return json.dumps(dict(driver=gpu_driver_version_string(), renderer=gpu_renderer_string(), conpty=win32_conpty_provider()))\n',
                encoding='utf-8',
            )
            evidence = json.loads(self.rc(['kitten', str(script)], deadline).stdout)
            software = any(word in evidence['renderer'].lower() for word in ('llvmpipe', 'softpipe', 'software', 'swrast', 'gdi generic', 'swiftshader'))
            evidence['software_rendering'] = software
            if software:
                return 'WARN', 'Functional framebuffer checks use software rendering; hardware GPU/display remains untested.', evidence
        elif probe_id == 'native.cleanup':
            ran = self.worker is not None
            self.close()
            evidence = {'owned_job_empty': True, 'session_started': ran}
        else:
            raise ValueError('Unknown native probe')
        return 'PASS', 'Check completed.', evidence


def colored_fixture_present(pixels: bytes, width: int, height: int) -> bool:
    if len(pixels) != width * height * 4:
        return False
    # Sample coarse blocks, not font/GPU-dependent screenshot hashes.
    counts = [0, 0]
    for y in range(0, height, 3):
        for x in range(0, min(width, width // 3), 3):
            r, g, b = pixels[(y * width + x) * 4 :][:3]
            counts[0] += r > 210 and b > 210 and g < 45
            counts[1] += g > 210 and r < 45 and b < 45
    return min(counts) >= 20
