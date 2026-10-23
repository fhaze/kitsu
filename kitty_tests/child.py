#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

import errno
import os
import subprocess
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from kitty.child import Child, add_kitty_vars_to_wslenv, memory_used_by_process_tree_rooted_at
from kitty.constants import is_macos, is_windows, kitten_exe, kitty_exe
from kitty.window import Window

from .base import BaseTest


def batch_file_test_args(inject: str) -> list[str]:
    # Arguments that must reach a batch file unchanged, without running inject
    args = [r'C:\tmp\a.txt&' + inject, f'x|{inject}', f'x<a.json&{inject}', f'x>y&{inject}', f'x^&{inject}', f'(x)&{inject}', f'"&{inject}&"']
    args += [f'a"&{inject}&"b', f'\\"&{inject}', f'^"&{inject}', f'%"&{inject}&"%', f'a\\"&{inject}', '%PATH%', '100%', '%cd%', '%%', '%=C:%']
    args += ['%~dp0', '%1', '%*', '%%cd:~,%', '!PATH!', '!', '', ' ', 'a b', '\t', 'C:\\dir\\', '\\\\', '"', '""', 'x"y"z', 'a\\\\"b', ';', ',']
    args += ['=', '[]{}', "`~'", 'y' * 5000]  # the last one is longer than the initial command line buffer
    return args


def write_batch_file_recorder(path: str) -> None:
    # A batch file that records its arguments in ..\args.json via kitty.exe,
    # which splits its command line with the C runtime rules the escaping for
    # batch files relies on
    with open(path, 'w', encoding='utf-8') as f:
        code = "import sys, json; open(sys.argv[1], 'w', encoding='utf-8').write(json.dumps(sys.argv[2:]))"
        f.write(f'@"{kitty_exe()}" +runpy "{code}" "%~dp0..\\args.json" %*\r\n')


