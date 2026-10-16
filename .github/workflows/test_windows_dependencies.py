import copy
import hashlib
import tempfile
import unittest
from pathlib import Path
from typing import Any

from windows_dependencies import read_fields, runtime_components, verify_scan_inputs


class WindowsDependenciesTest(unittest.TestCase):
    def test_shipped_versions_and_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package = root / 'package'
            (package / 'bin').mkdir(parents=True)
            (root / 'ucrt64/bin').mkdir(parents=True)

            def installed(name: str, version: str, filename: str, content: bytes) -> None:
                entry = root / 'var/lib/pacman/local' / name
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

    def test_scan_attestations_are_bound_to_zip_and_python_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / 'kitty.zip'
            archive.write_bytes(b'tested package')
            version = hashlib.sha256(archive.read_bytes()).hexdigest()
            product = f'pkg:generic/kitty-windows@{version}'
            python_product = f'pkg:generic/python@3.14.8?kitty_package={version}'
            runtime: dict[str, Any] = {
                'metadata': {'component': {'name': 'kitty-windows', 'version': version, 'purl': product}},
                'components': [
                    {
                        'name': 'python',
                        'version': '3.14.8',
                        'purl': python_product,
                        'properties': [{'name': 'kitty:packaged-file', 'value': 'bin/libpython3.14.dll'}],
                    }
                ],
            }
            binaries: dict[str, Any] = {
                'source': {'name': 'kitty-windows', 'version': version},
                'artifacts': [{'name': 'Python', 'version': '3.14.8', 'locations': [{'path': '/bin/libpython3.14.dll'}]}],
            }
            vex: dict[str, Any] = {
                'statements': [
                    {'vulnerability': {'name': cve}, 'products': [{'@id': product}, {'@id': python_product}], 'status': 'fixed'}
                    for cve in ('CVE-2025-15367', 'CVE-2026-12345')
                ]
            }
            verify_scan_inputs(archive, runtime, binaries, vex)

            altered = copy.deepcopy(binaries)
            altered['artifacts'][0]['version'] = '3.13.0'
            with self.assertRaisesRegex(ValueError, 'additional Python binary'):
                verify_scan_inputs(archive, runtime, altered, vex)
            altered = copy.deepcopy(binaries)
            altered['artifacts'][0]['locations'][0]['path'] = '/extra/python.dll'
            with self.assertRaisesRegex(ValueError, 'additional Python binary'):
                verify_scan_inputs(archive, runtime, altered, vex)
            altered = copy.deepcopy(binaries)
            altered['artifacts'].clear()
            with self.assertRaisesRegex(ValueError, 'not identified'):
                verify_scan_inputs(archive, runtime, altered, vex)
            altered = copy.deepcopy(vex)
            altered['statements'][0]['products'][0]['@id'] = 'pkg:generic/python@3.14.8'
            with self.assertRaisesRegex(ValueError, 'not restricted'):
                verify_scan_inputs(archive, runtime, binaries, altered)
            altered = copy.deepcopy(vex)
            altered['statements'][0]['status'] = 'not_affected'
            with self.assertRaisesRegex(ValueError, 'not restricted'):
                verify_scan_inputs(archive, runtime, binaries, altered)
            archive.write_bytes(b'a different package')
            with self.assertRaisesRegex(ValueError, 'does not describe this ZIP'):
                verify_scan_inputs(archive, runtime, binaries, vex)


if __name__ == '__main__':
    unittest.main()
