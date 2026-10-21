#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kitsu contributors

import os
import subprocess
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

from kitty.boss import Boss
from kitty.constants import is_windows, kitty_exe
from kitty.options.types import defaults
from kitty.utils import get_editor_from_env_vars, is_windows_gui_executable

from .base import BaseTest


@unittest.skipUnless(is_windows, 'Native Windows defaults')
class WindowsDefaultsTest(BaseTest):
    def test_windows_editor_fallback(self):
        with patch('kitty.child.default_env', return_value={}), patch('kitty.utils.read_shell_environment', return_value={}):
            with patch('kitty.utils.which', side_effect=lambda exe, **kw: exe if exe == 'notepad.exe' else None):
                self.ae(get_editor_from_env_vars(), ['notepad.exe'])
            for candidate, expected in (('edit.exe', ['edit.exe']), ('nvim', ['nvim']), ('code', ['code', '--wait'])):
                with self.subTest(candidate=candidate), patch(
                    'kitty.utils.which', side_effect=lambda exe, **kw: exe if exe in (candidate, 'notepad.exe') else None
                ):
                    self.ae(get_editor_from_env_vars(), expected)
        with patch('kitty.child.default_env', return_value={'VISUAL': 'custom-editor --wait'}), patch('kitty.utils.which', return_value='custom-editor'):
            self.ae(get_editor_from_env_vars(), ['custom-editor', '--wait'])

    def test_windows_editor_launch(self):
        boss = SimpleNamespace(show_error=Mock(), new_os_window=Mock())
        path = os.path.abspath('config with spaces.conf')
        system32 = os.path.join(os.environ['SystemRoot'], 'System32')
        notepad = os.path.join(system32, 'notepad.exe')
        cmd = os.path.join(system32, 'cmd.exe')
        self.assertTrue(is_windows_gui_executable(notepad))
        self.assertFalse(is_windows_gui_executable(cmd))
        self.assertFalse(is_windows_gui_executable(path))
        with patch('kitty.boss.get_options'), patch('kitty.boss.get_editor', return_value=[notepad]), patch('kitty.boss.subprocess.Popen') as popen:
            Boss.edit_file(boss, path)
            popen.assert_called_once_with([notepad, path], creationflags=subprocess.DETACHED_PROCESS)
            boss.new_os_window.assert_not_called()
            popen.side_effect = OSError(5, 'Access denied')
            Boss.edit_file(boss, path)
            self.assertIn('Access denied', boss.show_error.call_args.args[1])
        with patch('kitty.boss.get_options'), patch('kitty.boss.get_editor', return_value=[cmd]):
            Boss.edit_file(boss, path)
            boss.new_os_window.assert_called_once_with(cmd, path)
        with patch('kitty.boss.get_options'), patch('kitty.boss.get_editor', return_value=['missing-editor']), patch('kitty.boss.which', return_value=None):
            Boss.edit_file(boss, path)
            self.assertIn('missing-editor', boss.show_error.call_args.args[1])
            self.assertIn(path, boss.show_error.call_args.args[1])

    def test_windows_scrollback_pager(self):
        window = SimpleNamespace(id=1, cwd_of_child=os.getcwd(), screen=SimpleNamespace(cursor=SimpleNamespace(y=4, x=2)))
        tab = SimpleNamespace(new_special_window=Mock())
        boss = SimpleNamespace(active_tab=tab, active_window=window, show_error=Mock())
        bundled = os.path.join(os.path.dirname(kitty_exe()), 'less.exe')
        opts = defaults._replace()
        with patch('kitty.boss.get_options', return_value=opts), patch('kitty.boss.which', return_value=None):
            Boss.display_scrollback(boss, window, '\x1b[31mhistory\x1b[m', 42)
            spec = tab.new_special_window.call_args.args[0]
            self.ae(spec.cmd, [bundled, '--chop-long-lines', '--RAW-CONTROL-CHARS', '+42', '-+F'])
            self.ae(spec.stdin, b'\x1b[31mhistory\x1b[m')
            opts.scrollback_pager = ('missing-pager', '--invalid-less-option')
            Boss.display_scrollback(boss, window, 'history', 7)
            self.ae(tab.new_special_window.call_args.args[0].cmd, [bundled, '--chop-long-lines', '--RAW-CONTROL-CHARS', '+7', '-+F'])
            opts.scrollback_pager = (os.path.abspath('LESS.EXE'), '+INPUT_LINE_NUMBER', 'CURSOR_LINE', 'CURSOR_COLUMN')
            Boss.display_scrollback(boss, window, 'history', 9)
            self.ae(tab.new_special_window.call_args.args[0].cmd, [opts.scrollback_pager[0], '+9', '5', '3', '-+F'])
            tab.new_special_window.side_effect = OSError(2, 'Pager launch failed')
            self.assertIsNone(Boss.display_scrollback(boss, window, 'history'))
            self.assertIn('Pager launch failed', boss.show_error.call_args.args[1])

    def test_windows_packaged_less(self):
        pager = os.path.join(os.path.dirname(kitty_exe()), 'less.exe')
        self.assertTrue(os.path.isfile(pager), 'The Windows package must ship its default pager')
        env = {'SystemRoot': os.environ['SystemRoot'], 'PATH': os.path.join(os.environ['SystemRoot'], 'System32')}
        cp = subprocess.run([pager, '--version'], env=env, capture_output=True, timeout=30)
        self.ae(cp.returncode, 0, cp.stderr)
        self.assertIn(b'less 685', cp.stdout)
        data = b'\x1b[31mhistory\x1b[m\n' * 100
        cp = subprocess.run([pager, '--chop-long-lines', '--RAW-CONTROL-CHARS', '+42', '-+F'], input=data, env=env, capture_output=True, timeout=30)
        self.ae(cp.returncode, 0, cp.stderr)
        self.ae(cp.stdout.replace(b'\r\n', b'\n'), data)