class ChildMemoryTest(BaseTest):
    def test_windows_shell_detection(self):
        with patch('kitty.child.is_windows', True):
            for executable in ('cmd.exe', r'C:\Program Files\PowerShell\7\pwsh.exe', 'powershell.exe'):
                with self.subTest(executable=executable):
                    self.assertTrue(Child([executable], '').is_shell)
        self.assertFalse(Child(['python.exe'], '').is_shell)

    def test_windows_cwd_is_kitty_exe_dir(self):
        # When launched from a shortcut/Start Menu the process working
        # directory is the kitty.exe directory, which must be detected so the
        # home directory is used as the default working directory instead
        from kitty.utils import cwd_is_kitty_exe_dir

        with patch('kitty.utils.kitty_exe', return_value=r'C:\Program Files\kitty\bin\kitty.exe'):
            self.assertTrue(cwd_is_kitty_exe_dir(r'C:\Program Files\kitty\bin'))
            self.assertTrue(cwd_is_kitty_exe_dir(r'c:\program files\kitty\bin'))
            self.assertTrue(cwd_is_kitty_exe_dir(r'C:\Program Files\kitty\bin\\'))
            self.assertFalse(cwd_is_kitty_exe_dir(r'C:\Program Files\kitty'))
            self.assertFalse(cwd_is_kitty_exe_dir(r'C:\Users\someone'))
        with patch('kitty.utils.kitty_exe', side_effect=RuntimeError('not found')):
            self.assertFalse(cwd_is_kitty_exe_dir(os.getcwd()))

    def test_windows_default_shell_running_program_detection(self):
        child = SimpleNamespace(is_shell=True, pid=12, pid_for_cwd=12)
        window = SimpleNamespace(at_prompt=False, child=child)
        with patch('kitty.window.is_windows', True):
            self.assertFalse(Window.has_running_program.fget(window))
            child.pid_for_cwd = 13
            self.assertTrue(Window.has_running_program.fget(window))
            child.is_shell = False
            child.pid_for_cwd = child.pid
            self.assertTrue(Window.has_running_program.fget(window))

    @unittest.skipUnless(is_windows, 'Batch files are Windows only')
    def test_windows_batch_file_arguments(self):
        # cmd.exe parses the command line of batch files itself, arguments must
        # reach the script unchanged instead of running commands via its
        # metacharacters (CVE-2024-24576, aka BatBadBut). EDITOR=code for
        # instance resolves to code.cmd, which the default open action passes
        # file names from hyperlinks to.
        import ctypes
        import json
        import tempfile
        import threading
        from contextlib import suppress

        from kitty.fast_data_types import openpty, spawn

        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.OpenProcess.restype = ctypes.c_void_p
        k32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        k32.CloseHandle.argtypes = (ctypes.c_void_p,)
        inject = 'echo.>pwned'
        args = batch_file_test_args(inject)
        with tempfile.TemporaryDirectory() as tdir:
            # cmd.exe must be able to run a script whose path has metacharacters as well
            sdir = os.path.join(tdir, 'a b&c%PATH%d^')
            os.mkdir(sdir)
            script, pwned = os.path.join(sdir, 'record args.cmd'), os.path.join(tdir, 'pwned')
            write_batch_file_recorder(script)

            def drain(fd: int) -> None:
                # cmd.exe blocks when nothing reads its output, which includes its window title
                with suppress(OSError):
                    while os.read(fd, 65536):
                        pass

            def environ() -> tuple[str, ...]:
                # not stored in a local, as those are printed when a test fails
                return tuple(f'{k}={v}' for k, v in os.environ.items())

            def run(exe: str, *args: str) -> None:
                # The pseudoconsole is closed when its child exits, so every
                # run needs its own. The master is left open, os.close() would
                # leave the pty registered under its fd number, kitty closes it
                # from C.
                master, slave = openpty()
                threading.Thread(target=drain, args=(master,), daemon=True).start()
                try:
                    pid = spawn(exe, tdir, (exe,) + args, environ(), master, slave, -1, -1, -1, -1, (), kitten_exe(), False, ())
                finally:
                    os.close(slave)
                h = k32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
                self.assertTrue(h, ctypes.get_last_error())
                try:
                    self.ae(k32.WaitForSingleObject(h, 60000), 0)
                finally:
                    k32.CloseHandle(h)

            for exe in (script, script.upper(), script + '. .'):
                with self.subTest(exe=exe):
                    run(exe, *args)
                    with open(os.path.join(tdir, 'args.json'), encoding='utf-8') as f:
                        self.ae(json.load(f), args)
                    os.remove(os.path.join(tdir, 'args.json'))
                    self.assertFalse(os.path.exists(pwned), 'An argument was run as a command')
            # kitty.exe cannot receive non-ASCII arguments, so only check that these do not run anything
            run(script, f'ünïcødé&{inject}', f'漢字|{inject}', f'x\x85&{inject}')
            self.assertFalse(os.path.exists(pwned), 'An argument was run as a command')
            # CR and LF end the command, there is no way to escape them
            for arg in ('a\r\ndir', 'a\ndir', 'a\rdir'):
                with self.assertRaises(OSError) as cm:
                    run(script, arg)
                self.ae(cm.exception.errno, errno.EINVAL)

    @unittest.skipUnless(is_windows, 'Batch files are Windows only')
    def test_windows_batch_file_arguments_outside_windows(self):
        # Programs kitty starts outside its windows must pass arguments to
        # batch files safely too: via subprocess for open_url_with, hints
        # --program, launch --type=background and command_on_bell, and via Go
        # for launch --hold (kitten run-shell), choose-files previewers and
        # other kittens running programs the user chose
        import json
        import tempfile

        from kitty.constants import helper_process_popen_kwargs
        from kitty.fast_data_types import win32_batch_command_line
        from kitty.utils import batch_safe_popen_args, is_windows_batch_file, open_url

        for path, expected in {
            'C:\\x\\a.bat': True, 'C:/x/a.CMD': True, 'C:\\x\\a.cmd. .': True, '\\\\?\\C:\\x\\a.cmd': True,
            '\\\\?\\C:\\x\\a.cmd.': False, 'C:\\x\\a.exe': False, 'C:\\x\\acmd': False, 'C:\\x.cmd\\a': False,
        }.items():
            self.ae(is_windows_batch_file(path), expected, path)
        with self.assertRaises(ValueError):
            win32_batch_command_line('C:\\x\\r.cmd', ())

        # The escaping of kitty_win32_append_batch_arg(). The Go port in
        # tools/utils/command_windows.go is tested against the same string.
        argv = ('r', 'a b', '', '100%', 'a"b', 'C:\\dir\\', 'x\\"y', 'a&b', 'safe-arg_1')
        q = win32_batch_command_line('C:\\x\\r.cmd', argv)
        assert q is not None
        self.assertTrue(q[0].lower().endswith('\\cmd.exe'), q[0])
        self.ae(q[1], 'cmd.exe /d /e:ON /v:OFF /s /c " C:\\x\\r.cmd "a b" "" "100%%cd:~,%" "a""b" "C:\\dir\\\\" "x\\\\""y" "a&b" safe-arg_1"')
        self.assertIsNone(win32_batch_command_line('C:\\x\\r.exe', argv))
        with self.assertRaises(OSError) as cm:
            win32_batch_command_line('C:\\x\\r.cmd', ('r', 'a\nb'))
        self.ae(cm.exception.errno, errno.EINVAL)

        inject = 'echo.>pwned'
        args = batch_file_test_args(inject)
        with tempfile.TemporaryDirectory() as tdir:
            sdir = os.path.join(tdir, 'a b&c%PATH%d^')
            os.mkdir(sdir)
            script, pwned, recorded = os.path.join(sdir, 'record args.cmd'), os.path.join(tdir, 'pwned'), os.path.join(tdir, 'args.json')
            write_batch_file_recorder(script)
            write_batch_file_recorder(os.path.join(sdir, 'recorder.cmd'))

            def check(how: str) -> None:
                with self.subTest(how=how):
                    with open(recorded, encoding='utf-8') as f:
                        self.ae(json.load(f), args)
                    os.remove(recorded)
                    self.assertFalse(os.path.exists(pwned), 'An argument was run as a command')

            def popen(cmd: list[str]) -> None:
                a, exe = batch_safe_popen_args(cmd)
                self.assertTrue(exe, 'Not run via cmd.exe')
                subprocess.run(a, executable=exe, cwd=tdir, timeout=60, **helper_process_popen_kwargs())

            popen([script] + args)
            check('subprocess with a path')
            # A bare name resolves to the batch file via PATHEXT, which
            # CreateProcess() alone would not do
            path = os.environ['PATH']
            os.environ['PATH'] = sdir + os.pathsep + path
            try:
                popen(['recorder'] + args)
            finally:
                os.environ['PATH'] = path
            check('subprocess with a bare name')
            p = open_url(args, program=[script], cwd=tdir)
            assert p is not None
            p.wait(60)
            check('open_url')
            subprocess.run(
                [kitten_exe(), 'run-shell', '--shell=cmd.exe /d /c exit', '--', script] + args,
                cwd=tdir, stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
            check('kitten run-shell')
            with self.assertRaises(OSError) as cm:
                batch_safe_popen_args([script, 'a\r\ndir'])
            self.ae(cm.exception.errno, errno.EINVAL)
            # A missing batch file is left to fail to start, and programs that
            # are not batch files are passed through unchanged, whatever their
            # arguments
            missing = [os.path.join(sdir, 'missing.cmd'), 'x']
            self.ae(batch_safe_popen_args(missing), (missing, None))
            other = [os.path.join(os.environ.get('SystemRoot', 'C:\\Windows'), 'System32', 'cmd.exe'), 'a&b', '\udc80']
            self.ae(batch_safe_popen_args(other), (other, None))

    @unittest.skipUnless(is_windows, 'ShellExecute() is Windows only')
    def test_windows_default_url_opening(self):
        # The default open_url_with must not let a clicked link run programs
        # or connect to network hosts without confirmation, as ShellExecute()
        # runs executables and scripts, unlike xdg-open
        import tempfile
        import time
        from pathlib import Path

        from kitty.utils import open_url, windows_shell_open, windows_shell_open_risk

        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tdir:
            for url, cwd in (
                ('https://example.com/x.exe', None), ('HTTP://example.com', None), ('mailto:a@example.com', None),
                ('file:///C:/Users/Public/readme.txt', None), ('file://localhost/C:/a%20b/notes.pdf', None),
                ('C:\\x\\notes.pdf', None), ('notes.txt', 'C:\\x'), ('\\\\?\\C:\\x\\a.txt', None), (tdir, None),
                ('file:///' + tdir.replace('\\', '/'), None),
            ):
                self.ae(windows_shell_open_risk(url, cwd), '', url)
            for url, cwd in (
                ('file:///C:/Users/Public/x.bat', None), ('file:///C:/x/y.EXE', None), ('file:///C:/a%20b/x.lnk', None),
                ('C:\\x\\y.hta', None), ('C:\\x\\y.cmd. .', None), ('C:/x/y.vbs', None), ('C:\\x\\a.txt:b.exe', None),
                ('y.js', 'C:\\x'), ('x.ps1', None), ('x.msi', None), ('x.url', None), ('x.reg', None), ('cmd', None),
                ('C:\\x\\README', None), ('file://attacker.example/share/x.txt', None),
                ('file:////attacker.example/share/x.txt', None), ('\\\\attacker.example\\share\\a.pdf', None),
                ('//attacker.example/share/a.pdf', None), ('\\\\attacker.example@SSL\\DavWWWRoot\\a.txt', None),
                ('a.txt', '\\\\attacker.example\\share'), ('\\\\.\\C:\\x.txt', None), ('ms-msdt:/id PCWDiagnostic', None),
                ('search-ms:query=x', None), ('vbscript:x', None),
            ):
                self.assertTrue(windows_shell_open_risk(url, cwd), url)

            marker, script = os.path.join(tdir, 'ran'), os.path.join(tdir, 'x.bat')
            with open(script, 'w') as f:
                f.write(f'@echo ran> "{marker}"\n')
            opened: list[str] = []
            logged: list[str] = []
            choices: list[tuple] = []
            boss = SimpleNamespace(active_window=object(), choose=lambda msg, callback, *a, **kw: choices.append((callback, kw)))
            with patch('kitty.utils.windows_shell_open', lambda url, cwd=None: opened.append(url)), patch('kitty.utils.log_error', logged.append):
                # Without a window to ask in, dangerous links are not opened
                with patch('kitty.utils.get_boss', lambda: None):
                    open_url(['https://example.com', script, 'file:///' + script.replace('\\', '/')])
                self.ae(opened, ['https://example.com'])
                self.ae(len(logged), 2)
                with patch('kitty.utils.get_boss', lambda: boss):
                    open_url(script, cwd=tdir)
                self.ae(len(choices), 1)
                callback, kw = choices[0]
                self.ae(kw['default'], 'n')
                callback('n')
                self.ae(opened, ['https://example.com'])
                callback('o')
                self.ae(opened, ['https://example.com', script])
            self.assertFalse(os.path.exists(marker))
            # ShellExecuteEx() in the helper, including a relative path and
            # Unicode/URL-encoded names crossing the process boundary.
            script = os.path.join(tdir, 'open image ☃ & space.bat')
            with open(script, 'w') as f:
                f.write(f'@echo ran> "{marker}"\n')
            for url in (script, os.path.basename(script), Path(script).as_uri()):
                process = windows_shell_open(url, tdir)
                self.assertIsNotNone(process)
                self.ae(process.wait(timeout=30), 0)
                for i in range(300):
                    if os.path.exists(marker):
                        break
                    time.sleep(0.1)
                self.assertTrue(os.path.exists(marker), f'windows_shell_open() did not open {url!r}')
                time.sleep(0.5)
                os.remove(marker)

    @unittest.skipUnless(is_windows, 'Windows GUI URL opening')
    def test_windows_shell_open_with_gui_running(self):
        # A standalone Python test yields the GIL while polling and can hide
        # opener thread starvation. Exercise the actual C GUI event loop.
        import tempfile
        import time
        from pathlib import Path

        from kitty.constants import helper_process_popen_kwargs

        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tdir:
            marker = os.path.join(tdir, 'opened')
            script = os.path.join(tdir, 'open.bat')
            with open(script, 'w') as f:
                f.write(f'@echo opened> "{marker}"\n')
            watcher = os.path.join(tdir, 'watcher.py')
            with open(watcher, 'w') as f:
                f.write(
                    'from kitty.fast_data_types import add_timer\n'
                    'from kitty.utils import windows_shell_open\n'
                    'def on_load(boss, data):\n'
                    f'    add_timer(lambda timer_id: windows_shell_open({Path(script).as_uri()!r}), 0.2, False)\n'
                )
            with subprocess.Popen(
                [
                    kitty_exe(), '--config', 'NONE', '--start-as', 'minimized', '-o', f'watcher {watcher}',
                    kitty_exe(), '+runpy', 'import time; time.sleep(10)',
                ],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                **helper_process_popen_kwargs(),
            ) as process:
                try:
                    deadline = time.monotonic() + 30
                    while not os.path.exists(marker) and process.poll() is None and time.monotonic() < deadline:
                        time.sleep(0.1)
                    opened = os.path.exists(marker)
                finally:
                    process.terminate()
                _, stderr = process.communicate(timeout=10)
                self.assertTrue(opened, f'URL did not open while the GUI was running: {stderr.decode("utf-8", "replace")}')

    def _spawn_allocating_child(self, alloc_bytes: int) -> subprocess.Popen:
        p = subprocess.Popen(
            [
                kitty_exe(),
                '+runpy',
                f"""\
import sys, time
buf = bytearray({alloc_bytes})
for i in range(0, {alloc_bytes}, 4096):
    buf[i] = 1
sys.stdout.write("ready\\n")
sys.stdout.flush()
time.sleep(300)
""",
            ],
            stdout=subprocess.PIPE,
        )
        line = p.stdout.readline().strip()
        p.stdout.close()
        if line != b'ready':
            p.kill()
            p.wait()
            raise AssertionError(f'Unexpected output from allocating child: {line!r}')
        return p

    def _terminate(self, p: subprocess.Popen) -> None:
        p.terminate()
        p.wait()

    def test_memory_returns_positive_for_live_process(self):
        mem = memory_used_by_process_tree_rooted_at(os.getpid())
        self.assertGreater(mem, 0)

    def test_memory_returns_minus_one_for_nonexistent_pid(self):
        self.ae(memory_used_by_process_tree_rooted_at(99999999), -1)

    def test_memory_accounts_for_child_allocation(self):
        # Verify that a child's known resident allocation shows up in the
        # measurement.  check_if_cgroup_root=True falls back to per-process
        # tree walk when pid is not the cgroup root (the common case when
        # running under a shared session cgroup), so this exercises the tree
        # walk path on Linux and the always-tree-walk path on macOS.
        alloc = 20 * 1024 * 1024  # 20 MiB
        child = self._spawn_allocating_child(alloc)
        try:
            mem = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=True)
            self.assertGreater(
                mem,
                alloc // 2,
                f'Expected at least {alloc // 2} bytes for a {alloc}-byte allocation, got {mem}',
            )
        finally:
            self._terminate(child)

    def test_memory_of_parent_tree_includes_child(self):
        # Measuring the current process tree must yield more than measuring
        # the child alone, because the test runner itself occupies memory.
        alloc = 20 * 1024 * 1024  # 20 MiB
        child = self._spawn_allocating_child(alloc)
        try:
            mem_child = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=True)
            mem_tree = memory_used_by_process_tree_rooted_at(os.getpid(), check_if_cgroup_root=True)
            self.assertGreater(
                mem_tree,
                mem_child,
                'Parent tree memory should exceed child-only memory',
            )
        finally:
            self._terminate(child)

    def test_wslenv(self):
        env = {'TERM': 'xterm-kitty', 'COLORTERM': 'truecolor', 'KITTY_WINDOW_ID': '1', 'KITTY_PID': '2', 'TERMINFO': r'C:\kitty\terminfo'}
        add_kitty_vars_to_wslenv(env)
        self.ae(env['WSLENV'], 'TERM:COLORTERM:KITTY_WINDOW_ID:KITTY_PID:TERMINFO/p')

        env = {'TERM': 'xterm-kitty', 'TERMINFO': 'b64:abcd', 'WSLENV': 'WT_SESSION:TERM/u'}
        add_kitty_vars_to_wslenv(env, 'direct')
        self.ae(env['WSLENV'], 'WT_SESSION:TERM/u:TERMINFO')

        env = {'PWD': '/tmp'}
        add_kitty_vars_to_wslenv(env)
        self.assertNotIn('WSLENV', env)

    def test_terminfo_environment(self):
        from base64 import b64decode

        from kitty.config import load_config
        from kitty.fast_data_types import terminfo_data
        from kitty.options.types import Options, defaults

        default_type = 'direct' if is_windows else 'path'
        self.ae(Options().terminfo_type, default_type)
        self.ae(defaults.terminfo_type, default_type)
        boss = SimpleNamespace(encryption_public_key='', listening_on='', dcs_channel_address='')
        tdir = r'C:\Kitsu\lib\kitty\terminfo'
        for setting in ('', 'direct', 'path', 'none'):
            with self.subTest(setting=setting or 'default'):
                overrides = ['shell_integration disabled']
                if setting:
                    overrides.append(f'terminfo_type {setting}')
                opts = load_config(overrides=overrides)
                mode = setting or default_type
                self.ae(opts.terminfo_type, mode)
                with (
                    patch('kitty.child.default_env', return_value={'TERMINFO': 'inherited'}),
                    patch('kitty.fast_data_types.get_options', return_value=opts),
                    patch('kitty.fast_data_types.get_boss', return_value=boss),
                    patch('kitty.child.checked_terminfo_dir', return_value=tdir),
                ):
                    env, _ = Child(['cmd.exe' if is_windows else '/bin/sh'], os.getcwd()).get_final_env()
                self.ae(env['TERM'], 'xterm-kitty')
                if mode == 'direct':
                    self.assertTrue(env['TERMINFO'].startswith('b64:'))
                    self.ae(b64decode(env['TERMINFO'][4:]), terminfo_data())
                else:
                    self.ae(env['TERMINFO'], tdir if mode == 'path' else 'inherited')
                if is_windows:
                    entries = env['WSLENV'].split(':')
                    self.assertIn('TERMINFO/p' if mode == 'path' else 'TERMINFO', entries)
                    self.assertNotIn('TERMINFO' if mode == 'path' else 'TERMINFO/p', entries)

    def test_memory_cgroup_path_returns_positive(self):
        # The fast cgroup path (check_if_cgroup_root=False, the default) must
        # return a usable value on Linux.
        if is_macos:
            self.skipTest('cgroup not available on macOS')
        mem = memory_used_by_process_tree_rooted_at(os.getpid())
        self.assertGreater(mem, 0)

    def test_memory_cgroup_and_tree_walk_both_positive(self):
        # Both the cgroup path and the tree-walk path should give positive
        # results for a process that is alive.
        if is_macos:
            self.skipTest('cgroup path not applicable on macOS')
        alloc = 10 * 1024 * 1024  # 10 MiB
        child = self._spawn_allocating_child(alloc)
        try:
            mem_cgroup = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=False)
            mem_walk = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=True)
            self.assertGreater(mem_cgroup, 0)
            self.assertGreater(mem_walk, 0)
        finally:
            self._terminate(child)
