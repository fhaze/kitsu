#!/usr/bin/env python
"""Verify the packaged Windows branding, including every embedded icon image."""

import ctypes
import os
import struct
import subprocess
import sys
import unittest
from ctypes import wintypes
from pathlib import Path

PACKAGE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('windows-package/kitsu').resolve()
ICON = Path(__file__).resolve().parents[1] / 'logo/kitsu.ico'


class TestIconAssets(unittest.TestCase):
    def test_png_dimensions(self) -> None:
        for name in ('kitsu.png', 'kitsu-128.png'):
            with self.subTest(icon=name):
                data = ICON.with_name(name).read_bytes()
                self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
                width, height = struct.unpack_from('>II', data, 16)
                self.assertGreater(width, 0)
                self.assertEqual(width, height)
                if name == 'kitsu-128.png':
                    self.assertEqual((width, height), (128, 128))

    def test_ico_dimensions(self) -> None:
        data = ICON.read_bytes()
        sizes = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
        self.assertEqual(struct.unpack_from('<HHH', data), (0, 1, len(sizes)))
        for i, size in enumerate(sizes):
            with self.subTest(size=size):
                width, height, _, _, _, _, length, offset = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
                self.assertEqual((width or 256, height or 256), (size, size))
                frame = data[offset : offset + length]
                self.assertEqual(len(frame), length)
                self.assertEqual(frame[:8], b'\x89PNG\r\n\x1a\n')
                self.assertEqual(struct.unpack_from('>II', frame, 16), (size, size))


@unittest.skipUnless(sys.platform == 'win32', 'Windows resources')
class TestBranding(unittest.TestCase):
    def test_embedded_icons(self) -> None:
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.LoadLibraryExW.argtypes = (wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD)
        kernel.LoadLibraryExW.restype = wintypes.HMODULE
        kernel.FindResourceW.argtypes = (wintypes.HMODULE, wintypes.LPCWSTR, wintypes.LPCWSTR)
        kernel.FindResourceW.restype = wintypes.HRSRC
        kernel.SizeofResource.argtypes = (wintypes.HMODULE, wintypes.HRSRC)
        kernel.SizeofResource.restype = wintypes.DWORD
        kernel.LoadResource.argtypes = (wintypes.HMODULE, wintypes.HRSRC)
        kernel.LoadResource.restype = wintypes.HGLOBAL
        kernel.LockResource.argtypes = (wintypes.HGLOBAL,)
        kernel.LockResource.restype = ctypes.c_void_p
        kernel.FreeLibrary.argtypes = (wintypes.HMODULE,)
        ico = ICON.read_bytes()
        _, kind, count = struct.unpack_from('<HHH', ico)
        self.assertEqual(kind, 1)
        binaries = [(PACKAGE / 'bin' / name, 'GLFW_ICON') for name in ('kitsu.exe', 'kitsu.com', 'kitty.exe', 'kitty.com', 'kitten.exe')]
        binaries.extend((path, 'MAINICON') for path in PACKAGE.parent.glob('kitsu-*-windows-*-setup.exe'))
        expected_version = None
        for path, icon_name in binaries:
            with self.subTest(binary=path.name):
                module = kernel.LoadLibraryExW(str(path), None, 0x22)
                self.assertTrue(module, ctypes.get_last_error())
                try:

                    def resource(name: str | ctypes.c_wchar_p, kind: int) -> bytes:
                        found = kernel.FindResourceW(module, name, ctypes.cast(kind, wintypes.LPCWSTR))
                        self.assertTrue(found, ctypes.get_last_error())
                        size = kernel.SizeofResource(module, found)
                        ptr = kernel.LockResource(kernel.LoadResource(module, found))
                        self.assertTrue(ptr)
                        return ctypes.string_at(ptr, size)

                    group = resource(icon_name, 14)
                    if icon_name == 'GLFW_ICON':
                        version = resource(ctypes.cast(1, wintypes.LPCWSTR), 16)
                        self.assertIn('Kitsu'.encode('utf-16-le'), version)
                        if expected_version is None:
                            expected_version = version
                        self.assertEqual(version, expected_version)
                    self.assertEqual(struct.unpack_from('<HHH', group), (0, 1, count))
                    for i in range(count):
                        expected = struct.unpack_from('<BBBBHHII', ico, 6 + 16 * i)
                        actual = struct.unpack_from('<BBBBHHIH', group, 6 + 14 * i)
                        self.assertEqual(actual[:4], expected[:4])
                        # windres normalizes zero planes to one; Inno preserves the ICO header.
                        self.assertIn(actual[4], (expected[4], expected[4] or 1))
                        self.assertEqual(actual[5:7], expected[5:7])
                        self.assertEqual(resource(ctypes.cast(actual[7], wintypes.LPCWSTR), 3), ico[expected[7] : expected[7] + expected[6]])
                finally:
                    kernel.FreeLibrary(module)

    def test_commands_and_compatibility(self) -> None:
        for name in ('kitsu.exe', 'kitsu.com', 'kitty.exe', 'kitty.com'):
            with self.subTest(binary=name):
                result = subprocess.run([str(PACKAGE / 'bin' / name), '--version'], capture_output=True, text=True, timeout=30, check=True)
                self.assertIn('Kitsu ', result.stdout)
                self.assertIn('Kovid Goyal', result.stdout)
        for ext in ('.exe', '.com'):
            self.assertEqual((PACKAGE / 'bin' / ('kitsu' + ext)).read_bytes(), (PACKAGE / 'bin' / ('kitty' + ext)).read_bytes())

    def test_runtime_identity(self) -> None:
        code = (
            'from pathlib import Path; from kitty.constants import cli_name, display_name, appname, defconf, kitty_exe, logo_png_file; '
            'from kitty.main import get_icon128_path; from kitty.fast_data_types import load_png_data, set_default_window_icon; '
            'assert cli_name == "kitsu" and display_name == "Kitsu" and appname == "kitty"; '
            'assert Path(kitty_exe()).name == "kitsu.exe"; assert Path(defconf).name == "kitty.conf"; '
            'assert Path(logo_png_file).name == "kitsu.png"; '
            'small_logo = Path(get_icon128_path(logo_png_file)); assert small_logo.name == "kitsu-128.png"; '
            'assert load_png_data(small_logo.read_bytes())[1:] == (128, 128); '
            'set_default_window_icon(str(small_logo)); print(logo_png_file)'
        )
        env = dict(os.environ, KITTY_CONFIG_DIRECTORY=str(PACKAGE / 'test-config'))
        result = subprocess.run([str(PACKAGE / 'bin/kitsu.exe'), '+runpy', code], capture_output=True, text=True, timeout=30, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(Path(result.stdout.strip()).read_bytes(), ICON.with_suffix('.png').read_bytes())
        self.assertEqual(Path(result.stdout.strip()).with_name('kitsu-128.png').read_bytes(), ICON.with_name('kitsu-128.png').read_bytes())


if __name__ == '__main__':
    unittest.main()
