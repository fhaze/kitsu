#!/usr/bin/env python
"""Initializing GLFW must not change the system-wide foreground lock timeout.

Run against a built package: python windows/test_foreground_lock.py windows-package/kitsu
"""

import ctypes
import json
import subprocess
import sys
import unittest
from ctypes import wintypes
from pathlib import Path

PACKAGE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('windows-package/kitsu').resolve()
SPI_GETFOREGROUNDLOCKTIMEOUT = 0x2000
SPI_SETFOREGROUNDLOCKTIMEOUT = 0x2001
SENTINEL = 123457

INIT_GLFW = '''
import ctypes, json
from ctypes import wintypes
from kitty.fast_data_types import glfw_terminate
from kitty.main import init_glfw_module
# +runpy runs this with exec(), so functions defined here would not see these
# names, and the bundled Python strips asserts
value = wintypes.DWORD()
init_glfw_module('win32')
if not ctypes.windll.user32.SystemParametersInfoW(0x2000, 0, ctypes.byref(value), 0): raise ctypes.WinError()
after_init = value.value
glfw_terminate()
if not ctypes.windll.user32.SystemParametersInfoW(0x2000, 0, ctypes.byref(value), 0): raise ctypes.WinError()
print(json.dumps({'after_init': after_init, 'after_terminate': value.value}))
'''


def get_timeout() -> int:
    value = wintypes.DWORD()
    if not ctypes.windll.user32.SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, ctypes.byref(value), 0):
        raise ctypes.WinError()
    return value.value


def set_timeout(value: int) -> bool:
    # Neither SPIF_UPDATEINIFILE nor SPIF_SENDCHANGE, so this only lasts for the logon session
    return bool(ctypes.windll.user32.SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, ctypes.c_void_p(value), 0))


class ForegroundLockTimeout(unittest.TestCase):
    def test_glfw_init_leaves_foreground_lock_timeout_alone(self) -> None:
        original = get_timeout()
        # Windows only lets processes that may set the foreground window change the
        # timeout, so fall back to the current value, which must not be the 0 we test for
        expected = SENTINEL if set_timeout(SENTINEL) else original
        try:
            if expected == 0:
                self.skipTest('the foreground lock timeout is already 0 and cannot be changed from here')
            self.assertEqual(get_timeout(), expected)
            cp = subprocess.run(
                [str(PACKAGE / 'bin' / 'kitsu.com'), '+runpy', INIT_GLFW],
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120)
            self.assertEqual(cp.returncode, 0, cp.stdout + cp.stderr)
            result = json.loads(cp.stdout.strip().splitlines()[-1])
            self.assertEqual(result, {'after_init': expected, 'after_terminate': expected})
            self.assertEqual(get_timeout(), expected)
        finally:
            if expected != original:
                set_timeout(original)
        self.assertEqual(get_timeout(), original)


if __name__ == '__main__':
    unittest.main()
