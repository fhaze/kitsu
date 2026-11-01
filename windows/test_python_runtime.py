import shutil
import sysconfig
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from zipfile import ZipFile

from python_runtime import POP_COMMAND, TEMP_CLEANUP, TEMP_CLEANUP_SIGNATURES, harden_python, verify_archive, verify_python


class PythonRuntimePatchTest(unittest.TestCase):
    def setUp(self) -> None:
        # Native extension integrity has its own tests; these cover stdlib patches.
        verifier = patch('windows.python_ssl.verify')
        verifier.start()
        self.addCleanup(verifier.stop)

    def copy_tarfile(self, directory: Path) -> None:
        shutil.copy2(Path(sysconfig.get_path('stdlib')) / 'tarfile.py', directory / 'tarfile.py')

    def test_supported_cleanup_signatures(self) -> None:
        for signature in TEMP_CLEANUP_SIGNATURES:
            with self.subTest(signature=signature), tempfile.TemporaryDirectory() as tmp:
                package = Path(tmp) / 'kitty'
                original = Path(tmp) / 'python3.14'
                original.mkdir()
                self.copy_tarfile(original)
                (original / 'poplib.py').write_text(POP_COMMAND, encoding='utf-8')
                cleanup = '    @classmethod\n    def _cleanup(cls, name):\n        cls._rmtree(name)\n'
                source = 'import shutil as _shutil\n\nclass TemporaryDirectory:\n' + signature + '        pass\n\n' + cleanup
                (original / 'tempfile.py').write_text(source, encoding='utf-8')
                stdlib = package / 'lib' / original.name
                shutil.copytree(original, stdlib)
                (package / 'bin').mkdir()
                (package / 'bin/kitten.exe').write_bytes(b'helper')
                with self.assertRaisesRegex(ValueError, 'backport was not verified'):
                    verify_python(package, original)
                harden_python(stdlib)
                verify_python(package, original)
                patched = (stdlib / 'tempfile.py').read_text(encoding='utf-8')
                self.assertIn(TEMP_CLEANUP, patched)
                self.assertIn(cleanup, patched)
                self.assertEqual(patched.count('import subprocess as _subprocess\n'), 1)
                compile(patched, str(stdlib / 'tempfile.py'), 'exec')

    def test_changed_cleanup_source_fails_closed(self) -> None:
        signature = TEMP_CLEANUP_SIGNATURES[0]
        cleanup = '    @classmethod\n    def _cleanup(cls, name):\n        pass\n'
        source = 'import shutil as _shutil\n\nclass TemporaryDirectory:\n' + signature + '        pass\n\n' + cleanup
        for changed in (
            source.replace('repeated=False', 'changed=False'),
            source.replace(cleanup, ''),
            source.replace('import shutil as _shutil\n', ''),
            source + signature,
            source + cleanup,
            cleanup + source.replace(cleanup, ''),
        ):
            with self.subTest(source=changed), tempfile.TemporaryDirectory() as tmp:
                stdlib = Path(tmp)
                (stdlib / 'poplib.py').write_text(POP_COMMAND, encoding='utf-8')
                (stdlib / 'tempfile.py').write_text(changed, encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'tempfile source changed'):
                    harden_python(stdlib)
                self.assertEqual((stdlib / 'poplib.py').read_text(encoding='utf-8'), POP_COMMAND)

    def test_patches_and_rejects_unpatched_or_modified_sources(self) -> None:
        original = Path(sysconfig.get_path('stdlib'))
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / 'kitty'
            stdlib = package / 'lib' / original.name
            stdlib.mkdir(parents=True)
            (package / 'bin').mkdir()
            (package / 'bin/kitten.exe').write_bytes(b'helper')
            for name in ('poplib.py', 'tempfile.py', 'tarfile.py'):
                shutil.copy2(original / name, stdlib / name)
            with self.assertRaisesRegex(ValueError, 'backport was not verified'):
                verify_python(package, original)
            harden_python(stdlib)
            verify_python(package, original)
            for name in ('poplib.py', 'tempfile.py', 'tarfile.py'):
                with self.subTest(name=name):
                    patched = (stdlib / name).read_bytes()
                    (stdlib / name).write_bytes(patched + b'\n')
                    with self.assertRaisesRegex(ValueError, 'backport was not verified'):
                        verify_python(package, original)
                    (stdlib / name).write_bytes(patched)
            (package / 'bin/kitten.exe').unlink()
            with self.assertRaisesRegex(ValueError, 'helper is missing'):
                verify_python(package, original)

    def test_changed_upstream_source_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stdlib = Path(tmp)
            (stdlib / 'poplib.py').write_text('changed upstream source', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'source changed'):
                harden_python(stdlib)

    def test_zip_must_match_the_tested_runtime(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / 'kitty'
            (package / 'bin').mkdir(parents=True)
            stdlib = package / 'lib/python3.14'
            stdlib.mkdir(parents=True)
            extensions = stdlib / 'lib-dynload'
            extensions.mkdir()
            for path in (
                package / 'bin/kitten.exe',
                stdlib / 'poplib.py',
                stdlib / 'tempfile.py',
                stdlib / 'tarfile.py',
                extensions / '_ssl-build.json',
                extensions / '_ssl.test.pyd',
            ):
                path.write_bytes(b'tested')
            archive = Path(tmp) / 'kitty.zip'
            with ZipFile(archive, 'w') as z:
                for path in package.rglob('*'):
                    if path.is_file():
                        z.write(path, path.relative_to(package.parent))
            verify_archive(package, archive)
            (stdlib / 'tempfile.py').write_bytes(b'not the tested runtime')
            with self.assertRaisesRegex(ValueError, 'differs from the ZIP'):
                verify_archive(package, archive)

    def test_changed_tar_source_fails_before_writing_patches(self) -> None:
        original = Path(sysconfig.get_path('stdlib'))
        with tempfile.TemporaryDirectory() as tmp:
            stdlib = Path(tmp)
            for name in ('poplib.py', 'tempfile.py'):
                shutil.copy2(original / name, stdlib / name)
            (stdlib / 'tarfile.py').write_text('changed upstream source', encoding='utf-8')
            before = (stdlib / 'poplib.py').read_bytes()
            with self.assertRaisesRegex(ValueError, 'tarfile source changed'):
                harden_python(stdlib)
            self.assertEqual((stdlib / 'poplib.py').read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
