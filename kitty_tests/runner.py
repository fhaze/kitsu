#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kitsu contributors

import os
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from tempfile import TemporaryFile

from .main import run_serial_tests


class SerialWorkerFixture(unittest.TestCase):
    # These are selected explicitly in child processes, never by discovery.
    def succeed(self):
        pass

    def fail_test(self):
        self.fail('intentional worker failure')

    def exit_early(self):
        os._exit(0)


@unittest.skipUnless(sys.platform == 'win32', 'Windows subprocess test runner')
class WindowsRunnerTest(unittest.TestCase):
    def test_serial_worker_completion(self):
        for method, expected in (('succeed', True), ('fail_test', False), ('exit_early', False)):
            with self.subTest(method=method), TemporaryFile(mode='w+', encoding='utf-8') as log:
                with redirect_stdout(log), redirect_stderr(log):
                    result = run_serial_tests([SerialWorkerFixture(method)], 2)
                log.seek(0)
                output = log.read()
                self.assertEqual(result, expected, output)
                if method == 'exit_early':
                    self.assertIn('exited with status 0 before completing its tests', output)
