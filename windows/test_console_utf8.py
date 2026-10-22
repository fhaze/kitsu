#!/usr/bin/env python
"""Headless regression for the code pages and byte output of actual ConPTY children."""

import os
import subprocess
import sys
import tempfile
import threading
import unittest
from contextlib import suppress
from pathlib import Path

PROBE_SOURCE = r'''
#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1 && (!SetConsoleCP(932) || !SetConsoleOutputCP(932))) return 1;
    printf("CP=%u,%u\n", GetConsoleCP(), GetConsoleOutputCP());
    fflush(stdout);
    const char payload[] = "\x1b]2;t\xc3\xaf" "tle \xe6\xbc\xa2\xf0\x9f\x98\x80\a"
        "\x1b]8;;https://example.com/\xe6\xbc\xa2\x1b\\link\x1b]8;;\x1b\\"
        "\x1b]66;s=2;osc66-\xe6\xbc\xa2\xf0\x9f\x98\x80\a";
    DWORD written;
    return !WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), payload, sizeof(payload) - 1, &written, NULL)
        || written != sizeof(payload) - 1;
}
'''


class ConsoleUTF8(unittest.TestCase):
    def run_probe(self, *args: str) -> bytes:
        import ctypes
        import msvcrt
        import socket

        from kitty.constants import kitten_exe
        from kitty.fast_data_types import openpty, spawn

        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.OpenProcess.restype = ctypes.c_void_p
        k32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        k32.GetExitCodeProcess.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32))
        k32.CloseHandle.argtypes = (ctypes.c_void_p,)
        master, slave = openpty()
        output = bytearray()

        def drain() -> None:
            sock = socket.socket(fileno=msvcrt.get_osfhandle(master))
            try:
                with suppress(OSError):
                    while chunk := sock.recv(65536):
                        output.extend(chunk)
            finally:
                sock.detach()

        reader = threading.Thread(target=drain, daemon=True)
        reader.start()
        try:
            pid = spawn(PROBE, os.getcwd(), (PROBE,) + args, tuple(f'{k}={v}' for k, v in os.environ.items()),
                        master, slave, -1, -1, -1, -1, (), kitten_exe(), False, ())
        finally:
            os.close(slave)
        handle = k32.OpenProcess(0x00100000 | 0x1000, False, pid)
        self.assertTrue(handle, ctypes.get_last_error())
        try:
            self.assertEqual(k32.WaitForSingleObject(handle, 10000), 0)
            status = ctypes.c_uint32()
            self.assertTrue(k32.GetExitCodeProcess(handle, ctypes.byref(status)))
            self.assertEqual(status.value, 0)
        finally:
            k32.CloseHandle(handle)
        reader.join(10)
        self.assertFalse(reader.is_alive(), 'ConPTY output did not close')
        return bytes(output)

    def test_initial_code_pages_and_raw_utf8(self) -> None:
        output = self.run_probe()
        self.assertIn(b'CP=65001,65001', output)
        for text in ('tïtle 漢😀', 'https://example.com/漢', 'osc66-漢😀'):
            with self.subTest(payload=text):
                self.assertIn(text.encode('utf-8'), output)

    def test_code_page_changes_are_console_local(self) -> None:
        self.assertIn(b'CP=932,932', self.run_probe('legacy'))
        self.assertIn(b'CP=65001,65001', self.run_probe())


if __name__ == '__main__':
    if hasattr(sys, 'kitty_run_data'):
        PROBE = sys.argv.pop(1)
        unittest.main(verbosity=2)
    else:
        package = Path(sys.argv[1] if len(sys.argv) > 1 else 'windows-package/kitsu').resolve()
        script = str(Path(__file__).resolve())
        with tempfile.TemporaryDirectory(prefix='kitsu code pages ') as tmp:
            source, probe = Path(tmp) / 'probe.c', Path(tmp) / 'probe.exe'
            source.write_text(PROBE_SOURCE, encoding='utf-8')
            subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(probe)], check=True)
            code = f'import runpy, sys; sys.argv = [{script!r}, {str(probe)!r}]; runpy.run_path({script!r}, run_name="__main__")'
            raise SystemExit(subprocess.run([str(package / 'bin/kitsu.com'), '+runpy', code], timeout=90).returncode)
