#!/usr/bin/env python
# License: GPL v3 Copyright: 2020, Kovid Goyal <kovid at kovidgoyal.net>

import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from .base import BaseTest

_plat = sys.platform.lower()
is_macos = 'darwin' in _plat


class TestGLFW(BaseTest):
    @unittest.skipUnless(sys.platform == 'win32', 'Windows DIP sizing')
    def test_windows_initial_window_size(self):
        from kitty.os_window_size import WindowSize, WindowSizeData, WindowSizes, initial_window_size_func
        from kitty.types import FloatEdges

        edges = FloatEdges(3, 3, 3, 3)
        opts = WindowSizeData(WindowSizes(WindowSize(640, 'px'), WindowSize(400, 'px')), True, edges, edges, edges, edges)
        # Pre-DIP cache entries cannot be interpreted at a different monitor DPI.
        cache = {'window-size': (1280, 800)}
        for scale in (1, 1.5, 2):
            with self.subTest(scale=scale):
                args = (int(8 * scale), int(16 * scale), 96 * scale, 96 * scale, scale, scale)
                self.ae(initial_window_size_func(opts, cache)(*args), (640, 400))
                cells = opts._replace(initial_window_sizes=WindowSizes(WindowSize(80, 'cells'), WindowSize(24, 'cells')))
                self.ae(initial_window_size_func(cells, {})(*args), (657, 401))
                self.ae(initial_window_size_func(opts, {'window-size-dips': (800, 500)})(*args), (800, 500))
                self.ae(initial_window_size_func(opts._replace(remember_window_size=False), {'window-size-dips': (800, 500)})(*args), (640, 400))

    @unittest.skipUnless(sys.platform == 'win32', 'Windows DIP sizing')
    def test_windows_resize_and_remember_window_size(self):
        from kitty.boss import Boss
        from kitty.options.types import defaults

        for scale in (1, 1.5, 2):
            metrics = {
                'width': 640, 'height': 400, 'is_layer_shell': False,
                'xscale': scale, 'yscale': scale, 'cell_width': int(8 * scale), 'cell_height': int(16 * scale),
            }
            with self.subTest(scale=scale), patch('kitty.boss.set_os_window_size') as resize:
                for unit, width, height in (('pixels', 640, 400), ('cells', 80, 25)):
                    Boss.resize_os_window(None, 1, width, height, unit, metrics=metrics)
                    resize.assert_called_with(1, 640, 400)
                    Boss.resize_os_window(None, 1, width, height, unit, incremental=True, metrics=metrics)
                    resize.assert_called_with(1, 1280, 800)
                Boss.resize_os_window(None, 1, 0, 500, 'pixels', metrics=metrics)
                resize.assert_called_with(1, 640, 500)
        boss = SimpleNamespace(os_window_map={}, window_id_map={}, cached_values={}, os_window_death_actions={})
        with patch('kitty.boss.get_options', return_value=defaults._replace(remember_window_position=False)):
            Boss.on_os_window_closed(boss, 1, 0, 0, 800, 500, False, False)
        self.ae(boss.cached_values['window-size-dips'], (800, 500))
        self.assertNotIn('window-size', boss.cached_values)

    def test_os_window_size_calculation(self):
        from kitty.utils import get_new_os_window_size

        def t(w, h, width=0, height=0, unit='cells', incremental=False):
            self.ae((w, h), get_new_os_window_size(metrics, width, height, unit, incremental, has_window_scaling))

        with self.subTest(has_window_scaling=False):
            has_window_scaling = False
            metrics = {
                'width': 200,
                'height': 100,
                'framebuffer_width': 200,
                'framebuffer_height': 100,
                'xscale': 2.0,
                'yscale': 2.0,
                'xdpi': 192.0,
                'ydpi': 192.0,
                'cell_width': 8,
                'cell_height': 16,
            }
            t(80 * metrics['cell_width'], 100, 80)
            t(80 * metrics['cell_width'] + metrics['width'], 100, 80, incremental=True)
            t(1217, 100, 1217, unit='pixels')
            t(1217 + metrics['width'], 100, 1217, unit='pixels', incremental=True)

        with self.subTest(has_window_scaling=True):
            has_window_scaling = True
            metrics['framebuffer_width'] = metrics['width'] * 2
            metrics['framebuffer_height'] = metrics['height'] * 2
            t(80 * metrics['cell_width'] / metrics['xscale'], 100, 80)
            t(80 * metrics['cell_width'] / metrics['xscale'] + metrics['width'], 100, 80, incremental=True)
            t(1217, 100, 1217, unit='pixels')
            t(1217 + metrics['width'], 100, 1217, unit='pixels', incremental=True)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Skipping test as only the Linux backends are built with backend_utils')
    def test_utf_8_strndup(self):
        import ctypes

        from kitty.constants import glfw_path

        backend_utils = glfw_path('x11')
        lib = ctypes.CDLL(backend_utils)
        libc = ctypes.CDLL(None)

        class allocated_c_char_p(ctypes.c_char_p):
            def __del__(self):
                libc.free(self)

        utf_8_strndup = lib.utf_8_strndup
        utf_8_strndup.restype = allocated_c_char_p
        utf_8_strndup.argtypes = (ctypes.c_char_p, ctypes.c_size_t)

        def test(string):
            string_bytes = bytes(string, 'utf-8')
            prev_part_bytes = b''
            prev_length_bytes = -1
            for length in range(len(string) + 1):
                part = string[:length]
                part_bytes = bytes(part, 'utf-8')
                length_bytes = len(part_bytes)
                for length_bytes_2 in range(prev_length_bytes + 1, length_bytes):
                    self.ae(utf_8_strndup(string_bytes, length_bytes_2).value, prev_part_bytes)
                self.ae(utf_8_strndup(string_bytes, length_bytes).value, part_bytes)
                prev_part_bytes = part_bytes
                prev_length_bytes = length_bytes
            self.ae(utf_8_strndup(string_bytes, len(string_bytes) + 1).value, string_bytes)  # Try to go one character after the end of the string

        self.ae(utf_8_strndup(None, 2).value, None)
        self.ae(utf_8_strndup(b'', 2).value, b'')

        test('ö')
        test('>a<')
        test('>ä<')
        test('>ế<')
        test('>𐍈<')
        test('∮ E⋅da = Q,  n → ∞, 𐍈∑ f(i) = ∏ g(i)')
        self.ae(utf_8_strndup(b'\xf0\x9f\x98\xb8', 3).value, b'')
        self.ae(utf_8_strndup(b'\xc3\xb6\xf0\x9f\x98\xb8', 4).value, b'\xc3\xb6')
