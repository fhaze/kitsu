#!/usr/bin/env python
"""Exercise Windows GUI stderr fallback and detached logs using a built package."""

import ctypes
import os
import subprocess
import sys
import tempfile
import time
import unittest
from ctypes import wintypes
from pathlib import Path
from typing import BinaryIO

PACKAGE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('windows-package/kitsu').resolve()


@unittest.skipUnless(sys.platform == 'win32', 'Windows GUI launcher')
class TestLogging(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix='Kitsu logs ☃ ')
        self.addCleanup(self.cleanup_logs)
        self.root = Path(self.tmp.name)
        self.log = self.root / 'Kitsu/logs/kitsu.log'
        self.env = dict(os.environ, LOCALAPPDATA=str(self.root))
        for key in ('KITTY_EXEC_FOR_DETACH', 'PYTHONHOME', 'PYTHONPATH'):
            self.env.pop(key, None)

    def cleanup_logs(self) -> None:
        deadline = time.monotonic() + 10
        while True:
            try:
                self.tmp.cleanup()
                return
            except PermissionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.02)

    def launch(self, *args: str, stderr: int | BinaryIO = subprocess.DEVNULL, missing_stdio: bool = False) -> subprocess.Popen[bytes]:
        process = subprocess.Popen(
            [str(PACKAGE / 'bin/kitsu.exe'), *args], env=self.env,
            stdin=None if missing_stdio else subprocess.DEVNULL,
            stdout=None if missing_stdio else subprocess.DEVNULL, stderr=None if missing_stdio else stderr,
            creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP,
        )

        def cleanup() -> None:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)

        self.addCleanup(cleanup)
        return process

    def finish(self, process: subprocess.Popen[bytes], *, dialog: bool = False) -> str:
        user = ctypes.WinDLL('user32', use_last_error=True)
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user.GetWindowTextW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
        user.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
        user.EnumWindows.argtypes = (callback_type, wintypes.LPARAM)
        user.EnumChildWindows.argtypes = (wintypes.HWND, callback_type, wintypes.LPARAM)
        user.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
        messages: list[str] = []

        @callback_type
        def child(hwnd: int, _: int) -> bool:
            text = ctypes.create_unicode_buffer(8192)
            user.GetWindowTextW(hwnd, text, len(text))
            messages.append(text.value)
            return True

        @callback_type
        def window(hwnd: int, _: int) -> bool:
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == process.pid:
                title = ctypes.create_unicode_buffer(128)
                user.GetWindowTextW(hwnd, title, len(title))
                if title.value == 'Kitsu error':
                    messages.clear()
                    user.EnumChildWindows(hwnd, child, 0)
                    if any('Kitsu encountered' in text for text in messages):
                        user.PostMessageW(hwnd, 0x10, 0, 0)  # WM_CLOSE
            return True

        deadline = time.monotonic() + 30
        while process.poll() is None and time.monotonic() < deadline:
            user.EnumWindows(window, 0)
            time.sleep(0.02)
        process.wait(timeout=1)
        self.assertEqual(bool(messages), dialog, messages)
        return '\n'.join(messages)

    def test_gui_errors_and_raw_stderr_are_persisted(self) -> None:
        code = 'import sys; from kitty.fast_data_types import log_error_string; log_error_string("log marker ☃"); print("raw marker", file=sys.stderr)'
        process = self.launch('+runpy', code)
        self.finish(process)
        self.assertEqual(process.returncode, 0)
        text = self.log.read_text(encoding='utf-8')
        self.assertIn('log marker ☃', text)
        self.assertIn('raw marker', text)

    def test_missing_stdio_preserves_raw_stderr(self) -> None:
        # A re-executed detached child has no handles and must not attach to the
        # test runner's console (which can exist on CI).
        self.env['KITTY_EXEC_FOR_DETACH'] = 'detached'
        process = self.launch('+runpy', 'import os; os.write(2, b"raw fd marker")', missing_stdio=True)
        self.finish(process)
        self.assertEqual(process.returncode, 0)
        self.assertIn('raw fd marker', self.log.read_text(encoding='utf-8'))

    def test_startup_exception_is_logged_and_reported(self) -> None:
        code = (
            'from kitty import main; '
            'main.kitty_main = lambda *a: (_ for _ in ()).throw(RuntimeError("startup marker")); main.main()'
        )
        process = self.launch('+runpy', code)
        text = self.finish(process, dialog=True)
        self.assertEqual(process.returncode, 1)
        self.assertIn(str(self.log).replace('/', '\\'), text.replace('/', '\\'))
        self.assertIn('RuntimeError: startup marker', self.log.read_text(encoding='utf-8'))

    def test_uncaught_python_error_is_logged_and_reported(self) -> None:
        process = self.launch('+runpy', 'raise RuntimeError("uncaught marker")')
        self.finish(process, dialog=True)
        self.assertEqual(process.returncode, 1)
        self.assertIn('RuntimeError: uncaught marker', self.log.read_text(encoding='utf-8'))

    def test_unwritable_log_still_reports_failure(self) -> None:
        (self.root / 'Kitsu').write_text('not a directory', encoding='utf-8')
        process = self.launch('+runpy', 'raise RuntimeError("unwritable marker")')
        text = self.finish(process, dialog=True)
        self.assertEqual(process.returncode, 1)
        self.assertIn('could not write', text)

    def test_redirected_stderr_is_preserved_without_dialog(self) -> None:
        path = self.root / 'redirected.log'
        with path.open('wb') as dest:
            process = self.launch('+runpy', 'raise RuntimeError("redirected marker")', stderr=dest)
            self.finish(process)
        self.assertEqual(process.returncode, 1)
        self.assertIn('RuntimeError: redirected marker', path.read_text(encoding='utf-8'))
        self.assertFalse(self.log.exists())
        process = self.launch('+runpy', 'raise RuntimeError("pipe marker")', stderr=subprocess.PIPE)
        _, stderr = process.communicate(timeout=30)
        self.assertEqual(process.returncode, 1)
        self.assertIn(b'RuntimeError: pipe marker', stderr)
        self.assertFalse(self.log.exists())

    def test_log_rotation_keeps_previous_output(self) -> None:
        self.log.parent.mkdir(parents=True)
        previous = b'x' * (1024 * 1024)
        self.log.write_bytes(previous)
        process = self.launch('+runpy', 'import sys; print("new marker", file=sys.stderr)')
        self.finish(process)
        self.assertEqual(process.returncode, 0)
        self.assertEqual(self.log.with_suffix('.log.1').read_bytes(), previous)
        self.assertEqual(self.log.read_text(encoding='utf-8'), 'new marker\n')

    def test_detached_log_captures_both_streams_and_appends(self) -> None:
        path = self.root / 'detached ☃.log'
        path.write_text('previous marker\n', encoding='utf-8')
        done = self.root / 'done'
        generator = self.root / 'startup.py'
        config = self.root / 'kitty.conf'
        # A config generator runs in-process before GLFW. Exit cleanly there so
        # this checks the real detached launcher without needing an OpenGL GPU.
        code = (
            'import os; from pathlib import Path; from kitty.fast_data_types import log_error_string; '
            'os.write(1, b"stdout marker\\n"); os.write(2, b"stderr marker\\n"); log_error_string("detached marker"); '
            f'Path({str(done)!r}).touch(); raise SystemExit(0)'
        )
        generator.write_text(code, encoding='utf-8')
        config.write_text('geninclude startup.py\n', encoding='utf-8')
        process = self.launch('--detach', '--detached-log', str(path), '--config', str(config))
        self.finish(process)
        self.assertEqual(process.returncode, 0)
        deadline = time.monotonic() + 30
        while not done.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(done.exists(), 'Detached child did not finish')
        text = path.read_text(encoding='utf-8')
        for marker in ('previous marker', 'stdout marker', 'stderr marker', 'detached marker'):
            self.assertIn(marker, text)

    def test_health_check_reports_log_path(self) -> None:
        code = (
            'from pathlib import Path; from kitty.health_check import quick_probe; '
            'status, _, evidence = quick_probe("installation.logging"); '
            f'\nif status != "PASS" or Path(evidence["gui_log"]) != Path({str(self.log)!r}): raise RuntimeError(str(evidence))'
        )
        process = self.launch('+runpy', code)
        self.finish(process)
        self.assertEqual(process.returncode, 0, self.log.read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
