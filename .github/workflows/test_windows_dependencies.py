import tempfile
import unittest
from pathlib import Path

from windows_dependencies import read_fields, runtime_components


class WindowsDependenciesTest(unittest.TestCase):
    def test_shipped_versions_and_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package = root / 'package'
            (package / 'bin').mkdir(parents=True)
            (root / 'ucrt64/bin').mkdir(parents=True)

            def installed(name: str, version: str, filename: str, content: bytes) -> None:
                entry = root / 'var/lib/pacman/local' / f'{name}-{version}'
                entry.mkdir(parents=True)
                (entry / 'desc').write_text(f'%NAME%\nmingw-w64-ucrt-x86_64-{name}\n\n%VERSION%\n{version}\n', encoding='utf-8')
                (entry / 'files').write_text(f'%FILES%\nucrt64/bin/{filename}\n', encoding='utf-8')
                (root / 'ucrt64/bin' / filename).write_bytes(content)

            installed('python', '3.14.8-1', 'libpython3.14.dll', b'python')
            installed('openssl', '1:3.6.0-2', 'libssl.dll', b'openssl')
            installed('unshipped', '1.0-1', 'unshipped.exe', b'build tool')
            installed('different', '1.0-1', 'different.dll', b'not shipped')
            (package / 'bin/libpython3.14.dll').write_bytes(b'python')
            (package / 'bin/LIBSSL.DLL').write_bytes(b'openssl')
            (package / 'bin/different.dll').write_bytes(b'other binary with same name')
            (package / 'bin/kitten.exe').write_bytes(b'locally built')
            components = runtime_components(package, root, 'mingw-w64-ucrt-x86_64')
            by_name = {component['name']: component for component in components}
            self.assertEqual(set(by_name), {'python', 'openssl'})
            self.assertEqual(by_name['python']['version'], '3.14.8')
            self.assertEqual(by_name['openssl']['version'], '3.6.0')
            self.assertEqual(by_name['python']['purl'], 'pkg:generic/python@3.14.8')
            self.assertIn({'name': 'kitty:packaged-file', 'value': 'bin/libpython3.14.dll'}, by_name['python']['properties'])

    def test_empty_package_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'No packaged Windows binaries'):
                runtime_components(Path(tmp), Path(tmp), 'mingw-w64-ucrt-x86_64')

    def test_missing_python_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'var/lib/pacman/local').mkdir(parents=True)
            (root / 'kitten.exe').write_bytes(b'kitten')
            with self.assertRaisesRegex(ValueError, 'Python runtime was not identified'):
                runtime_components(root, root, 'mingw-w64-ucrt-x86_64')

    def test_database_fields(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'desc'
            path.write_text('%NAME%\npython\n\n%LICENSE%\nPSF\nBSD\n\n', encoding='utf-8')
            self.assertEqual(read_fields(path), {'NAME': ['python'], 'LICENSE': ['PSF', 'BSD']})


if __name__ == '__main__':
    unittest.main()
