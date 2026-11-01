import sys
import unittest
from pathlib import Path, PureWindowsPath
from xml.etree import ElementTree
from zipfile import ZipFile

from windows.terminal_package import package_version


def verify_external_paths(package: Path) -> None:
    with ZipFile(package / 'terminal-identity.msix') as archive:
        manifest = ElementTree.fromstring(archive.read('AppxManifest.xml'))
    paths: set[str] = set()
    for element in manifest.iter():
        if element.tag.rpartition('}')[2] == 'Logo' and element.text:
            paths.add(element.text)
        for name, value in element.attrib.items():
            if name.endswith('Logo') or name in ('Executable', 'Path', 'PublicFolder'):
                paths.add(value)
    missing = [name for name in sorted(paths) if not (package / Path(*PureWindowsPath(name).parts)).exists()]
    if missing:
        raise ValueError(f'Sparse manifest paths missing from the external location: {", ".join(missing)}')


class TerminalPackageTest(unittest.TestCase):
    def test_version(self) -> None:
        self.assertEqual(package_version('0.49.2', ''), '0.49.2.0')
        self.assertEqual(package_version('0.49.2', 'v0.49.2-windows.12'), '0.49.2.12')
        for version, tag in (('0.49', ''), ('0.49.65536', ''), ('0.49.2', 'v0.49.2-windows.65536')):
            with self.assertRaises(ValueError):
                package_version(version, tag)


if __name__ == '__main__':
    if len(sys.argv) == 2 and Path(sys.argv[1]).is_dir():
        verify_external_paths(Path(sys.argv[1]))
        print('PASS: all sparse manifest assets, public folders, executables and proxy DLLs exist externally')
    else:
        unittest.main()
