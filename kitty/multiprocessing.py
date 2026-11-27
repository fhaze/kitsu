#!/usr/bin/env python
# License: GPLv3 Copyright: 2020, Kovid Goyal <kovid at kovidgoyal.net>

# Monkeypatch the stdlib multiprocessing module to work with the embedded python
# in kitty, when using the spawn launcher.


import os
import sys
from collections.abc import Callable, Sequence
from concurrent.futures import ProcessPoolExecutor
from multiprocessing import context, get_all_start_methods, get_context, spawn, util
from typing import Any

from .constants import kitty_exe

orig_spawn_passfds = util.spawnv_passfds
orig_get_command_line = spawn.get_command_line
orig_executable = spawn.get_executable()


def spawnv_passfds(path: bytes, args: list[str], passfds: Sequence[int]) -> int:
    if '-c' in args:
        idx = args.index('-c')
        patched_args = [spawn.get_executable(), '+runpy'] + list(args)[idx + 1 :]
    else:
        idx = args.index('--multiprocessing-fork')
        prog = 'from multiprocessing.spawn import spawn_main; spawn_main(%s)'
        prog %= ', '.join(str(item) for item in args[idx + 1 :])
        patched_args = [spawn.get_executable(), '+runpy', prog]
    return orig_spawn_passfds(os.fsencode(kitty_exe()), patched_args, passfds)


def get_command_line(**kwds: Any) -> list[str]:
    # On Windows the child is started directly from this command line, there
    # is no spawnv_passfds() to intercept
    prog = 'from multiprocessing.spawn import spawn_main; spawn_main(%s)'
    prog %= ', '.join('%s=%r' % item for item in kwds.items())
    return [spawn.get_executable(), '+runpy', prog, '--multiprocessing-fork']


def monkey_patch_multiprocessing() -> None:
    # Use kitty to run the worker process used by multiprocessing
    spawn.set_executable(kitty_exe())
    util.spawnv_passfds = spawnv_passfds  # type: ignore
    if sys.platform == 'win32':
        spawn.get_command_line = get_command_line


def unmonkey_patch_multiprocessing() -> None:
    spawn.set_executable(orig_executable)
    util.spawnv_passfds = orig_spawn_passfds
    spawn.get_command_line = orig_get_command_line


def test_spawn() -> None:
    monkey_patch_multiprocessing()
    import shutil
    import subprocess
    from queue import Empty

    try:
        from multiprocessing import get_context

        ctx = get_context('spawn')
        q = ctx.Queue()
        p = ctx.Process(target=q.put, args=('hello',))
        p.start()
        try:
            x = q.get(timeout=8)
        except Empty:
            p.join()
            rc = p.exitcode
            if rc == 0:
                raise TimeoutError('Timed out waiting for response from spawned process')
            if shutil.which('coredumpctl'):
                subprocess.run(['sh', '-c', 'echo bt | coredumpctl debug'])
            raise SystemExit(f'Spawned process exited with return code: {rc}')
        assert x == 'hello'
        p.join()
    finally:
        unmonkey_patch_multiprocessing()
