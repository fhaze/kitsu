#!/usr/bin/env python
"""Unicode and long path handling of the packaged Windows executables.

Run against a built package: python windows/test_unicode_paths.py windows-package/kitsu
The POSIX shims themselves are tested by test_win32_compat.c.
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from ctypes import wintypes
from pathlib import Path

PACKAGE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('windows-package/kitsu').resolve()
SOURCE = Path(__file__).resolve().parents[1]
# Outside of any single ANSI code page: CJK, Latin-1, Cyrillic and an emoji,
# which is a surrogate pair in UTF-16
UNICODE_NAME = '日本語 é Ж \N{GRINNING FACE}'
EXECUTABLES = ('kitsu.exe', 'kitsu.com', 'kitty.exe', 'kitty.com', 'kitten.exe')


def run(exe: Path, *args: str, env: dict[str, str] | None = None, check: bool = True) -> subprocess.CompletedProcess[str]:
    cp = subprocess.run([str(exe), *args], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120, env=env)
    if check and cp.returncode != 0:
        raise AssertionError(f'{exe} {args} failed with exit code {cp.returncode}:\n{cp.stdout}\n{cp.stderr}')
    return cp


def run_python(code: str, *args: str, exe: Path | None = None, env: dict[str, str] | None = None) -> object:
    # The code prints its result as JSON, which is ASCII, so that the test
    # does not depend on the encoding of the output.
    cp = run(exe or PACKAGE / 'bin' / 'kitsu.com', '+runpy', code, *args, env=env)
    return json.loads(cp.stdout.strip().splitlines()[-1])


def native(path: str | Path) -> str:
    # MSYS2's Python uses / as the separator, which \\?\ paths and cmd.exe do not accept
    return str(path).replace('/', '\\')


def junction(link: Path, target: Path) -> None:
    subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', native(link), native(target)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def same_path(a: str, b: str | Path) -> bool:
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(str(b)))


class TempDir(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix='kitsu-unicode-'))

    def tearDown(self) -> None:
        # long paths need the \\?\ prefix to be removed
        shutil.rmtree('\\\\?\\' + native(self.tmp), ignore_errors=True)


@unittest.skipUnless(sys.platform == 'win32', 'Windows executables')
class TestManifest(unittest.TestCase):
    def manifest(self, path: Path) -> bytes:
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.LoadLibraryExW.argtypes = (wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD)
        kernel.LoadLibraryExW.restype = wintypes.HMODULE
        kernel.FindResourceW.argtypes = (wintypes.HMODULE, wintypes.LPCWSTR, wintypes.LPCWSTR)
        kernel.FindResourceW.restype = wintypes.HRSRC
        kernel.SizeofResource.argtypes = (wintypes.HMODULE, wintypes.HRSRC)
        kernel.SizeofResource.restype = wintypes.DWORD
        kernel.LoadResource.argtypes = (wintypes.HMODULE, wintypes.HRSRC)
        kernel.LoadResource.restype = wintypes.HGLOBAL
        kernel.LockResource.argtypes = (wintypes.HGLOBAL,)
        kernel.LockResource.restype = ctypes.c_void_p
        kernel.FreeLibrary.argtypes = (wintypes.HMODULE,)
        # LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE
        module = kernel.LoadLibraryExW(str(path), None, 0x22)
        self.assertTrue(module, ctypes.get_last_error())
        try:
            # RT_MANIFEST, CREATEPROCESS_MANIFEST_RESOURCE_ID
            found = kernel.FindResourceW(module, ctypes.cast(1, wintypes.LPCWSTR), ctypes.cast(24, wintypes.LPCWSTR))
            self.assertTrue(found, f'{path.name} has no application manifest')
            ptr = kernel.LockResource(kernel.LoadResource(module, found))
            return ctypes.string_at(ptr, kernel.SizeofResource(module, found))
        finally:
            kernel.FreeLibrary(module)

    def test_embedded_manifest(self) -> None:
        expected = (SOURCE / 'kitty/launcher/windows-app.manifest').read_bytes()
        for name in EXECUTABLES:
            with self.subTest(binary=name):
                path = PACKAGE / 'bin' / name
                manifest = self.manifest(path)
                self.assertEqual(manifest.replace(b'\r\n', b'\n'), expected.replace(b'\r\n', b'\n'))
                self.assertIn(b'>UTF-8</activeCodePage>', manifest)
                self.assertIn(b'>true</longPathAware>', manifest)
                self.assertIn(b'<requestedExecutionLevel level="asInvoker"/>', manifest)
                # Windows 10/11
                self.assertIn(b'{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}', manifest)
                # the default MinGW-w64 manifest must have been dropped by the linker
                self.assertEqual(path.read_bytes().count(b'urn:schemas-microsoft-com:asm.v1'), 1)

    def test_active_code_page(self) -> None:
        for name in ('kitsu.exe', 'kitsu.com'):
            with self.subTest(binary=name):
                acp = run_python('import ctypes, json; print(json.dumps(ctypes.windll.kernel32.GetACP()))', exe=PACKAGE / 'bin' / name)
                self.assertEqual(acp, 65001)


@unittest.skipUnless(sys.platform == 'win32', 'Windows executables')
class TestArguments(unittest.TestCase):
    args = ['é', '日本 語', '\N{GRINNING FACE}', 'a"b', 'back\\slash\\', '', 'trailing\\', '--opt=値', UNICODE_NAME]

    def test_python_receives_unicode_arguments(self) -> None:
        for name in ('kitsu.com', 'kitsu.exe'):
            with self.subTest(binary=name):
                argv = run_python('import json, sys; print(json.dumps(sys.argv[1:]))', *self.args, exe=PACKAGE / 'bin' / name)
                self.assertEqual(argv, self.args)

    def test_kitten_receives_unicode_arguments(self) -> None:
        # kitsu @ runs kitten.exe, the host name ends up in the error message
        cp = run(PACKAGE / 'bin' / 'kitsu.com', '@', '--to', f'tcp:{UNICODE_NAME}:1', 'ls', check=False)
        self.assertNotEqual(cp.returncode, 0)
        self.assertIn(UNICODE_NAME, cp.stdout + cp.stderr)


@unittest.skipUnless(sys.platform == 'win32', 'Windows executables')
class TestInstallDirectory(TempDir):
    # Installed per user by default, so the install directory contains the user name
    def setUp(self) -> None:
        super().setUp()
        self.root = self.tmp / f'{UNICODE_NAME} Kitsu'
        junction(self.root, PACKAGE)

    def tearDown(self) -> None:
        os.rmdir(self.root)  # removes the junction, not its target
        super().tearDown()

    def test_runs_from_non_ascii_directory(self) -> None:
        for name in ('kitsu.exe', 'kitsu.com'):
            with self.subTest(binary=name):
                cp = run(self.root / 'bin' / name, '--version')
                self.assertIn('Kitsu ', cp.stdout)
        code = '''
import json, os
from kitty.constants import fonts_dir, kitty_exe
from kitty.fast_data_types import Face, set_builtin_nerd_font
nerd_font = os.path.join(fonts_dir, 'SymbolsNerdFontMono-Regular.ttf')
descriptor = set_builtin_nerd_font(nerd_font)
print(json.dumps({
    'exe': kitty_exe(), 'nerd_font': nerd_font, 'descriptor_path': descriptor['path'],
    'postscript_name': Face(path=nerd_font).postscript_name(),
}))
'''
        result = run_python(code, exe=self.root / 'bin' / 'kitsu.com')
        assert isinstance(result, dict)
        self.assertTrue(same_path(result['exe'], self.root / 'bin' / 'kitsu.exe'), result['exe'])
        self.assertTrue(same_path(result['nerd_font'], self.root / 'lib/kitty/fonts/SymbolsNerdFontMono-Regular.ttf'), result['nerd_font'])
        self.assertEqual(result['descriptor_path'], result['nerd_font'])
        self.assertEqual(result['postscript_name'], 'SymbolsNFM')

    def test_kitten_runs_from_non_ascii_directory(self) -> None:
        cp = run(self.root / 'bin' / 'kitsu.com', '@', '--help')
        self.assertIn('Control kitty by sending it commands', cp.stdout)


@unittest.skipUnless(sys.platform == 'win32', 'Windows executables')
class TestFonts(TempDir):
    def test_font_from_non_ascii_path(self) -> None:
        font_dir = self.tmp / UNICODE_NAME
        font_dir.mkdir()
        font = font_dir / f'フォント {UNICODE_NAME}.ttf'
        shutil.copy2(SOURCE / 'kitty_tests/LiberationMono-Regular.ttf', font)
        code = '''
import json, os, sys
from kitty.fast_data_types import Face, add_font_file, fc_list
path = sys.argv[1]
added = add_font_file(path)
norm = lambda p: os.path.normcase(os.path.normpath(p))
descriptors = [d for d in fc_list() if norm(d['path']) == norm(path)]
print(json.dumps({
    'by_path': Face(path=path).postscript_name(), 'added': added, 'listed': len(descriptors),
    'by_descriptor': Face(descriptor=descriptors[0]).postscript_name() if descriptors else '',
}))
'''
        result = run_python(code, str(font))
        self.assertEqual(result, {'by_path': 'LiberationMono', 'added': True, 'listed': 1, 'by_descriptor': 'LiberationMono'})


@unittest.skipUnless(sys.platform == 'win32', 'Windows executables')
class TestPaths(TempDir):
    def test_config_directory(self) -> None:
        config = self.tmp / UNICODE_NAME
        config.mkdir()
        env = dict(os.environ, KITTY_CONFIG_DIRECTORY=str(config))
        code = (
            'import json; from kitty.constants import config_dir; from kitty.fast_data_types import get_config_dir; '
            'print(json.dumps([config_dir, get_config_dir()]))'
        )
        result = run_python(code, env=env)
        assert isinstance(result, list)
        for path in result:
            self.assertTrue(same_path(path, config), path)

    def test_long_paths(self) -> None:
        # longer than MAX_PATH, as UTF-16 and as UTF-8, created by the C code
        component = f'長いディレクトリ名 long directory name {UNICODE_NAME}'
        deep = '\\'.join([native(self.tmp)] + [component] * 6)
        self.assertGreater(len(deep), 260)
        code = r'''
import json, os, sys
from kitty.fast_data_types import makedirs, read_file
deep = sys.argv[1]
makedirs(deep)
# Python itself needs the \\?\ prefix for long paths
with open('\\\\?\\' + deep + '\\file.txt', 'w', encoding='utf-8') as f:
    f.write('contents')
print(json.dumps({'isdir': os.path.isdir('\\\\?\\' + deep), 'read': read_file(deep + '/file.txt').decode()}))
'''
        self.assertEqual(run_python(code, deep), {'isdir': True, 'read': 'contents'})


if __name__ == '__main__':
    unittest.main()
