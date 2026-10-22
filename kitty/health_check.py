#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kitsu contributors

import argparse
import importlib
import json
import math
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class Probe:
    id: str
    required: bool = True
    prerequisites: tuple[str, ...] = ()
    remediation: str = 'Rebuild or reinstall this Kitsu build, then repeat the check.'


@dataclass
class Result:
    id: str
    required: bool
    prerequisites: tuple[str, ...]
    timeout_seconds: float
    status: str = 'BLOCKED'
    summary: str = 'Not run: run deadline or cancellation.'
    evidence: dict[str, Any] = field(default_factory=dict)
    remediation: str = ''
    duration_ms: int = 0


def exit_code(results: list[Result], *, internal_error: bool = False, cancelled: bool = False) -> int:
    if cancelled:
        return 130
    if internal_error:
        return 3
    if any(r.status == 'FAIL' for r in results):
        return 1
    if any(r.required and r.status in ('BLOCKED', 'SKIP') for r in results):
        return 2
    return 0


def sanitized(value: Any, secrets: tuple[str, ...] = ()) -> Any:
    if isinstance(value, str):
        for secret in secrets:
            if secret:
                value = value.replace(secret, '<redacted>')
        home = os.path.expanduser('~')
        for prefix in (home, home.replace('\\', '/'), home.replace('/', '\\')):
            if len(prefix) > 3:
                value = re.sub(re.escape(prefix), '~', value, flags=re.IGNORECASE)
        return re.sub(r'[\x00-\x1f\x7f-\x9f]', '?', value)[:4096]
    if isinstance(value, dict):
        return {sanitized(str(k), secrets): sanitized(v, secrets) for k, v in value.items()}
    if isinstance(value, (tuple, list)):
        return [sanitized(v, secrets) for v in value]
    return value


def render(report: dict[str, Any], json_output: bool) -> str:
    if json_output:
        return json.dumps(report, ensure_ascii=True, indent=2) + '\n'
    lines = [f'Kitsu Windows health check ({report["mode"]}) - exit {report["exit_code"]}']
    for r in report['results']:
        lines.append(f'{r["status"]:7} {r["id"]}: {r["summary"]}')
        if r['evidence']:
            lines.append('        ' + json.dumps(r['evidence'], ensure_ascii=True, sort_keys=True))
        if r['status'] != 'PASS' and r['remediation']:
            lines.append('        ' + r['remediation'])
    lines.append('Coverage is limited to the listed probes. WSL, physical input and hardware display checks are not performed.')
    return '\n'.join(lines) + '\n'


class Deadline:
    def __init__(self, seconds: float) -> None:
        self.end = time.monotonic() + seconds

    def remaining(self) -> float:
        ans = self.end - time.monotonic()
        if ans <= 0:
            raise TimeoutError
        return ans


class RunDirectory(tempfile.TemporaryDirectory):
    def cleanup(self) -> None:
        deadline = Deadline(5)
        while True:
            try:
                super().cleanup()
                return
            except PermissionError:
                deadline.remaining()
                time.sleep(0.05)


