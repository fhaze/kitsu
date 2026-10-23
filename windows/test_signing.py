import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

from signing import (
    REQUIRED_FILES,
    batches,
    check_signatures,
    expand,
    first_party_files,
    iscc_arguments,
    sign_command,
    sign_package,
)

SIGNTOOL = '$qC:\\Program Files (x86)\\signtool.exe$q sign /fd sha256 /sha1 AB12 $f'


def make_package(root: Path) -> Path:
    package = root / 'kitsu'
    for name in REQUIRED_FILES + (
        'cmd/extra.exe',
        'lib/kitty/kittens/other/module.pyd',
        # third-party files, never signed
        'bin/libpython3.14.dll',
        'bin/slangc.exe',
        'bin/OpenConsole.exe',
        'lib/python3.14/lib-dynload/_ssl.pyd',
        'lib/kitty/windows/wsl/kitten-linux-amd64',
    ):
        path = package / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'MZ')
    return package


class SigningTest(unittest.TestCase):
    def test_signs_only_first_party_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package = make_package(Path(tmp))
            names = {p.relative_to(package).as_posix() for p in first_party_files(package)}
            self.assertEqual(names, set(REQUIRED_FILES) | {'cmd/extra.exe', 'lib/kitty/kittens/other/module.pyd'})
            (package / 'bin/kitsu.exe').unlink()
            with self.assertRaisesRegex(ValueError, 'bin/kitsu.exe'):
                first_party_files(package)

    def test_expand(self) -> None:
        self.assertEqual(
            expand(SIGNTOOL, ['C:\\a b\\x.exe', 'C:\\y.dll']),
            '"C:\\Program Files (x86)\\signtool.exe" sign /fd sha256 /sha1 AB12 "C:\\a b\\x.exe" "C:\\y.dll"',
        )
        self.assertEqual(expand('a $$f $x $', ['f']), 'a $f $x $')
        with self.assertRaises(ValueError):
            expand('$f', ['a"b'])

    def test_sign_command(self) -> None:
        self.assertEqual(sign_command({}), '')
        self.assertEqual(sign_command({'KITSU_SIGN_COMMAND': '  '}), '')
        self.assertEqual(sign_command({'KITSU_SIGN_COMMAND': f' {SIGNTOOL} '}), SIGNTOOL)
        with self.assertRaisesRegex(ValueError, r'\$f'):
            sign_command({'KITSU_SIGN_COMMAND': 'signtool sign'})

    def test_sign_package(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package = make_package(Path(tmp))
            commands: list[str] = []
            self.assertEqual(sign_package(package, '', commands.append), [])
            self.assertEqual(commands, [])
            signed = sign_package(package, 'sign $f', commands.append)
            self.assertEqual(len(commands), 1)
            for path in signed:
                if path.name in ('kitty.exe', 'kitty.com') and path.parent.name == 'bin':
                    self.assertNotIn(f'"{path}"', commands[0])
                else:
                    self.assertIn(f'"{path}"', commands[0])
            self.assertNotIn('slangc', commands[0])

    def test_timestamped_compatibility_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package = make_package(Path(tmp))

            def timestamp_files(paths, cmd, run) -> None:
                for i, path in enumerate(paths):
                    path.write_bytes(path.read_bytes() + f'signature and timestamp {i}'.encode())

            with patch('signing.sign_files', side_effect=timestamp_files):
                signed = sign_package(package, SIGNTOOL)
            self.assertEqual(signed, first_party_files(package))
            for ext in ('.exe', '.com'):
                source = (package / 'bin' / ('kitsu' + ext)).read_bytes()
                self.assertIn(b'signature and timestamp', source)
                self.assertEqual((package / 'bin' / ('kitty' + ext)).read_bytes(), source)

    def test_refuses_different_compatibility_alias(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package = make_package(Path(tmp))
            (package / 'bin/kitty.exe').write_bytes(b'different launcher')
            with patch('signing.sign_files') as sign:
                with self.assertRaisesRegex(ValueError, 'differs .* before signing'):
                    sign_package(package, SIGNTOOL)
                sign.assert_not_called()

    def test_batches(self) -> None:
        paths = [f'C:\\{i:03}.exe' for i in range(100)]
        parts = list(batches(paths, limit=200))
        self.assertGreater(len(parts), 1)
        self.assertEqual([p for part in parts for p in part], paths)
        self.assertTrue(all(sum(len(p) + 3 for p in part) <= 200 for part in parts))

    def test_iscc_arguments(self) -> None:
        self.assertEqual(iscc_arguments(''), [])
        self.assertEqual(
            iscc_arguments('"C:\\signtool.exe" sign /f "C:\\a b.pfx" $f'),
            ['/DSignTool=kitsusign', '/Skitsusign=$qC:\\signtool.exe$q sign /f $qC:\\a b.pfx$q $f'],
        )

    def test_check_signatures(self) -> None:
        good = {'path': 'a.exe', 'status': 'Valid', 'message': '', 'thumbprint': 'AB12', 'subject': 'CN=Me', 'timestamped': True}
        self.assertEqual(check_signatures([good], 'ab:12', require_timestamp=True), [])
        unsigned = dict(good, status='NotSigned', message='The file is not digitally signed.', thumbprint='')
        self.assertEqual(check_signatures([unsigned]), ['a.exe: NotSigned: The file is not digitally signed.'])
        self.assertIn('not by CD34', check_signatures([good], 'CD34')[0])
        self.assertIn('no timestamp', check_signatures([dict(good, timestamped=False)], require_timestamp=True)[0])
        self.assertEqual(check_signatures([dict(good, timestamped=False)]), [])


if __name__ == '__main__':
    unittest.main()
