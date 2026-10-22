#!/usr/bin/env python
"""Headless Win32 clipboard regression: python windows/test_clipboard.py windows-package/kitsu."""

import ctypes
import subprocess
import sys
import time
import unittest
from collections.abc import Callable, Generator
from contextlib import contextmanager
from ctypes import wintypes
from pathlib import Path
from types import SimpleNamespace


class BinaryClipboard(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        from kitty.constants import glfw_path
        from kitty.main import init_glfw_module

        init_glfw_module('win32')
        cls.glfw = ctypes.CDLL(glfw_path('win32'))
        cls.glfw.glfwGetError.argtypes = [ctypes.POINTER(ctypes.c_char_p)]
        cls.glfw.glfwGetError.restype = ctypes.c_int
        cls.user32 = ctypes.WinDLL('user32', use_last_error=True)
        cls.kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        for dll, signatures in (
            (
                cls.user32,
                {
                    'OpenClipboard': ([wintypes.HWND], wintypes.BOOL),
                    'CloseClipboard': ([], wintypes.BOOL),
                    'EmptyClipboard': ([], wintypes.BOOL),
                    'GetClipboardOwner': ([], wintypes.HWND),
                    'RegisterClipboardFormatW': ([wintypes.LPCWSTR], wintypes.UINT),
                    'GetClipboardData': ([wintypes.UINT], wintypes.HANDLE),
                    'SetClipboardData': ([wintypes.UINT, wintypes.HANDLE], wintypes.HANDLE),
                },
            ),
            (
                cls.kernel32,
                {
                    'GlobalAlloc': ([wintypes.UINT, ctypes.c_size_t], wintypes.HGLOBAL),
                    'GlobalLock': ([wintypes.HGLOBAL], ctypes.c_void_p),
                    'GlobalUnlock': ([wintypes.HGLOBAL], wintypes.BOOL),
                    'GlobalSize': ([wintypes.HGLOBAL], ctypes.c_size_t),
                    'GlobalFree': ([wintypes.HGLOBAL], wintypes.HGLOBAL),
                },
            ),
        ):
            for name, (args, result) in signatures.items():
                function = getattr(dll, name)
                function.argtypes, function.restype = args, result

    @classmethod
    def tearDownClass(cls) -> None:
        from kitty.fast_data_types import glfw_terminate, set_boss, set_options

        set_boss(None)  # type: ignore
        set_options(None)
        glfw_terminate()

    def setUp(self) -> None:
        from kitty.clipboard import Clipboard
        from kitty.fast_data_types import set_boss, set_options
        from kitty.options.types import defaults

        set_options(defaults)
        self.clipboard = Clipboard()
        set_boss(SimpleNamespace(clipboard=self.clipboard))  # type: ignore
        self.clipboard_call(self.clipboard.set_mime, {})

    def clipboard_call[T](self, operation: Callable[..., T], *args: object) -> T:
        # Clipboard listeners can briefly lock the system clipboard after a write.
        for _ in range(100):
            self.glfw.glfwGetError(None)
            result = operation(*args)
            description = ctypes.c_char_p()
            if not self.glfw.glfwGetError(ctypes.byref(description)):
                return result
            if b'Failed to open clipboard' not in (description.value or b''):
                self.fail(f'Unexpected GLFW error: {description.value!r}')
            time.sleep(0.01)
        self.fail('Clipboard remained locked')

    @contextmanager
    def open_clipboard(self) -> Generator[None]:
        for _ in range(100):
            if self.user32.OpenClipboard(self.user32.GetClipboardOwner()):
                break
            self.assertEqual(ctypes.get_last_error(), 5)  # ERROR_ACCESS_DENIED
            time.sleep(0.01)
        else:
            self.fail('Clipboard remained locked')
        try:
            yield
        finally:
            self.assertTrue(self.user32.CloseClipboard())

    def put_raw(self, fmt: int, data: bytes) -> None:
        handle = self.kernel32.GlobalAlloc(0x0042, max(1, len(data)))  # GMEM_MOVEABLE | GMEM_ZEROINIT
        self.assertTrue(handle)
        pointer = self.kernel32.GlobalLock(handle)
        if not pointer:
            self.kernel32.GlobalFree(handle)
            self.fail('GlobalLock failed')
        ctypes.memmove(pointer, data, len(data))
        self.kernel32.GlobalUnlock(handle)
        if not self.user32.SetClipboardData(fmt, handle):
            self.kernel32.GlobalFree(handle)
            self.fail('SetClipboardData failed')

    def read_raw(self, fmt: int) -> bytes:
        handle = self.user32.GetClipboardData(fmt)
        self.assertTrue(handle)
        pointer = self.kernel32.GlobalLock(handle)
        self.assertTrue(pointer)
        try:
            return ctypes.string_at(pointer, self.kernel32.GlobalSize(handle))
        finally:
            self.kernel32.GlobalUnlock(handle)

    def test_binary_roundtrip(self) -> None:
        for payload in (b'', b'\x00', b'abc', b'abc\x00\x00', b'{"ok":true}', b'x' * 16, bytes(range(256)) * 40):
            with self.subTest(size=len(payload)):
                self.clipboard_call(self.clipboard.set_mime, {'application/octet-stream': payload})
                self.assertEqual(self.clipboard_call(self.clipboard.get_mime_data, 'application/octet-stream'), payload)

    def test_multiple_formats_and_streamed_data(self) -> None:
        def chunks() -> Callable[[], bytes]:
            return iter((b'first', b'\x00second\x00', b'')).__next__

        self.clipboard_call(self.clipboard.set_mime, {'text/plain': b'line1\nline2', 'application/json': b'{"a":1}', 'image/png': chunks})
        self.assertEqual(set(self.clipboard_call(self.clipboard.get_available_mime_types_for_paste)), {'text/plain', 'application/json', 'image/png'})
        for _ in range(2):
            self.assertEqual(self.clipboard_call(self.clipboard.get_mime_data, 'application/json'), b'{"a":1}')
            self.assertEqual(self.clipboard_call(self.clipboard.get_mime_data, 'image/png'), b'first\x00second\x00')
            self.assertEqual(self.clipboard_call(self.clipboard.get_text), 'line1\nline2')

    def test_padding_and_external_reader(self) -> None:
        mime, payload = 'application/octet-stream', b'binary\x00\x00'
        self.clipboard_call(self.clipboard.set_mime, {mime: payload})
        fmt = self.user32.RegisterClipboardFormatW(mime)
        size_fmt = self.user32.RegisterClipboardFormatW(f'KitsuClipboardLengthV1:{mime}')
        with self.open_clipboard():
            self.assertEqual(self.read_raw(fmt)[: len(payload)], payload)
            self.assertEqual(int.from_bytes(self.read_raw(size_fmt)[:8], 'little'), len(payload))
            self.put_raw(fmt, payload + b'padding' * 8)
        self.assertEqual(self.clipboard_call(self.clipboard.get_mime_data, mime), payload)
        code = (
            'from kitty.main import init_glfw_module; init_glfw_module("win32"); '
            'from kitty.clipboard import Clipboard; '
            f'assert Clipboard().get_mime_data({mime!r}) == {payload!r}'
        )
        subprocess.run([sys.executable, '+runpy', code], check=True, timeout=30)

    def test_foreign_data_and_invalid_length(self) -> None:
        mime = 'application/octet-stream'
        fmt = self.user32.RegisterClipboardFormatW(mime)
        size_fmt = self.user32.RegisterClipboardFormatW(f'KitsuClipboardLengthV1:{mime}')
        for length in (None, b'\xff', (2**64 - 1).to_bytes(8, 'little')):
            with self.subTest(length=length):
                self.clipboard_call(self.clipboard.set_mime, {mime: b'old data'})
                with self.open_clipboard():
                    self.assertTrue(self.user32.EmptyClipboard())
                    self.put_raw(fmt, b'foreign\x00\x00')
                    if length is not None:
                        self.put_raw(size_fmt, length)
                    expected = self.read_raw(fmt)
                self.assertEqual(self.clipboard_call(self.clipboard.get_mime_data, mime), expected)


if __name__ == '__main__':
    if hasattr(sys, 'kitty_run_data'):
        unittest.main(verbosity=2)
    else:
        package = Path(sys.argv[1] if len(sys.argv) > 1 else 'windows-package/kitsu').resolve()
        script = str(Path(__file__).resolve())
        code = f'import runpy, sys; sys.argv = [{script!r}]; runpy.run_path({script!r}, run_name="__main__")'
        raise SystemExit(subprocess.run([str(package / 'bin' / 'kitsu.com'), '+runpy', code], timeout=120).returncode)
