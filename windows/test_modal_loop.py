#!/usr/bin/env python
"""Run against a built package: python windows/test_modal_loop.py windows-package/kitsu."""

from __future__ import annotations

import ctypes
import os
import sys
import time
import unittest
from collections.abc import Callable
from ctypes import wintypes
from pathlib import Path

PACKAGE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('windows-package/kitsu').resolve()
WM_ENTERSIZEMOVE, WM_EXITSIZEMOVE = 0x0231, 0x0232
WM_ENTERMENULOOP, WM_EXITMENULOOP = 0x0211, 0x0212
WM_SIZING = 0x0214
WM_TEST = 0x8000 + 90
TickCallback = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
TimerCallback = ctypes.CFUNCTYPE(None, ctypes.c_ulonglong, ctypes.c_void_p)
LiveResizeCallback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_bool)
WindowProc = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)


class ModalLoop(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.dll_directory = os.add_dll_directory(str(PACKAGE / 'bin'))
        cls.glfw = ctypes.CDLL(str(PACKAGE / 'lib/kitty/kitty/glfw-win32.dll'))
        cls.user32 = ctypes.WinDLL('user32', use_last_error=True)
        signatures = {
            'glfwInit': ([ctypes.c_longlong, ctypes.POINTER(ctypes.c_bool)], ctypes.c_int),
            'glfwWindowHint': ([ctypes.c_int, ctypes.c_int], None),
            'glfwCreateWindow': ([ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_void_p),
            'glfwGetWin32Window': ([ctypes.c_void_p], wintypes.HWND),
            'glfwSetLiveResizeCallback': ([ctypes.c_void_p, LiveResizeCallback], ctypes.c_void_p),
            'glfwRunMainLoop': ([TickCallback, ctypes.c_void_p], None),
            'glfwStopMainLoop': ([], None),
            'glfwPostEmptyEvent': ([], None),
            'glfwAddTimer': ([ctypes.c_longlong, ctypes.c_bool, TimerCallback, ctypes.c_void_p, ctypes.c_void_p], ctypes.c_ulonglong),
            'glfwRemoveTimer': ([ctypes.c_ulonglong], None),
            'glfwDestroyWindow': ([ctypes.c_void_p], None),
            'glfwTerminate': ([], None),
        }
        for name, (args, result) in signatures.items():
            function = getattr(cls.glfw, name)
            function.argtypes, function.restype = args, result
        signatures = {
            'SendMessageW': ([wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM], ctypes.c_ssize_t),
            'PostMessageW': ([wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM], wintypes.BOOL),
            'PeekMessageW': ([ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT, wintypes.UINT], wintypes.BOOL),
            'TranslateMessage': ([ctypes.POINTER(wintypes.MSG)], wintypes.BOOL),
            'DispatchMessageW': ([ctypes.POINTER(wintypes.MSG)], ctypes.c_ssize_t),
            'SetWindowLongPtrW': ([wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t], ctypes.c_ssize_t),
            'CallWindowProcW': ([ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM], ctypes.c_ssize_t),
        }
        for name, (args, result) in signatures.items():
            function = getattr(cls.user32, name)
            function.argtypes, function.restype = args, result
        if not cls.glfw.glfwInit(0, ctypes.byref(ctypes.c_bool())):
            raise RuntimeError('glfwInit failed')

    @classmethod
    def tearDownClass(cls) -> None:
        cls.glfw.glfwTerminate()
        cls.dll_directory.close()

    def setUp(self) -> None:
        self.glfw.glfwWindowHint(0x00022001, 0)  # GLFW_CLIENT_API = GLFW_NO_API
        self.glfw.glfwWindowHint(0x00020004, 0)  # GLFW_VISIBLE = false
        self.window = self.glfw.glfwCreateWindow(100, 100, b'Modal loop regression', None, None, None)
        self.assertTrue(self.window, 'glfwCreateWindow failed')
        self.hwnd = self.glfw.glfwGetWin32Window(self.window)
        self.ticks = 0
        self.resizes: list[bool] = []
        self.callback_error: BaseException | None = None
        self.tick_callback = TickCallback(self.on_tick)
        self.resize_callback = LiveResizeCallback(lambda window, started: self.resizes.append(started))
        self.glfw.glfwSetLiveResizeCallback(self.window, self.resize_callback)

    def tearDown(self) -> None:
        if self.window:
            self.glfw.glfwDestroyWindow(self.window)

    def send(self, message: int, wparam: int = 0, lparam: int = 0) -> None:
        self.user32.SendMessageW(self.hwnd, message, wparam, lparam)

    def on_tick(self, data: int) -> None:
        self.ticks += 1

    def pump(self, duration: float = 0.12) -> None:
        # Deliberately do not call glfwPollEvents/WaitEvents: a native modal pump
        # cannot reach their timer or main-loop dispatch paths.
        deadline = time.monotonic() + duration
        message = wintypes.MSG()
        while time.monotonic() < deadline:
            if self.user32.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
                self.user32.TranslateMessage(ctypes.byref(message))
                self.user32.DispatchMessageW(ctypes.byref(message))
            else:
                time.sleep(0.001)

    def run_in_window_proc(self, body: Callable[[], None]) -> None:
        def window_proc(hwnd: int, message: int, wparam: int, lparam: int) -> int:
            if message != WM_TEST:
                return self.user32.CallWindowProcW(original, hwnd, message, wparam, lparam)
            try:
                body()
            except BaseException as error:
                self.callback_error = error
            finally:
                self.glfw.glfwStopMainLoop()
            return 0

        callback = WindowProc(window_proc)
        original = self.user32.SetWindowLongPtrW(self.hwnd, -4, ctypes.cast(callback, ctypes.c_void_p).value)
        self.assertTrue(original, 'SetWindowLongPtrW failed')
        try:
            self.assertTrue(self.user32.PostMessageW(self.hwnd, WM_TEST, 0, 0))
            self.glfw.glfwRunMainLoop(self.tick_callback, None)
        finally:
            if self.window:
                self.user32.SetWindowLongPtrW(self.hwnd, -4, original)
        if self.callback_error:
            raise self.callback_error

    def test_resize_notifications_ignore_moves_and_menus(self) -> None:
        self.send(WM_ENTERSIZEMOVE)
        self.send(WM_EXITSIZEMOVE)
        self.send(WM_ENTERMENULOOP)
        self.send(WM_EXITMENULOOP)
        self.assertEqual(self.resizes, [])
        rect = wintypes.RECT(0, 0, 200, 200)
        for attempt in range(2):
            self.send(WM_ENTERSIZEMOVE)
            self.send(WM_SIZING, 2, ctypes.addressof(rect))
            self.send(WM_SIZING, 2, ctypes.addressof(rect))
            self.assertEqual(self.resizes, [True, False] * attempt + [True])
            self.send(WM_ENTERMENULOOP)
            self.send(WM_EXITMENULOOP)
            self.assertTrue(self.resizes[-1])
            self.send(WM_EXITSIZEMOVE)
        self.assertEqual(self.resizes, [True, False, True, False])

    def check_dispatch(self, enter: int, leave: int, destroy: bool = False) -> None:
        timer_calls = 0

        def timer(timer_id: int, data: int) -> None:
            nonlocal timer_calls
            timer_calls += 1
            self.glfw.glfwPostEmptyEvent()

        callback = TimerCallback(timer)
        timer_id = self.glfw.glfwAddTimer(10_000_000, True, callback, None, None)
        self.assertTrue(timer_id)

        def body() -> None:
            self.send(enter)
            self.glfw.glfwPostEmptyEvent()
            self.pump()
            self.assertGreater(self.ticks, 0, 'ticks stalled inside the modal pump')
            self.assertGreater(timer_calls, 0, 'GLFW timers stalled inside the modal pump')
            self.send(WM_ENTERMENULOOP)
            self.send(WM_EXITMENULOOP)
            before = self.ticks
            self.pump()
            self.assertGreater(self.ticks, before, 'nested menu exit stopped modal dispatch')
            if destroy:
                self.glfw.glfwDestroyWindow(self.window)
                self.window = None
            else:
                self.send(leave)
            before = self.ticks, timer_calls
            self.pump()
            self.assertEqual((self.ticks, timer_calls), before, 'modal dispatch survived exit/destruction')

        try:
            self.run_in_window_proc(body)
        finally:
            self.glfw.glfwRemoveTimer(timer_id)

    def test_size_move_dispatches_timers_and_ticks(self) -> None:
        self.check_dispatch(WM_ENTERSIZEMOVE, WM_EXITSIZEMOVE)

    def test_menu_dispatches_timers_and_ticks(self) -> None:
        self.check_dispatch(WM_ENTERMENULOOP, WM_EXITMENULOOP)

    def test_destroy_in_modal_loop_cleans_up_dispatch(self) -> None:
        self.check_dispatch(WM_ENTERSIZEMOVE, WM_EXITSIZEMOVE, destroy=True)

    def test_stopping_main_loop_suppresses_modal_callbacks(self) -> None:
        def body() -> None:
            self.send(WM_ENTERMENULOOP)
            self.glfw.glfwPostEmptyEvent()
            self.pump()
            self.assertGreater(self.ticks, 0)
            self.glfw.glfwStopMainLoop()
            before = self.ticks
            self.glfw.glfwPostEmptyEvent()
            self.pump()
            self.assertEqual(self.ticks, before)
            self.send(WM_EXITMENULOOP)

        self.run_in_window_proc(body)

    def test_callbacks_do_not_reenter(self) -> None:
        depth = 0
        maximum_depth = 0

        def callback_body() -> None:
            nonlocal depth, maximum_depth
            depth += 1
            maximum_depth = max(depth, maximum_depth)
            self.glfw.glfwPostEmptyEvent()
            if depth < 3:
                self.pump(0.025)
            depth -= 1

        self.tick_callback = TickCallback(lambda data: callback_body())
        callback = TimerCallback(lambda timer_id, data: callback_body())
        timer_id = self.glfw.glfwAddTimer(10_000_000, True, callback, None, None)

        def body() -> None:
            self.send(WM_ENTERSIZEMOVE)
            self.glfw.glfwPostEmptyEvent()
            self.pump()
            self.send(WM_EXITSIZEMOVE)
            self.assertEqual(maximum_depth, 1)

        try:
            self.run_in_window_proc(body)
        finally:
            self.glfw.glfwRemoveTimer(timer_id)


if __name__ == '__main__':
    unittest.main()
