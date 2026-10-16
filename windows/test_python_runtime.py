import shutil
import sysconfig
import tempfile
import unittest
from pathlib import Path
from zipfile import ZipFile

from python_runtime import harden_python, verify_archive, verify_python


class PythonRuntimePatchTest(unittest.TestCase):
    def test_patches_and_rejects_unpatched_or_modified_sources(self) -> None:
        original = Path(sysconfig.get_path('stdlib'))
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / 'kitty'
            stdlib = package / 'lib' / original.name
            stdlib.mkdir(parents=True)
            (package / 'bin').mkdir()
            (package / 'bin/kitten.exe').write_bytes(b'helper')
            for name in ('poplib.py', 'tempfile.py'):
                shutil.copy2(original / name, stdlib / name)
            with self.assertRaisesRegex(ValueError, 'backport was not verified'):
                verify_python(package, original)
            harden_python(stdlib)
            verify_python(package, original)
            for name in ('poplib.py', 'tempfile.py'):
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
            for path in (package / 'bin/kitten.exe', stdlib / 'poplib.py', stdlib / 'tempfile.py'):
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


if __name__ == '__main__':
    unittest.main()