class WindowsJob:
    """A non-inheritable job owns the gated worker and all of its descendants."""

    def __init__(self) -> None:
        import ctypes as c
        from ctypes import wintypes as w

        class Basic(c.Structure):
            _fields_ = [
                ('user', c.c_int64),
                ('job', c.c_int64),
                ('flags', w.DWORD),
                ('min_ws', c.c_size_t),
                ('max_ws', c.c_size_t),
                ('active', w.DWORD),
                ('affinity', c.c_size_t),
                ('priority', w.DWORD),
                ('scheduling', w.DWORD),
            ]

        class IO(c.Structure):
            _fields_ = [(x, c.c_uint64) for x in ('read_ops', 'write_ops', 'other_ops', 'read_bytes', 'write_bytes', 'other_bytes')]

        class Limits(c.Structure):
            _fields_ = [
                ('basic', Basic),
                ('io', IO),
                ('process_memory', c.c_size_t),
                ('job_memory', c.c_size_t),
                ('peak_process', c.c_size_t),
                ('peak_job', c.c_size_t),
            ]

        self.api = c.WinDLL('kernel32', use_last_error=True)  # ty: ignore[unresolved-attribute]
        signatures = {
            'CreateJobObjectW': ([w.LPVOID, w.LPCWSTR], w.HANDLE),
            'SetInformationJobObject': ([w.HANDLE, c.c_int, w.LPVOID, w.DWORD], w.BOOL),
            'AssignProcessToJobObject': ([w.HANDLE, w.HANDLE], w.BOOL),
            'TerminateJobObject': ([w.HANDLE, w.UINT], w.BOOL),
            'QueryInformationJobObject': ([w.HANDLE, c.c_int, w.LPVOID, w.DWORD, w.LPVOID], w.BOOL),
            'CloseHandle': ([w.HANDLE], w.BOOL),
        }
        for name, (args, restype) in signatures.items():
            func = getattr(self.api, name)
            func.argtypes, func.restype = args, restype
        self.handle = self.api.CreateJobObjectW(None, None)
        if not self.handle:
            raise OSError('Cannot create process isolation job')
        limits = Limits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.api.SetInformationJobObject(self.handle, 9, c.byref(limits), c.sizeof(limits)):
            self.api.CloseHandle(self.handle)
            raise OSError('Cannot enable process-tree cleanup')

    def assign(self, process: subprocess.Popen[bytes]) -> None:
        if not self.api.AssignProcessToJobObject(self.handle, int(getattr(process, '_handle'))):
            raise OSError('Cannot isolate worker; no probes were started')

    def close(self) -> None:
        import ctypes as c

        if not self.handle:
            return
        try:
            if not self.api.TerminateJobObject(self.handle, 1):
                raise OSError('Cannot terminate owned process tree')
            deadline = Deadline(5)
            accounting = (c.c_uint32 * 12)()
            while True:
                if not self.api.QueryInformationJobObject(self.handle, 1, c.byref(accounting), c.sizeof(accounting), None):
                    raise OSError('Cannot verify owned process cleanup')
                if accounting[10] == 0:
                    break
                deadline.remaining()
                time.sleep(0.02)
        finally:
            self.api.CloseHandle(self.handle)
            self.handle = None


class Worker:
    def __init__(self, exe: str, code: str, args: list[str], root: Path, env: dict[str, str] | None = None) -> None:
        self.job = WindowsJob()
        self.process: subprocess.Popen[bytes] | None = None
        fd, gate = tempfile.mkstemp(dir=root, prefix='gate-')
        os.close(fd)
        os.unlink(gate)
        try:
            self.process = subprocess.Popen(
                [exe, '+runpy', 'from kitty.health_check import gated_worker; gated_worker()', gate, code, *args],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                env=env,
                cwd=root,
            )
            self.job.assign(self.process)
            Path(gate).touch()
        except BaseException:
            if self.process is not None:
                self.process.kill()
                self.process.wait(timeout=5)
            self.job.close()
            raise

    def close(self) -> None:
        try:
            self.job.close()
        finally:
            if self.process is not None:
                self.process.wait(timeout=5)
                if self.process.stdout:
                    self.process.stdout.close()


def gated_worker() -> None:
    gate, code, *args = sys.argv[1:]
    deadline = Deadline(15)
    while not os.path.isfile(gate):
        deadline.remaining()
        time.sleep(0.01)
    os.unlink(gate)
    sys.argv = ['health-check-worker', *args]
    exec(code)


def command(args: list[str], timeout: float = 10, **kwargs: Any) -> subprocess.CompletedProcess[bytes]:
    # Commands run inside a job-owned worker/session, or are leaf RC clients.
    return subprocess.run(args, timeout=timeout, capture_output=True, **kwargs)


QUICK = (
    Probe('runtime.identity'),
    Probe('runtime.imports'),
    Probe('runtime.ssl'),
    Probe('runtime.sqlite'),
    Probe('runtime.glfw'),
    Probe('runtime.fonts'),
    Probe('runtime.shader'),
    Probe('resources.terminfo'),
    Probe('resources.shell-integration'),
    Probe('resources.wsl'),
    Probe('runtime.conpty', False),
    Probe('installation.path', False, remediation='Use the matching Kitsu bin directory first in PATH; do not mix versions.'),
    Probe('installation.kind', False),
    Probe('installation.logging', False),
    Probe('installation.environment', False, remediation='Use a Windows shell with .COM and .EXE in PATHEXT to run native console helpers.'),
    Probe('configuration.parse', False, remediation='Fix configuration errors; generated includes are deliberately not executed.'),
)


