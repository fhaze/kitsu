#!/usr/bin/env python
"""Run against a built package: python windows/test_scroll.py windows-package/kitsu."""

from __future__ import annotations

import ctypes
import os
import sys
import unittest
from ctypes import wintypes
from pathlib import Path

PACKAGE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('windows-package/kitsu').resolve()
WM_MOUSEWHEEL = 0x020A
WM_MOUSEHWHEEL = 0x020E
GLFW_SCROLL_OFFEST_V120 = 1


class ScrollEvent(ctypes.Structure):
    _fields_ = [
        ('x_offset', ctypes.c_double), ('y_offset', ctypes.c_double),
        ('unscaled_x', ctypes.c_double), ('unscaled_y', ctypes.c_double),
        ('momentum_type', ctypes.c_int), ('offset_type', ctypes.c_int), ('keyboard_modifiers', ctypes.c_int),
    ]


ScrollCallback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(ScrollEvent))


class WheelMessages(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.dll_directory = os.add_dll_directory(str(PACKAGE / 'bin'))
        cls.glfw = ctypes.CDLL(str(PACKAGE / 'lib' / 'kitty' / 'kitty' / 'glfw-win32.dll'))
        cls.glfw.glfwInit.argtypes = [ctypes.c_longlong, ctypes.POINTER(ctypes.c_bool)]
        cls.glfw.glfwInit.restype = ctypes.c_int
        cls.glfw.glfwWindowHint.argtypes = [ctypes.c_int, ctypes.c_int]
        cls.glfw.glfwWindowHint.restype = None
        cls.glfw.glfwCreateWindow.argtypes = [
            ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        cls.glfw.glfwCreateWindow.restype = ctypes.c_void_p
        cls.glfw.glfwGetWin32Window.argtypes = [ctypes.c_void_p]
        cls.glfw.glfwGetWin32Window.restype = wintypes.HWND
        cls.glfw.glfwSetScrollCallback.argtypes = [ctypes.c_void_p, ScrollCallback]
        cls.glfw.glfwSetScrollCallback.restype = ctypes.c_void_p
        cls.glfw.glfwDestroyWindow.argtypes = [ctypes.c_void_p]
        cls.glfw.glfwDestroyWindow.restype = None
        cls.glfw.glfwTerminate.argtypes = []
        cls.glfw.glfwTerminate.restype = None
        cls.user32 = ctypes.WinDLL('user32', use_last_error=True)
        cls.user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        cls.user32.SendMessageW.restype = ctypes.c_ssize_t
        if not cls.glfw.glfwInit(0, ctypes.byref(ctypes.c_bool())):
            raise RuntimeError('glfwInit failed')

    @classmethod
    def tearDownClass(cls) -> None:
        cls.glfw.glfwTerminate()
        cls.dll_directory.close()

    def setUp(self) -> None:
        self.glfw.glfwWindowHint(0x00022001, 0)  # GLFW_CLIENT_API = GLFW_NO_API (no GPU required)
        self.glfw.glfwWindowHint(0x00020004, 0)  # GLFW_VISIBLE = false
        self.window = self.glfw.glfwCreateWindow(100, 100, b'Wheel regression', None, None, None)
        self.assertTrue(self.window, 'glfwCreateWindow failed')
        self.addCleanup(self.glfw.glfwDestroyWindow, self.window)
        self.hwnd = self.glfw.glfwGetWin32Window(self.window)
        self.events: list[tuple[float, float, float, float, int, int]] = []

        def on_scroll(window: int, event: ctypes._Pointer[ScrollEvent]) -> None:
            ev = event.contents
            self.events.append((ev.x_offset, ev.y_offset, ev.unscaled_x, ev.unscaled_y, ev.offset_type, ev.momentum_type))

        self.callback = ScrollCallback(on_scroll)
        self.glfw.glfwSetScrollCallback(self.window, self.callback)

    def check_deltas(self, message: int, deltas: tuple[int, ...]) -> None:
        for delta in deltas:
            with self.subTest(message=hex(message), delta=delta):
                self.events.clear()
                self.user32.SendMessageW(self.hwnd, message, (delta & 0xffff) << 16, 0)
                x, y = (-delta, 0) if message == WM_MOUSEHWHEEL else (0, delta)
                self.assertEqual(self.events, [(x, y, x, y, GLFW_SCROLL_OFFEST_V120, 0)])

    def test_vertical_raw_signed_deltas(self) -> None:
        self.check_deltas(WM_MOUSEWHEEL, (1, -1, 8, -8, 30, -30, 120, -120, 240, -240))

    def test_horizontal_raw_signed_deltas(self) -> None:
        self.check_deltas(WM_MOUSEHWHEEL, (1, -1, 8, -8, 30, -30, 120, -120, 240, -240))

    def test_fragments_and_direction_reversal(self) -> None:
        for message in (WM_MOUSEWHEEL, WM_MOUSEHWHEEL):
            self.check_deltas(message, (30, 30, 30, 30, -30, -30, -30, -30))


if __name__ == '__main__':
    unittest.main()
