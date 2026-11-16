import json
import sys
import sysconfig
import tarfile
import tempfile
import unittest
from pathlib import Path

from python_security import METADATA, MODULES, SHA256, VERSION, prepare_source, replace, sha256, verify


class PythonSecurityBuildTest(unittest.TestCase):
    def test_unreviewed_source_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, 'source changed'):
            replace('missing', 'expected', 'fixed')
        with self.assertRaisesRegex(ValueError, 'source changed'):
            replace('expected expected', 'expected', 'fixed')
        self.assertEqual(replace('expected', 'expected', 'fixed'), 'fixed')

    def test_source_checksum_is_required_before_extraction(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            archive = work / 'untrusted.tar.xz'
            with tarfile.open(archive, 'w:xz'):
                pass
            with self.assertRaisesRegex(ValueError, 'archive checksum mismatch'):
                prepare_source(archive, work)
            self.assertFalse((work / f'Python-{VERSION}').exists())

    def test_binary_and_recipe_provenance_are_required(self) -> None:
        import python_security

        with tempfile.TemporaryDirectory() as tmp:
            stdlib = Path(tmp)
            directory = stdlib / 'lib-dynload'
            directory.mkdir()
            names = [name + sysconfig.get_config_var('EXT_SUFFIX') for name in MODULES]
            for name in names:
                (directory / name).write_bytes(b'tested native module')
            info = {
                'source_version': VERSION, 'source_sha256': SHA256,
                'recipe_sha256': sha256(Path(python_security.__file__)),
                'python_version': list(sys.version_info[:3]),
                'extensions': {name: sha256(directory / name) for name in names},
            }
            metadata = directory / METADATA
            metadata.write_text(json.dumps(info), encoding='utf-8')
            self.assertEqual({p.name for p in verify(stdlib)}, set(names))
            for key in ('source_version', 'source_sha256', 'recipe_sha256', 'python_version', 'extensions'):
                altered = dict(info)
                altered.pop(key)
                metadata.write_text(json.dumps(altered), encoding='utf-8')
                with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'do not match'):
                    verify(stdlib)
            metadata.write_text(json.dumps(info), encoding='utf-8')
            for name in names:
                module = directory / name
                module.write_bytes(b'replaced native module')
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'do not match'):
                    verify(stdlib)
                module.write_bytes(b'tested native module')
            (directory / '_socket.extra.pyd').write_bytes(b'extra module')
            with self.assertRaisesRegex(ValueError, 'do not match'):
                verify(stdlib)


if __name__ == '__main__':
    unittest.main()
