#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kitsu contributors

import os
import re
import subprocess
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

from kitty.boss import Boss
from kitty.constants import is_windows, kitty_exe
from kitty.options.types import defaults
from kitty.utils import (
    cmdline_for_hold,
    cmdline_split,
    get_editor,
    get_editor_from_env_vars,
    is_windows_gui_executable,
    reload_conf_in_all_kitties,
    resolved_shell,
)

from .base import BaseTest


@unittest.skipUnless(is_windows, 'Native Windows defaults')
class WindowsDefaultsTest(BaseTest):
    def test_windows_debug_config(self):
        from kitty.debug_config import IssueData, compositor_name, debug_config

        env = {'COMSPEC': r'C:\Windows\System32\cmd.exe', 'PATHEXT': '.COM;.EXE;.BAT', 'WSLENV': 'TERM', 'SHELL': 'pwsh.exe'}
        with (
            patch.object(os, 'uname', create=True, side_effect=AttributeError('os.uname is unavailable on Windows')),
            patch.dict(os.environ, env),
            patch('kitty.debug_config.current_fonts', return_value={}),
            patch('kitty.debug_config.glfw_get_system_color_theme', return_value='light'),
            patch('kitty.fast_data_types.get_boss', return_value=None),
        ):
            self.ae(IssueData().s, 'Windows')
            self.ae(compositor_name(), 'Win32')
            for channel in (None, 'private-channel-address-and-token'):
                with self.subTest(channel=channel), patch.dict(os.environ):
                    os.environ.pop('KITTY_DCS_CHANNEL', None)
                    if channel is not None:
                        os.environ['KITTY_DCS_CHANNEL'] = channel
                    report = re.sub(r'\x1b\[[0-9;]*m', '', debug_config())
                    self.assertIn('Running under: Win32', report)
                    self.assertNotIn('X11', report)
                    self.assertIn('Windows version:', report)
                    self.assertRegex(report, r'Windows build: \d+\.\d+\.\d+\.\d+')
                    self.assertIn('ConPTY:', report)
                    self.assertIn('GUI log:', report)
                    self.assertIn('GLFW not initialized', report)
                    for name, value in env.items():
                        self.assertRegex(report, re.escape(name) + r'\s+' + re.escape(value))
                    self.assertRegex(report, r'KITTY_DCS_CHANNEL\s+' + ('not set' if channel is None else 'set'))
                    self.assertNotIn('private-channel-address-and-token', report)

    def test_windows_debug_info(self):
        from kitty.debug_config import windows_debug_info

        metrics = {'width': 800, 'height': 600, 'xdpi': 144.0, 'ydpi': 144.0, 'xscale': 1.5, 'yscale': 1.5}
        with (
            patch('winreg.OpenKey', side_effect=PermissionError('registry unavailable')),
            patch('kitty.fast_data_types.get_boss', return_value=SimpleNamespace(os_window_map={1: None})),
            patch('kitty.fast_data_types.get_os_window_size', return_value=metrics),
            patch('kitty.fast_data_types.glfw_get_monitor_names', return_value=((r'\\.\DISPLAY1', 'Test monitor'),)),
            patch('kitty.fast_data_types.glfw_get_monitor_workarea', return_value=((0, 0, 1920, 1080),)),
        ):
            for provider in ('system', 'bundled'):
                with self.subTest(provider=provider), patch('kitty.fast_data_types.win32_conpty_provider', return_value=provider):
                    lines = []
                    windows_debug_info(lambda *args: lines.append(' '.join(map(str, args))))
                    report = '\n'.join(lines)
                    self.assertRegex(report, r'Windows build: \d+\.\d+\.\d+\n')
                    self.assertIn(f'ConPTY: {provider}', report)
                    self.assertIn(r'\\.\DISPLAY1 (Test monitor): work area (0, 0, 1920, 1080)', report)
                    self.assertIn('Window 1: 800x600, DPI 144.0x144.0, scale 1.5x1.5', report)

    def test_windows_query_os_name(self):
        from kitty.terminfo import get_capabilities

        from .base import parse_bytes

        names = ('query-os-name', 'query-os_name', 'TN')
        query = ';'.join(name.encode().hex() for name in names)
        expected = [f'1+r{name.encode().hex()}={value.encode().hex()}' for name, value in zip(names, ('Windows', 'Windows', defaults.term))]
        with patch.object(os, 'uname', create=True, side_effect=AttributeError('os.uname is unavailable on Windows')):
            self.ae(list(get_capabilities(query, defaults)), expected)
            screen = self.create_screen()
            parse_bytes(screen, f'\x1bP+q{query}\x1b\\'.encode())
            self.ae(screen.callbacks.wtcbuf, ''.join(expected).encode())

    def test_windows_reload_all(self):
        with patch('kitty.utils.kitten_exe', return_value='kitten.exe'), patch('subprocess.run') as run:
            reload_conf_in_all_kitties()
            run.assert_called_once_with(['kitten.exe', '__reload_config__'], check=True, creationflags=subprocess.CREATE_NO_WINDOW)
            run.side_effect = subprocess.CalledProcessError(1, 'kitten.exe')
            with self.assertRaises(subprocess.CalledProcessError):
                reload_conf_in_all_kitties()

    def test_windows_config_command_lines(self):
        from kitty.config import load_config
        from kitty.launch import LaunchSpec
        from kitty.options.utils import parse_key_action
        from kitty.session import parse_session

        command = r'"C:\Program Files\tools\app.exe" C:\Users\x\a.txt "\\server\share\two words"'
        expected = [r'C:\Program Files\tools\app.exe', r'C:\Users\x\a.txt', r'\\server\share\two words']
        bad_lines = []
        opts = load_config(
            overrides=(
                f'{key} {command}'
                for key in (
                    'shell',
                    'editor',
                    'command_on_bell',
                    'scrollback_pager',
                    'open_url_with',
                )
            ),
            accumulate_bad_lines=bad_lines,
        )
        self.ae(bad_lines, [])
        self.ae(resolved_shell(opts), expected)
        self.ae(get_editor(opts), expected)
        for key in ('command_on_bell', 'scrollback_pager', 'open_url_with'):
            self.ae(list(getattr(opts, key)), expected)
        for key in ('VISUAL', 'EDITOR'):
            with patch('kitty.child.default_env', return_value={key: command}), patch('kitty.utils.which', return_value=expected[0]):
                self.ae(get_editor_from_env_vars(), expected)
        for prefix in ('', r'--cwd=C:\Users\x ', r'--cwd="\\server\share\two words" '):
            action = parse_key_action('launch ' + prefix + command)
            sessions = tuple(parse_session('launch ' + prefix + command, opts))
            spec = sessions[0].tabs[0].windows[0].launch_spec
            assert isinstance(spec, LaunchSpec)
            self.ae(spec.args, expected)
            if prefix:
                self.ae(spec.opts.cwd, r'C:\Users\x' if 'Users' in prefix else r'\\server\share\two words')
            self.ae(list(action.args), ([prefix.strip().replace('"', '')] if prefix else []) + expected)
        # The hold wrapper is consumed by Go's run-shell; keep its POSIX single-quote round trip working too.
        shell_arg = next(x for x in cmdline_for_hold(opts=opts) if x.startswith('--shell='))
        self.ae(list(cmdline_split(shell_arg.partition('=')[2])), expected)

    def test_windows_editor_fallback(self):
        with patch('kitty.child.default_env', return_value={}), patch('kitty.utils.read_shell_environment', return_value={}):
            with patch('kitty.utils.which', side_effect=lambda exe, **kw: exe if exe == 'notepad.exe' else None):
                self.ae(get_editor_from_env_vars(), ['notepad.exe'])
            for candidate, expected in (('edit.exe', ['edit.exe']), ('nvim', ['nvim']), ('code', ['code', '--wait'])):
                with self.subTest(candidate=candidate), patch(
                    'kitty.utils.which', side_effect=lambda exe, **kw: exe if exe in (candidate, 'notepad.exe') else None
                ):
                    self.ae(get_editor_from_env_vars(), expected)
        # resolve_editor_cmd() looks VISUAL up on the environment's own PATH, which is not part of this test
        with patch('kitty.child.default_env', return_value={'VISUAL': 'custom-editor --wait'}), patch(
            'kitty.utils.resolve_editor_cmd', side_effect=lambda editor, env: editor
        ), patch('kitty.utils.which', return_value='custom-editor'):
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
