import json
import sys
import sysconfig
import tempfile
import unittest
from pathlib import Path

from python_ssl import METADATA, SHA256, VERSION, prepare_source, sha256, verify


class PythonSSLBuildTest(unittest.TestCase):
    def test_corrupt_source_is_rejected_before_extraction(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            archive = work / 'source.tar.xz'
            archive.write_bytes(b'not the verified upstream source')
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                prepare_source(archive, work)
            self.assertEqual(list(work.iterdir()), [archive])

    def test_only_the_tested_extension_is_verified(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stdlib = Path(tmp)
            directory = stdlib / 'lib-dynload'
            directory.mkdir()
            name = '_ssl' + sysconfig.get_config_var('EXT_SUFFIX')
            extension = directory / name
            extension.write_bytes(b'tested native extension')
            info = {
                'source_version': VERSION,
                'source_sha256': SHA256,
                'python_version': list(sys.version_info[:3]),
                'extension': name,
                'extension_sha256': sha256(extension),
            }
            metadata = directory / METADATA
            metadata.write_text(json.dumps(info), encoding='utf-8')
            verify(stdlib)
            for field in info:
                altered = dict(info, **{field: 'unexpected'})
                metadata.write_text(json.dumps(altered), encoding='utf-8')
                with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'does not match'):
                    verify(stdlib)
            metadata.write_text(json.dumps(info), encoding='utf-8')
            extension.write_bytes(b'replaced module')
            with self.assertRaisesRegex(ValueError, 'does not match'):
                verify(stdlib)
            extension.write_bytes(b'tested native extension')
            (directory / '_ssl.extra.pyd').write_bytes(b'additional module')
            with self.assertRaisesRegex(ValueError, 'does not match'):
                verify(stdlib)


if __name__ == '__main__':
    unittest.main()