def require(condition: Any, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def resources(root: Path, names: tuple[str, ...], *, lf: bool = False) -> dict[str, Any]:
    missing, crlf = [], []
    for name in names:
        path = root / name
        if not path.is_file() or not path.stat().st_size:
            missing.append(name)
        elif lf and b'\r\n' in path.read_bytes():
            crlf.append(name)
    return {'missing': missing, 'crlf': crlf, 'checked': len(names)}


def quick_probe(probe_id: str) -> tuple[str, str, dict[str, Any]]:
    from kitty.constants import (
        defconf,
        fonts_dir,
        glfw_path,
        kitten_exe,
        kitty_base_dir,
        kitty_exe,
        kitty_run_data,
        shell_integration_dir,
        slangc,
        str_version,
        terminfo_dir,
    )

    exe = kitty_exe()
    bindir = Path(exe).parent
    source = bool(kitty_run_data.get('from_source'))
    evidence: dict[str, Any] = {}
    if probe_id == 'runtime.identity':
        evidence = {
            'version': str_version,
            'architecture': platform.machine(),
            'bits': struct.calcsize('P') * 8,
            'executable': exe,
            'helper': kitten_exe(),
            'from_source': source,
        }
        cp = command([kitten_exe(), '--version'])
        require(cp.returncode == 0 and str_version in cp.stdout.decode('utf-8', 'replace'), 'Sibling kitten version does not match')
    elif probe_id == 'runtime.imports':
        for name in ('kitty.fast_data_types', 'kittens.transfer.rsync'):
            importlib.import_module(name)
        evidence = {'extensions_loaded': 2, 'python': platform.python_version()}
    elif probe_id == 'runtime.ssl':
        import ssl

        ctx = ssl.create_default_context()
        count = len(ctx.get_ca_certs())
        evidence = {'openssl': ssl.OPENSSL_VERSION, 'loaded_ca_certificates': count, 'network_used': False}
        if not count:
            return 'WARN', 'No eagerly loaded CA certificates; network trust was not tested.', evidence
    elif probe_id == 'runtime.sqlite':
        import sqlite3

        with sqlite3.connect(':memory:') as conn:
            require(conn.execute('select 6 * 7').fetchone() == (42,), 'SQLite query failed')
        evidence = {'version': sqlite3.sqlite_version, 'in_memory_query': True}
    elif probe_id == 'runtime.glfw':
        import ctypes

        path = glfw_path('win32')
        lib = ctypes.CDLL(path)
        require(getattr(lib, 'glfwInit', None), 'GLFW entry point missing')
        evidence = {'library': path, 'context_created': False}
    elif probe_id == 'runtime.fonts':
        from kitty.fonts.common import all_fonts_map

        fonts = all_fonts_map(True)
        require(any(fonts.values()), 'No monospace fonts enumerated')
        evidence = resources(Path(fonts_dir), ('SymbolsNerdFontMono-Regular.ttf',))
        require(not evidence['missing'], 'Bundled symbol font missing')
        evidence['font_enumeration'] = True
    elif probe_id == 'runtime.shader':
        from kitty.constants import read_kitty_resource, shaders_dir

        require(read_kitty_resource('graphics.slang', 'kitty.shaders'), 'Shader source missing')
        require((Path(shaders_dir) / 'graphics.spv').is_file(), 'Compiled graphics shader missing')
        compiler = slangc() if source else (str(bindir / 'slangc.exe'),)
        if not source:
            evidence = resources(bindir, ('slangc.exe', 'slang-compiler.dll', 'slang-glsl-module.dll', 'slang-glslang.dll'))
            if evidence['missing']:
                return 'FAIL', 'Bundled shader compiler/runtime missing.', evidence
        cp = command([*compiler, '-version'])
        require(cp.returncode == 0, 'Shader compiler failed')
        src = b'[shader("vertex")] float4 main(uint id : SV_VertexID) : SV_Position { return float4(id, 1, 0, 1); }'
        cp = command([*compiler, '-lang', 'slang', '-entry', 'main', '-stage', 'vertex', '-target', 'glsl', '-o', 'test.glsl', '--', '-'], input=src)
        require(cp.returncode == 0 and Path('test.glsl').stat().st_size, 'Shader compilation failed')
        evidence = {'compiler': compiler[0], 'compiled_test_shader': True}
    elif probe_id.startswith('resources.'):
        if probe_id == 'resources.terminfo':
            root, names, lf = Path(terminfo_dir), ('x/xterm-kitty', '78/xterm-kitty'), False
            if source:
                names = ('x/xterm-kitty',)
        elif probe_id == 'resources.wsl':
            root, names, lf = Path(kitty_base_dir) / 'windows/wsl', ('install-kitten.sh', 'kitten-linux-amd64', 'kitten-linux-arm64'), False
            if source:
                return 'SKIP', 'WSL release payload is only required in packaged builds.', {'wsl_executed': False}
        else:
            root, names, lf = (
                Path(shell_integration_dir),
                (
                    'bash/kitty.bash',
                    'zsh/kitty-integration',
                    'fish/vendor_conf.d/kitty-shell-integration.fish',
                    'powershell/kitty-integration.ps1',
                ),
                True,
            )
        evidence = resources(root, names, lf=lf)
        if evidence['missing'] or evidence['crlf']:
            return 'FAIL', 'Resources missing, empty, or have incorrect line endings.', evidence
    elif probe_id == 'runtime.conpty':
        evidence = resources(bindir, ('conpty.dll', 'OpenConsole.exe'))
        return ('WARN' if evidence['missing'] else 'PASS'), 'Bundled provider availability only; native checks exercise the selected provider.', evidence
    elif probe_id == 'installation.path':
        for name in ('kitsu.exe', 'kitsu.com', 'kitty.exe', 'kitty.com', 'kitten.exe'):
            found = shutil.which(name)
            evidence[name] = found
            if found and Path(found).resolve() != (bindir / name).resolve():
                return 'WARN', 'PATH selects a different installation.', evidence
        if any(v is None for v in evidence.values()):
            return 'WARN', 'One or more command names are not in PATH.', evidence
    elif probe_id == 'installation.kind':
        from kitty.update import inno_install_prefix

        evidence = {
            'kind': 'source' if source else ('installer' if inno_install_prefix() else 'portable'),
            'bin_writable': os.access(bindir, os.W_OK),
            'windows': platform.version(),
        }
    elif probe_id == 'installation.logging':
        from kitty.fast_data_types import windows_log_path

        path = windows_log_path()
        evidence = {'gui_log': path, 'exists': Path(path).is_file(), 'rotation': 'At startup, at 1 MiB; one .1 backup.'}
    elif probe_id == 'installation.environment':
        extensions = os.environ.get('PATHEXT', '').upper().split(';')
        evidence = {'pathext_com': '.COM' in extensions, 'pathext_exe': '.EXE' in extensions}
        if not all(evidence.values()):
            return 'WARN', 'PATHEXT is missing .COM or .EXE; PowerShell may not run native console helpers.', evidence
    elif probe_id == 'configuration.parse':
        from kitty.conf import utils
        from kitty.config import parse_config

        if not Path(defconf).exists():
            return 'PASS', 'No user configuration; defaults apply.', {'files_parsed': 0}
        bad: list[utils.BadLine] = []
        messages: list[str] = []
        original = getattr(utils.log_error, 'redirect', None)

        # The parser reports unknown keys and skipped includes through log_error.
        def record_warning(*a: Any) -> None:
            messages.append('warning')

        setattr(utils.log_error, 'redirect', record_warning)
        try:
            with open(defconf, encoding='utf-8') as f:
                parse_config(f, accumulate_bad_lines=bad, allow_geninclude=False)
        finally:
            if original is None:
                delattr(utils.log_error, 'redirect')
            else:
                setattr(utils.log_error, 'redirect', original)
        evidence = {'invalid_lines': len(bad), 'parser_warnings': len(messages), 'generated_includes_executed': False}
        if bad or messages:
            return 'WARN', 'Configuration has errors/warnings or excluded generated includes; values withheld.', evidence
    else:
        raise ValueError('Unknown probe')
    return 'PASS', 'Check completed.', evidence


def quick_worker() -> None:
    try:
        result = quick_probe(sys.argv[1])
    except AssertionError as err:
        result = ('FAIL', str(err), {})
    except Exception as err:
        result = ('FAIL', f'Probe failed ({type(err).__name__}); raw exception withheld.', {})
    print(json.dumps(result, ensure_ascii=True))


def positive_seconds(value: str) -> float:
    ans = float(value)
    if not math.isfinite(ans) or ans <= 0 or ans > 3600:
        raise argparse.ArgumentTypeError('must be finite, greater than zero and at most 3600 seconds')
    return ans


def main(args: list[str]) -> None:
    parser = argparse.ArgumentParser(prog='kitsu +health-check', description='Read-only Windows diagnostics; native mode opens an isolated test window.')
    parser.add_argument('--mode', choices=('quick', 'native'), default='quick', help='native includes quick diagnostics')
    parser.add_argument('--format', choices=('text', 'json'), default='text')
    parser.add_argument('--output', type=Path, help='also write the report to this file (no automatic upload)')
    parser.add_argument('--timeout', type=positive_seconds, default=180, help='overall probe deadline in seconds (default: 180)')
    parser.add_argument('--probe-timeout', type=positive_seconds, default=20, help='individual probe deadline (default: 20)')
    opts = parser.parse_args(args[1:])
    from kitty.constants import kitty_exe

    from .health_check_native import NATIVE, NativeSession

    specs = list(QUICK)
    # Source trees need not contain downloaded Linux payloads.
    from kitty.constants import kitty_run_data

    if kitty_run_data.get('from_source'):
        specs = [Probe(p.id, False, p.prerequisites, p.remediation) if p.id == 'resources.wsl' else p for p in specs]
    if opts.mode == 'native':
        specs.extend(NATIVE)
    results = [Result(p.id, p.required, p.prerequisites, opts.probe_timeout, remediation=p.remediation) for p in specs]
    by_id = {r.id: r for r in results}
    deadline = Deadline(opts.timeout)
    internal, cancelled = False, False
    secrets: tuple[str, ...] = ()
    session: NativeSession | None = None
    try:
        if sys.platform != 'win32':
            for r in results:
                r.summary = 'Windows-only health-check runner.'
        else:
            with RunDirectory(prefix='kitty-health-') as tmp:
                root, exe = Path(tmp), kitty_exe()
                session = NativeSession(exe, root)
                secrets = (session.password,)
                try:
                    for r in results:
                        try:
                            budget = min(opts.probe_timeout, deadline.remaining())
                        except TimeoutError:
                            break
                        if any(by_id[p].status not in ('PASS', 'WARN') for p in r.prerequisites):
                            r.summary = 'Prerequisite did not pass.'
                            continue
                        start = time.monotonic()
                        try:
                            if r.id.startswith('native.'):
                                r.status, r.summary, r.evidence = session.probe(r.id, Deadline(budget))
                            else:
                                worker = Worker(exe, 'from kitty.health_check import quick_worker; quick_worker()', [r.id], root)
                                try:
                                    assert worker.process is not None
                                    data, _ = worker.process.communicate(timeout=max(0.01, budget - (time.monotonic() - start)))
                                    if worker.process.returncode:
                                        r.status, r.summary = 'FAIL', 'Diagnostic worker could not initialize; use the PowerShell bootstrap.'
                                    else:
                                        r.status, r.summary, r.evidence = json.loads(data)
                                finally:
                                    worker.close()
                        except (TimeoutError, subprocess.TimeoutExpired):
                            r.status, r.summary = 'BLOCKED', 'Probe timed out; remaining independent checks will continue.'
                        except AssertionError as err:
                            r.status, r.summary = 'FAIL', str(err)
                        except OSError as err:
                            r.status, r.summary = 'BLOCKED', f'Native operation unavailable ({type(err).__name__}); raw exception withheld.'
                        except Exception as err:
                            internal = True
                            r.status, r.summary = 'BLOCKED', f'Runner error ({type(err).__name__}); raw exception withheld.'
                        r.duration_ms = round((time.monotonic() - start) * 1000)
                finally:
                    session.close()
    except KeyboardInterrupt:
        cancelled = True
    except Exception:
        internal = True
        print('Health-check runner/cleanup failed; raw exception withheld.', file=sys.stderr)
    code = exit_code(results, internal_error=internal, cancelled=cancelled)
    report = sanitized(
        {
            'schema_version': 1,
            'mode': opts.mode,
            'exit_code': code,
            'cancelled': cancelled,
            'internal_error': internal,
            'results': [asdict(r) for r in results],
            'excluded_coverage': ['WSL sessions', 'physical input', 'hardware display/GPU'],
        },
        secrets,
    )
    output = render(report, opts.format == 'json')
    if opts.output:
        try:
            opts.output.write_text(output, encoding='utf-8')
        except OSError:
            print('Cannot write health-check output file.', file=sys.stderr)
            report['exit_code'] = code = 3
            output = render(report, opts.format == 'json')
    sys.stdout.write(output)
    raise SystemExit(code)
