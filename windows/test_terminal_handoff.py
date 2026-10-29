#!/usr/bin/env python
"""Cross-process ITerminalHandoff3 smoke test (also runs on Server 2022).

Uses synthetic conhost pipes, NOT Windows default-terminal delegation. --gui
opens the real Kitsu GUI for manual input/resize validation using those pipes.
All test COM registry overrides are restored, even when the test fails.
"""

from __future__ import annotations

import argparse
import ctypes as c
import json
import msvcrt
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import winreg
from contextlib import ExitStack, contextmanager
from ctypes import wintypes as w
from pathlib import Path
from typing import Iterator

CLSID = '{75AB2A9B-3128-4777-9D2B-3126EC262B1D}'
IID = '{6F23DA90-15C5-4203-9DB0-64E73F1B1B00}'
PROXY = '{3171DE52-6EFA-4AEF-8A9F-D02BD67E7A4F}'
TITLE = 'Kitsu handoff validation — synthetic console'
k32 = c.WinDLL('kernel32', use_last_error=True)
ole = c.OleDLL('ole32')
aut = c.WinDLL('oleaut32')
k32.CreatePipe.argtypes = [c.POINTER(w.HANDLE), c.POINTER(w.HANDLE), c.c_void_p, w.DWORD]
k32.CreatePipe.restype = w.BOOL
k32.CloseHandle.argtypes = [w.HANDLE]
k32.WriteFile.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
k32.ReadFile.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
k32.PeekNamedPipe.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.c_void_p, c.POINTER(w.DWORD), c.c_void_p]
ole.CoCreateInstance.argtypes = [c.c_void_p, c.c_void_p, w.DWORD, c.c_void_p, c.POINTER(c.c_void_p)]
ole.CoInitializeSecurity.argtypes = [c.c_void_p, w.LONG, c.c_void_p, c.c_void_p, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, c.c_void_p]
ole.CoSetProxyBlanket.argtypes = [c.c_void_p, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, c.c_void_p, w.DWORD]
aut.SysAllocString.argtypes, aut.SysAllocString.restype = [w.LPCWSTR], c.c_void_p
aut.SysFreeString.argtypes = [c.c_void_p]


class StartupInfo(c.Structure):
    _fields_ = (
        [('title', c.c_void_p), ('icon', c.c_void_p), ('index', w.LONG)]
        + [(n, w.DWORD) for n in ('x', 'y', 'width', 'height', 'columns', 'rows', 'fill', 'flags')]
        + [('show', w.WORD)]
    )


def guid(value: str) -> c.Array[c.c_char]:
    return c.create_string_buffer(uuid.UUID(value).bytes_le)


@contextmanager
def registry(path: str, name: str, value: str) -> Iterator[None]:
    path = 'Software\\Classes\\' + path
    existed = True
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_ALL_ACCESS)
    except FileNotFoundError:
        existed = False
        key = winreg.CreateKey(winreg.HKEY_CURRENT_USER, path)
    try:
        with key:
            try:
                previous = winreg.QueryValueEx(key, name)
            except FileNotFoundError:
                previous = None
            winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)
            try:
                yield
            finally:
                if previous is None:
                    winreg.DeleteValue(key, name)
                else:
                    winreg.SetValueEx(key, name, 0, previous[1], previous[0])
    finally:
        if not existed:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)


def pipe(stack: ExitStack) -> tuple[w.HANDLE, w.HANDLE]:
    read, write = w.HANDLE(), w.HANDLE()
    if not k32.CreatePipe(c.byref(read), c.byref(write), None, 131072):
        raise c.WinError(c.get_last_error())
    stack.callback(k32.CloseHandle, read)
    stack.callback(k32.CloseHandle, write)
    return read, write


def write(handle: w.HANDLE, data: bytes) -> None:
    n = w.DWORD()
    if not k32.WriteFile(handle, data, len(data), c.byref(n), None) or n.value != len(data):
        raise c.WinError(c.get_last_error())


def read(handle: w.HANDLE, timeout: float = 15) -> bytes:
    deadline = time.monotonic() + timeout
    available = w.DWORD()
    while time.monotonic() < deadline:
        if not k32.PeekNamedPipe(handle, None, 0, None, c.byref(available), None):
            raise c.WinError(c.get_last_error())
        if available.value:
            buf = c.create_string_buffer(available.value)
            n = w.DWORD()
            if not k32.ReadFile(handle, buf, len(buf), c.byref(n), None):
                raise c.WinError(c.get_last_error())
            return buf.raw[: n.value]
        time.sleep(0.02)
    raise TimeoutError('No data received from handoff pipe')


