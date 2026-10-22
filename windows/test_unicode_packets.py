#!/usr/bin/env python
"""Headless VK_PACKET regression: python windows/test_unicode_packets.py windows-package/kitsu."""

from __future__ import annotations

import ctypes
import os
import sys
import unittest
from ctypes import wintypes
from pathlib import Path
from typing import Any

PACKAGE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('windows-package/kitsu').resolve()
WM_KEYDOWN, WM_KEYUP, WM_CHAR = 0x0100, 0x0101, 0x0102
VK_PACKET = 0xE7
GLFW_PRESS, GLFW_REPEAT, GLFW_RELEASE = 1, 2, 0


class KeyEvent(ctypes.Structure):
    _fields_ = [
        ('key', ctypes.c_uint32), ('shifted_key', ctypes.c_uint32), ('alternate_key', ctypes.c_uint32),
        ('native_key', ctypes.c_int), ('action', ctypes.c_int), ('mods', ctypes.c_int), ('text', ctypes.c_char_p),
        ('ime_state', ctypes.c_int), ('native_key_id', ctypes.c_uint32), ('fake_event_on_focus_change', ctypes.c_bool),
        ('win32_vk', ctypes.c_uint32), ('win32_char', ctypes.c_uint32), ('win32_control_state', ctypes.c_uint32),
    ]


KeyCallback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(KeyEvent))


class UnicodePackets(unittest.TestCase):
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
        cls.glfw.glfwSetKeyboardCallback.argtypes = [ctypes.c_void_p, KeyCallback]
        cls.glfw.glfwSetKeyboardCallback.restype = ctypes.c_void_p
        cls.glfw.glfwDestroyWindow.argtypes = [ctypes.c_void_p]
        cls.glfw.glfwDestroyWindow.restype = None
        cls.glfw.glfwTerminate.argtypes = []
        cls.glfw.glfwTerminate.restype = None
        cls.user32 = ctypes.WinDLL('user32', use_last_error=True)
        for name in ('SendMessageW', 'PostMessageW'):
            func = getattr(cls.user32, name)
            func.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
            func.restype = ctypes.c_ssize_t
        if not cls.glfw.glfwInit(0, ctypes.byref(ctypes.c_bool())):
            raise RuntimeError('glfwInit failed')

    @classmethod
    def tearDownClass(cls) -> None:
        cls.glfw.glfwTerminate()
        cls.dll_directory.close()

    def setUp(self) -> None:
        self.glfw.glfwWindowHint(0x00022001, 0)  # GLFW_CLIENT_API = GLFW_NO_API
        self.glfw.glfwWindowHint(0x00020004, 0)  # GLFW_VISIBLE = false
        self.window = self.glfw.glfwCreateWindow(100, 100, b'Unicode packet regression', None, None, None)
        self.assertTrue(self.window, 'glfwCreateWindow failed')
        self.addCleanup(self.glfw.glfwDestroyWindow, self.window)
        self.hwnd = self.glfw.glfwGetWin32Window(self.window)
        self.events: list[dict[str, Any]] = []

        def on_key(window: int, event: Any) -> None:
            ev = event.contents
            self.events.append({field[0]: getattr(ev, field[0]) for field in KeyEvent._fields_})

        self.callback = KeyCallback(on_key)
        self.glfw.glfwSetKeyboardCallback(self.window, self.callback)

    def packet(self, unit: int, scan: int | None = None) -> None:
        # Queue the WM_CHAR that TranslateMessage produces, without injecting into another application.
        lparam = 1 | ((unit if scan is None else scan) << 16)
        self.assertTrue(self.user32.PostMessageW(self.hwnd, WM_CHAR, unit, lparam))
        self.user32.SendMessageW(self.hwnd, WM_KEYDOWN, VK_PACKET, lparam)
        self.user32.SendMessageW(self.hwnd, WM_KEYUP, VK_PACKET, lparam | (3 << 30))

    def check_text(self, text: str) -> None:
        self.assertEqual(len(self.events), 1, self.events)
        event = self.events[0]
        self.assertEqual(event['text'], text.encode('utf-8'))
        self.assertEqual(event['action'], GLFW_PRESS)
        self.assertEqual(event['win32_vk'], VK_PACKET)
        for name in ('key', 'shifted_key', 'alternate_key', 'native_key', 'native_key_id', 'mods', 'win32_char', 'win32_control_state'):
            self.assertEqual(event[name], 0, name)

    def test_packet_text_never_acquires_hardware_identity(self) -> None:
        # F7, Alt, F11/F12, keypad, Pause/PrintScreen fixups, non-ASCII and zero scancode.
        for char in 'A8WXGHS6T\u0141\u65e5':
            for scan in (ord(char), 0):
                with self.subTest(char=char, scan=scan):
                    self.events.clear()
                    self.packet(ord(char), scan)
                    self.check_text(char)

    def test_repeated_packets_remain_independent_text(self) -> None:
        for _ in range(3):
            self.events.clear()
            self.packet(ord('A'))
            self.check_text('A')

    def test_surrogate_pair_is_delivered_once(self) -> None:
        self.packet(0xD83D, 0)
        self.assertEqual(self.events, [])
        self.packet(0xDC08, 0)
        self.check_text('\U0001f408')

    def test_packets_do_not_release_or_repeat_held_hardware_keys(self) -> None:
        for vk, scan in ((0x76, 0x41), (0xA4, 0x38), (0x67, 0x47)):
            with self.subTest(vk=vk):
                self.events.clear()
                self.user32.SendMessageW(self.hwnd, WM_KEYDOWN, vk, 1 | (scan << 16))
                self.assertEqual(self.events[-1]['action'], GLFW_PRESS)
                key = self.events[-1]['key']
                self.events.clear()
                self.packet(scan)
                self.check_text(chr(scan))
                self.user32.SendMessageW(self.hwnd, WM_KEYDOWN, vk, 1 | (scan << 16) | (1 << 30))
                self.user32.SendMessageW(self.hwnd, WM_KEYUP, vk, 1 | (scan << 16) | (3 << 30))
                self.assertEqual([e['action'] for e in self.events], [GLFW_PRESS, GLFW_REPEAT, GLFW_RELEASE])
                self.assertEqual([e['key'] for e in self.events[1:]], [key, key])


if __name__ == '__main__':
    unittest.main()
