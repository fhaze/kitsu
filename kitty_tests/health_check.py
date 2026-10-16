#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kitty contributors

import json
import os
import subprocess
import sys
import tempfile
import unittest
from dataclasses import asdict
from pathlib import Path
from unittest.mock import patch

from kitty.health_check import QUICK, Deadline, Result, RunDirectory, Worker, exit_code, main, positive_seconds, quick_probe, render, resources, sanitized
from kitty.health_check_native import NATIVE, RC_ACTIONS, RC_PASSWORD_ENV, NativeSession, colored_fixture_present, gui_worker, ps_quote

from .base import BaseTest


class TestHealthCheck(BaseTest):
    def result(self, status, required=True):
        return Result('test', required, (), 1, status, 'summary', {'test': True}, 'repair')

    def test_health_exit_codes(self):
        for status, required, code in (
            ('PASS', True, 0),
            ('WARN', True, 0),
            ('FAIL', False, 1),
            ('FAIL', True, 1),
            ('BLOCKED', True, 2),
            ('SKIP', True, 2),
            ('BLOCKED', False, 0),
            ('SKIP', False, 0),
        ):
            self.ae(exit_code([self.result(status, required)]), code)
        mixed = [self.result('BLOCKED'), self.result('FAIL')]
        self.ae(exit_code(mixed), 1)
        self.ae(exit_code(mixed, internal_error=True), 3)
        self.ae(exit_code(mixed, internal_error=True, cancelled=True), 130)

    def test_health_probe_policy(self):
        probes = QUICK + NATIVE
        ids = [p.id for p in probes]
        self.ae(len(ids), len(set(ids)))
        seen = set()
        for probe in probes:
            self.assertTrue(set(probe.prerequisites) <= seen)
            seen.add(probe.id)
        self.assertTrue(next(p for p in QUICK if p.id == 'resources.wsl').required)
        self.assertIn('native.authentication', next(p for p in NATIVE if p.id == 'native.shells').prerequisites)

    def test_health_formatter_privacy(self):
        home = os.path.expanduser('~')
        private = self.result('FAIL')
        private.evidence = {'path': home + '/private', 'nested': ['TOKEN', 'hello\x1b[31m\nworld']}
        report = sanitized({'mode': 'quick', 'exit_code': 1, 'results': [asdict(private)]}, ('TOKEN',))
        encoded = render(report, True)
        self.assertNotIn('TOKEN', encoded)
        self.assertNotIn(home, encoded)
        self.assertNotIn('\x1b', encoded)
        parsed = json.loads(encoded)
        self.ae(parsed['results'][0]['evidence']['nested'][0], '<redacted>')
        text = render(report, False)
        self.assertIn('FAIL', text)
        self.assertIn('repair', text)
        self.assertNotIn('\x1b', text)

    def test_health_resources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'empty').touch()
            (root / 'bad').write_bytes(b'abc\r\n')
            (root / 'good').write_bytes(b'abc\n')
            result = resources(root, ('missing', 'empty', 'bad', 'good'), lf=True)
            self.ae(result, {'missing': ['missing', 'empty'], 'crlf': ['bad'], 'checked': 4})

    def test_health_resource_failure_evidence(self):
        with tempfile.TemporaryDirectory() as tmp, patch('kitty.constants.terminfo_dir', tmp), patch('kitty.constants.kitty_run_data', {}):
            status, _, evidence = quick_probe('resources.terminfo')
            self.ae(status, 'FAIL')
            self.ae(evidence['missing'], ['x/xterm-kitty', '78/xterm-kitty'])

    def test_health_packaged_shader_never_uses_external_compiler(self):
        with tempfile.TemporaryDirectory() as tmp, patch('kitty.constants.kitty_run_data', {}):
            exe = str(Path(tmp) / 'kitty.exe')
            with patch('kitty.constants.kitty_exe', return_value=exe), patch('kitty.constants.slangc') as external:
                status, _, evidence = quick_probe('runtime.shader')
            self.ae(status, 'FAIL')
            self.assertIn('slangc.exe', evidence['missing'])
            external.assert_not_called()

    def test_health_config_does_not_execute(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'generated.py').write_text('raise Exception("must never run")', encoding='utf-8')
            (root / 'included.conf').write_text('geninclude generated.py\nfont_size invalid-secret-value\n', encoding='utf-8')
            config = root / 'kitty.conf'
            config.write_text('include included.conf\nunknown_key secret-config-value\n', encoding='utf-8')
            with patch('kitty.constants.defconf', str(config)), patch('kitty.conf.utils.geninclude') as gen:
                status, summary, evidence = quick_probe('configuration.parse')
                gen.assert_not_called()
            self.ae(status, 'WARN')
            self.ae(evidence['generated_includes_executed'], False)
            self.assertGreater(evidence['invalid_lines'], 0)
            self.assertGreater(evidence['parser_warnings'], 0)
            self.assertNotIn('secret', json.dumps((summary, evidence)))

    def test_health_path_shadowing(self):
        from kitty.constants import kitten_exe, kitty_exe

        bindir = Path(kitty_exe()).parent
        with patch('shutil.which', side_effect=lambda name: str(bindir / name)):
            self.ae(quick_probe('installation.path')[0], 'PASS')
        with patch('shutil.which', return_value=str(Path(tempfile.gettempdir()) / 'other' / Path(kitten_exe()).name)):
            self.ae(quick_probe('installation.path')[0], 'WARN')

    @unittest.skipUnless(sys.platform == 'win32', 'Windows executable paths')
    def test_health_preserves_windows_executable_path(self):
        from kitty.utils import which

        path = r'C:\Windows\System32\cmd.exe'
        with patch('shutil.which') as search:
            self.ae(which(path), path)
            search.assert_not_called()

    def test_health_deadlines(self):
        with patch('time.monotonic', side_effect=[10, 10.1, 10.5]):
            deadline = Deadline(0.5)
            self.assertAlmostEqual(deadline.remaining(), 0.4)
            self.assertRaises(TimeoutError, deadline.remaining)
        for value in ('nan', 'inf', '-1', '0', '3601'):
            import argparse

            self.assertRaises(argparse.ArgumentTypeError, positive_seconds, value)

    def test_health_pathext_diagnostics(self):
        with patch.dict(os.environ, {'PATHEXT': '.COM;.EXE;.BAT;.CMD'}):
            self.ae(quick_probe('installation.environment')[0], 'PASS')
        with patch.dict(os.environ, {'PATHEXT': '.EXE'}):
            status, _, evidence = quick_probe('installation.environment')
            self.ae(status, 'WARN')
            self.ae(evidence, {'pathext_com': False, 'pathext_exe': True})

    def test_health_pixels_and_quoting(self):
        magenta, green = b'\xff\x00\xff\xff', b'\x00\xff\x00\xff'
        pixels = (magenta * 30 + green * 30 + b'\x00\x00\x00\xff' * 120) * 60
        self.assertTrue(colored_fixture_present(pixels, 180, 60))
        self.assertFalse(colored_fixture_present(magenta * 3600, 60, 60))
        self.assertFalse(colored_fixture_present(b'bad', 60, 60))
        self.ae(ps_quote("a b'c"), "'a b''c'")

    def run_cli(self, args, worker=None):
        import io

        stdout = io.StringIO()
        with patch('sys.stdout', stdout), patch('kitty.health_check.Worker', worker) if worker else patch('sys.platform', 'unsupported'):
            with self.assertRaises(SystemExit) as raised:
                main(['health-check', '--format=json', *args])
        report = json.loads(stdout.getvalue())
        self.ae(report['exit_code'], raised.exception.code)
        return report

    def test_health_unsupported_partial_report(self):
        report = self.run_cli([])
        self.ae(report['exit_code'], 2)
        self.ae(len(report['results']), len(QUICK))
        self.assertTrue(all(r['status'] == 'BLOCKED' for r in report['results']))

    @unittest.skipUnless(sys.platform == 'win32', 'Windows process jobs')
    def test_health_worker_timeout_cleanup(self):
        from kitty.constants import kitty_exe

        with tempfile.TemporaryDirectory() as tmp:
            worker = Worker(kitty_exe(), 'import time; time.sleep(120)', [], Path(tmp))
            try:
                self.assertRaises(subprocess.TimeoutExpired, worker.process.communicate, timeout=0.15)
            finally:
                worker.close()
            self.assertIsNotNone(worker.process.poll())
            worker.close()

    @unittest.skipUnless(sys.platform == 'win32', 'Windows process jobs')
    def test_health_timeout_partial_report(self):
        class TimedOut:
            def __init__(self, *a):
                self.process = self

            def communicate(self, **kw):
                raise subprocess.TimeoutExpired('redacted', 0.1)

            def close(self):
                pass

        report = self.run_cli([], TimedOut)
        self.ae(report['exit_code'], 2)
        self.assertTrue(all(r['status'] == 'BLOCKED' for r in report['results']))

    @unittest.skipUnless(sys.platform == 'win32', 'Windows process jobs')
    def test_health_cancellation_partial_report(self):
        class Cancelled:
            def __init__(self, *a):
                raise KeyboardInterrupt

        report = self.run_cli([], Cancelled)
        self.ae(report['exit_code'], 130)
        self.assertTrue(report['cancelled'])
        self.assertTrue(all(r['status'] == 'BLOCKED' for r in report['results']))

    def test_health_refuses_public_rc_endpoint(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = NativeSession('kitty.exe', Path(tmp))
            session.endpoint = 'tcp:0.0.0.0:1234'
            self.assertRaises(AssertionError, session.rc, ['ls'], Deadline(1))

    def test_health_password_stays_out_of_files_and_process_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            session = NativeSession('kitty.exe', root)
            self.ae(list(root.iterdir()), [])
            state = {
                'endpoint': 'tcp:127.0.0.1:1234',
                'window_id': '1',
                'public_key': 'public',
                'size': [80, 24],
                'credential_in_environment': False,
            }
            with patch('kitty.health_check_native.Worker') as worker, patch.object(session, 'state', return_value=state):
                session.probe('native.startup', Deadline(1))
            args = worker.call_args.args
            self.assertNotIn(session.password, repr(args[:-1]))
            self.ae(args[-1][RC_PASSWORD_ENV], session.password)
            self.assertNotIn(RC_PASSWORD_ENV, session.env)
            self.ae([p.name for p in root.iterdir()], ['health.conf'])
            self.assertNotIn(session.password, (root / 'health.conf').read_text(encoding='utf-8'))

    def test_health_gui_worker_consumes_credential_before_main(self):
        password = 'health_test_credential'
        argv = ['worker', '--config', 'health.conf']
        data = {'cli_flags': {}}
        with patch.dict(os.environ, {RC_PASSWORD_ENV: password}), patch('sys.argv', argv), patch('sys.kitty_run_data', data, create=True):
            with patch('kitty.main.main') as app:
                gui_worker()
                app.assert_called_once_with()
                self.assertNotIn(RC_PASSWORD_ENV, os.environ)
                self.assertNotIn('cli_flags', data)
                self.ae(argv, ['kitty', '--override', f'remote_control_password={password} {RC_ACTIONS}', '--config', 'health.conf'])

    def test_health_remote_password_uses_stdin(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = NativeSession('kitty.exe', Path(tmp))
            session.endpoint = 'tcp:127.0.0.1:1234'
            cp = subprocess.CompletedProcess([], 0, b'[]', b'')
            with patch('kitty.health_check_native.command', return_value=cp) as run:
                session.rc(['ls'], Deadline(1))
                args, kwargs = run.call_args.args[0], run.call_args.kwargs
                self.assertNotIn(session.password, repr(args))
                self.ae(args[4:7], ['--use-password=always', '--password-file', '-'])
                self.ae(kwargs['input'], session.password.encode('utf-8'))
                self.assertNotIn(RC_PASSWORD_ENV, kwargs['env'])
                session.rc(['ls'], Deadline(1), authenticated=False)
                self.ae(run.call_args.kwargs['input'], b'')
                self.assertIn('--use-password=never', run.call_args.args[0])

    def test_health_refuses_credential_leak_to_terminal_child(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = NativeSession('kitty.exe', Path(tmp))
            with patch('kitty.health_check_native.Worker'), patch.object(session, 'state', return_value={'credential_in_environment': True}):
                self.assertRaises(AssertionError, session.probe, 'native.startup', Deadline(1))

    @unittest.skipUnless(sys.platform == 'win32', 'Windows console shim')
    def test_health_console_shim(self):
        from kitty.constants import kitty_exe

        with RunDirectory() as tmp:
            session = NativeSession(kitty_exe(), Path(tmp))
            try:
                status, _, evidence = session.probe('native.console', Deadline(20))
            finally:
                session.close()
            self.ae(status, 'PASS')
            self.ae(evidence['exit_code'], 17)
