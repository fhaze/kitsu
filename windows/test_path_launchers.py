#!/usr/bin/env python
"""Check that the Windows PATH directory exposes commands, not runtime DLLs."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PACKAGE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('windows-package/kitsu').resolve()
COMMANDS = ('kitsu.com', 'kitsu.exe', 'kitty.com', 'kitty.exe', 'kitten.exe')


@unittest.skipUnless(sys.platform == 'win32', 'Windows executables')
class TestPathLaunchers(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix='kitsu PATH ')
        self.addCleanup(self.tmp.cleanup)
        self.cwd = Path(self.tmp.name)
        self.env = dict(os.environ)
        system = Path(os.environ['SystemRoot']) / 'System32'
        self.env['PATH'] = os.pathsep.join(map(str, (PACKAGE / 'cmd', system, system / 'WindowsPowerShell/v1.0')))
        for key in ('PYTHONHOME', 'PYTHONPATH', 'KITTY_PID', 'KITTY_PATH_TO_KITTY_EXE'):
            self.env.pop(key, None)

    def run_command(self, *args: str, input: str = '') -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            args, input=input, capture_output=True, text=True, encoding='utf-8', errors='replace',
            cwd=self.cwd, env=self.env, timeout=60,
        )

    def test_only_dll_free_commands_are_on_path(self) -> None:
        self.assertEqual(sorted(x.name for x in (PACKAGE / 'cmd').iterdir()), sorted(COMMANDS))
        private_dlls = {p.name.lower() for p in (PACKAGE / 'bin').glob('*.dll')}
        self.assertIn('zlib1.dll', private_dlls)
        for name in COMMANDS:
            with self.subTest(command=name):
                out = subprocess.check_output(['objdump', '-p', str(PACKAGE / 'cmd' / name)], text=True)
                imports = {line.split(':', 1)[1].strip().lower() for line in out.splitlines() if line.strip().startswith('DLL Name:')}
                self.assertTrue(imports)
                self.assertFalse(imports & private_dlls, imports & private_dlls)
                result = self.run_command(str(PACKAGE / 'cmd' / name), '--version')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('kitten' if name == 'kitten.exe' else 'Kitsu', result.stdout)

    def test_shell_command_resolution(self) -> None:
        for shell in ('cmd.exe', 'powershell.exe'):
            for name in ('kitsu', 'kitty', 'kitten'):
                with self.subTest(shell=shell, command=name):
                    args = ('/d', '/c', f'{name} --version') if shell == 'cmd.exe' else ('-NoProfile', '-Command', f'{name} --version; exit $LASTEXITCODE')
                    result = self.run_command(shell, *args)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('kitten' if name == 'kitten' else 'Kitsu', result.stdout)

    def test_kitten_finds_private_python_runtime(self) -> None:
        script = self.cwd / 'custom.py'
        script.write_text('def main(args):\n    print("Python kitten reached")\n', encoding='utf-8')
        self.env['KITTY_CONFIG_DIRECTORY'] = str(self.cwd)
        result = self.run_command(str(PACKAGE / 'cmd/kitten.exe'), script.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Python kitten reached', result.stdout)

    def test_arguments_streams_working_directory_and_exit_code(self) -> None:
        arguments = ['space in argument', '"quoted"', 'trailing\\', 'C:\\with space\\', '', '\N{GRINNING FACE}']
        code = (
            'import json, os, sys; '
            'print(json.dumps([sys.argv[1:], sys.stdin.read(), os.getcwd()])); '
            'print("stderr marker", file=sys.stderr); sys.exit(37)'
        )
        for name in COMMANDS[:-1]:
            with self.subTest(command=name):
                result = self.run_command(str(PACKAGE / 'cmd' / name), '+runpy', code, *arguments, input='stdin marker\n')
                self.assertEqual(result.returncode, 37, result.stderr)
                actual_args, stdin, cwd = json.loads(result.stdout)
                self.assertEqual(actual_args, arguments)
                self.assertEqual(stdin, 'stdin marker\n')
                self.assertEqual(Path(cwd), self.cwd)
                self.assertIn('stderr marker', result.stderr)

    def test_relocated_package_with_spaces_and_unicode(self) -> None:
        relocated = self.cwd / 'Kitsu \N{GRINNING FACE}'
        # Copy the command shims; junction the private runtime to avoid copying hundreds of MB.
        shutil.copytree(PACKAGE / 'cmd', relocated / 'cmd')
        for name in ('bin', 'lib', 'etc', 'share'):
            subprocess.run(
                ['cmd.exe', '/d', '/c', 'mklink', '/J', str(relocated / name).replace('/', '\\'), str(PACKAGE / name).replace('/', '\\')],
                check=True, capture_output=True,
            )
        for name in COMMANDS[:-1]:
            with self.subTest(command=name):
                result = self.run_command(str(relocated / 'cmd' / name), '--version')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('Kitsu', result.stdout)

    def test_unrelated_process_cannot_load_private_dll_from_path(self) -> None:
        source = self.cwd / 'probe.c'
        source.write_text(
            '#include <windows.h>\n'
            'int main(void) {\n'
            '    SetErrorMode(SEM_FAILCRITICALERRORS);\n'
            '    HMODULE dll = LoadLibraryW(L"zlib1.dll");\n'
            '    if (!dll) return GetLastError() == ERROR_MOD_NOT_FOUND ? 0 : 2;\n'
            '    FreeLibrary(dll);\n'
            '    return 1;\n'
            '}\n', encoding='utf-8',
        )
        probe = self.cwd / 'probe.exe'
        subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(probe)], check=True, capture_output=True)
        # Positive control: reproduce the old installer's fallback DLL search via bin on PATH.
        old_env = dict(self.env, PATH=str(PACKAGE / 'bin') + os.pathsep + self.env['PATH'])
        old = subprocess.run([str(probe)], env=old_env, cwd=self.cwd, timeout=30)
        self.assertEqual(old.returncode, 1, 'The legacy bin PATH must expose zlib1.dll')
        result = self.run_command(str(probe))
        self.assertEqual(result.returncode, 0, 'The command-only PATH must not expose zlib1.dll')


if __name__ == '__main__':
    unittest.main()
