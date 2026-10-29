import unittest

from windows.terminal_package import package_version


class TerminalPackageTest(unittest.TestCase):
    def test_version(self) -> None:
        self.assertEqual(package_version('0.49.2', ''), '0.49.2.0')
        self.assertEqual(package_version('0.49.2', 'v0.49.2-windows.12'), '0.49.2.12')
        for version, tag in (('0.49', ''), ('0.49.65536', ''), ('0.49.2', 'v0.49.2-windows.65536')):
            with self.assertRaises(ValueError):
                package_version(version, tag)


if __name__ == '__main__':
    unittest.main()
