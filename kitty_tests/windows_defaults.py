#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kitsu contributors

import base64
import os
import shutil
import subprocess
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

from kitty.boss import Boss
from kitty.constants import is_windows, kitty_exe, shell_integration_dir, windows_default_shell
from kitty.options.types import defaults
from kitty.shell_integration import modify_shell_environ
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
    def test_windows_default_shell(self):
        inbox = os.path.join('C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')
        env = {'SystemRoot': 'C:\\Windows', 'COMSPEC': r'C:\custom cmd\cmd.exe'}
        with patch.dict(os.environ, env, clear=True), patch('shutil.which') as which, patch('os.path.isfile') as isfile:
            which.side_effect = lambda name: {'pwsh.exe': r'C:\Program Files\PowerShell\7\pwsh.exe', 'powershell.exe': inbox}.get(name)
            self.ae(windows_default_shell(), r'C:\Program Files\PowerShell\7\pwsh.exe')
            os.environ['SHELL'] = r'C:\custom shell\shell.exe'
            self.ae(windows_default_shell(), os.environ['SHELL'])
            del os.environ['SHELL']
            which.side_effect = lambda name: inbox if name == 'powershell.exe' else None
            self.ae(windows_default_shell(), inbox)
            which.side_effect = None
            which.return_value = None
            isfile.return_value = True
            self.ae(windows_default_shell(), inbox)
            isfile.return_value = False
            self.ae(windows_default_shell(), env['COMSPEC'])
            del os.environ['COMSPEC']
            self.ae(windows_default_shell(), 'cmd.exe')

    def test_windows_cmd_prompt_integration(self):
        opts = defaults._replace()

        def setup(args=(), env=None, flags=('enabled',)):
            opts.shell_integration = frozenset(flags)
            env = {} if env is None else env.copy()
            argv = [r'C:\Windows\System32\CMD.EXE', *args]
            modify_shell_environ(opts, env, argv)
            self.ae(argv, [r'C:\Windows\System32\CMD.EXE', *args])
            return env

        prompt = setup()['PROMPT']
        self.ae(prompt, '$E]133;D$E\\$E]133;A$E\\$E]7;file://localhost/$P$E\\$P$G$E]133;B$E\\')
        for flags in ((), ('disabled',), ('no-rc',)):
            self.ae(setup(flags=flags), {})
        for args in (('/c', 'echo hello'), ('/Cecho hello',), ('/?',)):
            self.ae(setup(args=args), {})
        for key in ('PROMPT', 'Prompt'):
            self.ae(setup(env={key: 'custom $G'}), {key: 'custom $G'})
        self.assertNotIn('133;', setup(flags=('no-prompt-mark',))['PROMPT'])
        self.assertNotIn(']7;', setup(flags=('no-cwd',))['PROMPT'])
        self.ae(setup(flags=('no-cwd', 'no-prompt-mark'))['PROMPT'], '$P$G')
        self.ae(setup(args=('/d', '/k'))['PROMPT'], prompt)
        env = os.environ.copy()
        env['PROMPT'] = prompt
        cp = subprocess.run(['cmd.exe', '/d', '/q'], input=b'echo integration-output\r\nexit\r\n', env=env, capture_output=True)
        self.ae(cp.returncode, 0, cp.stderr)
        self.assertIn(b'\x1b]133;A\x1b\\', cp.stdout)
        self.assertIn(b'\x1b]133;B\x1b\\', cp.stdout)
        self.assertIn(b'\x1b]7;file://localhost/', cp.stdout)
        self.assertIn(b'integration-output', cp.stdout)

    def test_windows_powershell_prompt_integration(self):
        script = os.path.join(shell_integration_dir, 'powershell', 'kitty-integration.ps1').replace("'", "''")
        shells = [path for name in ('powershell.exe', 'pwsh.exe') if (path := shutil.which(name))]
        self.assertTrue(shells, 'Windows PowerShell must be available')
        for shell in shells:
            for flags in ('enabled', 'no-cursor', 'no-prompt-mark no-cwd no-title no-cursor'):
                with self.subTest(shell=shell, flags=flags):
                    env = os.environ.copy()
                    env['KITTY_SHELL_INTEGRATION'] = flags
                    command = f"""
Import-Module PSReadLine -ErrorAction Stop
function global:Prompt {{ 'test> ' }}
function global:PSConsoleHostReadLine {{ 'echo integration-output' }}
$global:TestHistoryId = 1
function global:Get-History {{ [pscustomobject]@{{Id=$global:TestHistoryId}} }}
. '{script}'
[Console]::Write((Prompt))
$line = PSConsoleHostReadLine
Write-Output 'integration-output'
$global:TestHistoryId = 2
[Console]::Write((Prompt))
$line = PSConsoleHostReadLine
$global:TestHistoryId = 3
Write-Error 'test failure' -ErrorAction SilentlyContinue
[Console]::Write((Prompt))
"""
                    encoded = base64.b64encode(command.encode('utf-16-le')).decode('ascii')
                    cp = subprocess.run([shell, '-NoLogo', '-NoProfile', '-EncodedCommand', encoded], env=env, capture_output=True)
                    self.ae(cp.returncode, 0, cp.stderr.decode('utf-8', 'replace'))
                    output = cp.stdout.decode('utf-8', 'replace')
                    self.assertIn('integration-output', output)
                    for sequence, disabled in (
                        ('\x1b[5 q', 'no-cursor'),
                        ('\x1b[0 q', 'no-cursor'),
                        ('\x1b]7;file:', 'no-cwd'),
                        ('\x1b]2;', 'no-title'),
                        ('\x1b]133;A\x07', 'no-prompt-mark'),
                        ('\x1b]133;B\x07', 'no-prompt-mark'),
                        ('\x1b]133;C\x07', 'no-prompt-mark'),
                        ('\x1b]133;D;0\x07', 'no-prompt-mark'),
                        ('\x1b]133;D;1\x07', 'no-prompt-mark'),
                    ):
                        if disabled in flags:
                            self.assertNotIn(sequence, output)
                        else:
                            self.assertIn(sequence, output)

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