RECEIVER = """
import json, msvcrt, socket
from pathlib import Path
from kitty.fast_data_types import win32_receive_terminal_handoff, ChildMonitor, Screen
fd, pid, title = win32_receive_terminal_handoff(20000)
sock = socket.socket(fileno=msvcrt.get_osfhandle(fd))
sock.settimeout(10)
assert sock.recv(4096) == b'handoff output sentinel'
sock.sendall(b'handoff input sentinel')
monitor = ChildMonitor(lambda *a: None, None)
monitor.add_child(1, pid, fd, Screen(None, 24, 80, 100, 10, 20))
monitor.resize_pty(1, 37, 123, 0, 0)
Path(RESULT).write_text(json.dumps([pid, title]))
sock.detach()
"""


def run(package: Path, gui: bool = False) -> None:
    with ExitStack() as stack:
        stack.enter_context(registry('CLSID\\' + PROXY + '\\InprocServer32', '', str(package / 'bin/OpenConsoleProxy.dll')))
        stack.enter_context(registry('CLSID\\' + PROXY + '\\InprocServer32', 'ThreadingModel', 'Both'))
        stack.enter_context(registry('Interface\\' + IID + '\\ProxyStubClsid32', '', PROXY))
        tmp = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix='kitsu-handoff-')))
        result = tmp / 'result.json'
        args = ['+terminal-handoff'] if gui else ['+runpy', RECEIVER.replace('RESULT', repr(str(result)))]
        process = stack.enter_context(subprocess.Popen([str(package / 'bin/kitsu.exe'), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE))
        stack.callback(lambda: process.kill() if process.poll() is None else None)
        host = stack.enter_context(subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(180)'], creationflags=subprocess.CREATE_NO_WINDOW))
        stack.callback(lambda: host.kill() if host.poll() is None else None)
        signal_read, signal_write = pipe(stack)
        reference_file = stack.enter_context((tmp / 'reference').open('w+b'))
        reference = w.HANDLE(msvcrt.get_osfhandle(reference_file.fileno()))
        ole.CoInitializeEx(None, 0)
        stack.callback(ole.CoUninitialize)
        ole.CoInitializeSecurity(None, -1, None, None, 6, 3, None, 0, None)
        interface = c.c_void_p()
        deadline = time.monotonic() + 20
        while True:
            try:
                ole.CoCreateInstance(guid(CLSID), None, 4, guid(IID), c.byref(interface))
                break
            except OSError:
                if process.poll() is not None or time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        vtable = c.cast(interface, c.POINTER(c.POINTER(c.c_void_p))).contents
        release = c.WINFUNCTYPE(w.ULONG, c.c_void_p)(vtable[2])
        stack.callback(release, interface)
        ole.CoSetProxyBlanket(interface, 0xffffffff, 0xffffffff, None, 6, 3, None, 0)
        handoff = c.WINFUNCTYPE(
            c.HRESULT, c.c_void_p, c.POINTER(w.HANDLE), c.POINTER(w.HANDLE), w.HANDLE, w.HANDLE, w.HANDLE, w.HANDLE, c.POINTER(StartupInfo)
        )(vtable[3])
        info = StartupInfo(title=aut.SysAllocString(TITLE), columns=80, rows=24)
        stack.callback(aut.SysFreeString, info.title)
        input_read, output_write = w.HANDLE(), w.HANDLE()
        hr = handoff(interface, c.byref(input_read), c.byref(output_write), signal_write, reference, int(host._handle), int(host._handle), c.byref(info))
        if hr < 0:
            raise OSError(f'EstablishPtyHandoff failed: 0x{hr & 0xFFFFFFFF:08x}')
        stack.callback(k32.CloseHandle, input_read)
        stack.callback(k32.CloseHandle, output_write)
        print('COM handoff accepted; original handles remain caller-owned', flush=True)
        if gui:
            write(
                output_write,
                (
                    '\x1b[2J\x1b[HITerminalHandoff3 — synthetic console\r\n'
                    'Not Windows 11 default-terminal delegation.\r\n'
                    'Type text to check input; resize the window.\r\n'
                    'Type exit to end this fixture.\r\n> '
                ).encode(),
            )
            while process.poll() is None:
                try:
                    data = read(input_read, 1)
                except TimeoutError:
                    continue
                except BrokenPipeError:
                    process.wait(10)
                    break
                print('INPUT:', repr(data), flush=True)
                if b'exit' in data:
                    break
                write(output_write, data.replace(b'\r', b'\r\n> '))
            print('PASS: synthetic GUI fixture closed', flush=True)
        else:
            write(output_write, b'handoff output sentinel')
            assert read(input_read) == b'handoff input sentinel'
            resize = read(signal_read)
            assert resize == struct.pack('<hhh', 8, 123, 37), repr(resize)
            process.wait(20)
            stdout, stderr = process.communicate()
            assert process.returncode == 0, (process.returncode, stdout, stderr)
            assert json.loads(result.read_text()) == [host.pid, TITLE]
            print('PASS: cross-process handle marshaling, PTY input/output, title/PID, resize signal', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--gui', action='store_true')
    options = parser.parse_args()
    run(options.package.resolve(), options.gui)
