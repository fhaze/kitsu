#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

import os
import tempfile
from pathlib import Path
from unittest.mock import patch

from .base import BaseTest
from .main import find_testable_go_packages


class TestRunner(BaseTest):
    def test_go_package_paths(self):
        with tempfile.TemporaryDirectory() as root:
            package = Path(root) / 'tools' / 'example'
            package.mkdir(parents=True)
            (package / 'example_test.go').write_text('package example\nfunc TestExample(t *testing.T) {}\n')
            real_walk = os.walk
            with patch('kitty_tests.main.os.getcwd', return_value=root), patch('kitty_tests.main.os.walk', side_effect=lambda _: real_walk(root)):
                packages, functions = find_testable_go_packages()
            self.ae(packages, {'tools/example'})
            self.ae(functions, {'Example': ['tools/example']})
