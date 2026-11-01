import shutil
import sysconfig
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from python_ssl import require_vendor_runtime, verify


class PythonSSLVendorTest(unittest.TestCase):
    def test_fixed_vendor_version_is_required(self) -> None:
        for version in ((3, 14, 9), (3, 13, 18), (3, 15, 2)):
            with self.subTest(version=version), patch('python_ssl.sys.version_info', version):
                with self.assertRaisesRegex(ValueError, 'Upgrade MSYS2'):
                    require_vendor_runtime()
        for version in ((3, 14, 10), (3, 14, 11)):
            with self.subTest(version=version), patch('python_ssl.sys.version_info', version):
                require_vendor_runtime()

    def test_both_vendor_binaries_must_be_unchanged(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            original = root / 'vendor/lib/python3.14'
            packaged = root / 'package/lib/python3.14'
            name = '_ssl' + sysconfig.get_config_var('EXT_SUFFIX')
            extension = Path('lib/python3.14/lib-dynload') / name
            dll = Path('bin/libpython3.14.dll')
            for relative in (extension, dll):
                source = root / 'vendor' / relative
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(b'fixed vendor binary')
                target = root / 'package' / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
            verify(packaged, original)
            for relative in (extension, dll):
                target = root / 'package' / relative
                with self.subTest(binary=str(relative)):
                    target.write_bytes(b'old or replaced binary')
                    with self.assertRaisesRegex(ValueError, 'differs from the fixed MSYS2 vendor binary'):
                        verify(packaged, original)
                    shutil.copy2(root / 'vendor' / relative, target)
            (packaged / 'lib-dynload/_ssl.extra.pyd').write_bytes(b'additional module')
            with self.assertRaisesRegex(ValueError, 'Unexpected vendor SSL extension layout'):
                verify(packaged, original)


if __name__ == '__main__':
    unittest.main()
