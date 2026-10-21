#!/usr/bin/env python
"""Exercise the native Win32 primary selection without creating a terminal window.

Run against a built package: python windows/test_primary_selection.py windows-package/kitsu
"""

import subprocess
import sys
import unittest
from base64 import standard_b64encode
from collections.abc import Callable
from pathlib import Path
from types import SimpleNamespace
from typing import Any


class PrimarySelection(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        from kitty.main import init_glfw_module

        init_glfw_module('win32')

    @classmethod
    def tearDownClass(cls) -> None:
        from kitty.fast_data_types import glfw_terminate, set_boss, set_options

        set_boss(None)  # type: ignore
        set_options(None)
        glfw_terminate()

    def setUp(self) -> None:
        from kitty.clipboard import Clipboard, ClipboardType
        from kitty.fast_data_types import set_boss, set_options
        from kitty.options.types import defaults

        set_options(defaults._replace(clipboard_control=('read-primary', 'write-primary')))
        self.primary = Clipboard(ClipboardType.primary_selection)
        self.clipboard = Clipboard()
        self.responses: list[str] = []
        screen = SimpleNamespace(send_escape_code_to_child=lambda code, data: self.responses.append(data))
        self.boss: Any = SimpleNamespace(primary_selection=self.primary, clipboard=self.clipboard, window_id_map={1: SimpleNamespace(screen=screen)})
        set_boss(self.boss)
        self.primary.set_mime({})

    def test_empty_selection(self) -> None:
        self.assertEqual(self.primary.get_text(), '')
        self.assertEqual(self.primary.get_available_mime_types_for_paste(), ())

    def test_text_replacement_and_clear(self) -> None:
        for text in ('primary selection', 'Unicode: \u65e5\u672c\u8a9e \U0001f98a\nsecond line', 'replacement', ''):
            with self.subTest(text=text):
                self.primary.set_text(text)
                self.assertEqual(self.primary.get_text(), text)
        self.primary.set_mime({})
        self.assertEqual(self.primary.get_available_mime_types_for_paste(), ())
        self.assertEqual(self.primary.get_text(), '')

    def test_mime_types_and_streamed_data(self) -> None:
        def chunks() -> Callable[[], bytes]:
            return iter((b'first', b'\x00second', b'')).__next__

        self.primary.set_mime({'text/plain': b'text', 'application/octet-stream': chunks})
        self.assertEqual(self.primary.get_available_mime_types_for_paste(), ('text/plain', 'application/octet-stream'))
        for _ in range(2):
            self.assertEqual(self.primary.get_mime_data('application/octet-stream'), b'first\x00second')
        self.assertEqual(self.primary.get_mime_data('missing/type'), b'')

    def test_self_offer_callback(self) -> None:
        from kitty.fast_data_types import GLFW_PRIMARY_SELECTION, get_clipboard_mime

        self.primary.set_text('self offer')
        for mime in (None, 'text/plain'):
            with self.subTest(mime=mime), self.assertRaisesRegex(RuntimeError, '^is_self_offer$'):
                get_clipboard_mime(GLFW_PRIMARY_SELECTION, mime, lambda data: self.fail('expected a self offer'))

    def test_system_clipboard_is_independent(self) -> None:
        self.clipboard.set_text('clipboard sentinel')
        self.primary.set_text('primary sentinel')
        self.assertEqual(self.clipboard.get_text(), 'clipboard sentinel')
        self.assertEqual(self.primary.get_text(), 'primary sentinel')
        self.clipboard.set_text('clipboard replacement')
        self.assertEqual(self.primary.get_text(), 'primary sentinel')
        self.primary.set_mime({})
        self.assertEqual(self.clipboard.get_text(), 'clipboard replacement')

    def test_paste_from_selection(self) -> None:
        from kitty.boss import Boss

        pasted: list[str] = []
        window = SimpleNamespace(send_paste_event=lambda **kw: False, paste_with_actions=pasted.append)
        self.boss.window_for_dispatch = window
        self.boss.current_primary_selection_or_clipboard = lambda: Boss.current_primary_selection_or_clipboard(self.boss)
        self.primary.set_text('paste action')
        Boss.paste_from_selection(self.boss)
        self.assertEqual(pasted, ['paste action'])

    def test_osc52_primary_and_permissions(self) -> None:
        from kitty.clipboard import ClipboardRequestManager
        from kitty.fast_data_types import set_options
        from kitty.options.types import defaults

        manager = ClipboardRequestManager(1)
        payload = standard_b64encode('OSC 52 primary \u65e5\u672c\u8a9e'.encode())
        manager.parse_osc_52(memoryview(b'p;' + payload))
        manager.parse_osc_52(memoryview(b'p;?'))
        self.assertEqual(self.responses, ['52;p;' + payload.decode('ascii')])
        self.assertEqual(self.primary.get_text(), 'OSC 52 primary \u65e5\u672c\u8a9e')
        set_options(defaults._replace(clipboard_control=()))
        manager.parse_osc_52(memoryview(b'p;' + standard_b64encode(b'denied')))
        manager.parse_osc_52(memoryview(b'p;?'))
        self.assertEqual(self.responses[-1], '52;p;')
        self.assertEqual(self.primary.get_text(), 'OSC 52 primary \u65e5\u672c\u8a9e')


if __name__ == '__main__':
    if hasattr(sys, 'kitty_run_data'):
        unittest.main(verbosity=2)
    else:
        package = Path(sys.argv[1] if len(sys.argv) > 1 else 'windows-package/kitsu').resolve()
        script = str(Path(__file__).resolve())
        code = f'import runpy, sys; sys.argv = [{script!r}]; runpy.run_path({script!r}, run_name="__main__")'
        raise SystemExit(subprocess.run([str(package / 'bin' / 'kitsu.com'), '+runpy', code], timeout=120).returncode)
