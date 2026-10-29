import errno
import os
import poplib
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch


class WindowsRuntimeTest(unittest.TestCase):
    def test_tar_filters_reject_links_replacing_destination(self) -> None:
        with tempfile.TemporaryDirectory() as destination:
            for filter in (tarfile.tar_filter, tarfile.data_filter):
                for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE):
                    for name in ('', '.', './', '././', 'subdir/..'):
                        member = tarfile.TarInfo(name)
                        member.type = kind
                        member.linkname = 'other'
                        with self.subTest(filter=filter.__name__, kind=kind, name=name), self.assertRaises(tarfile.OutsideDestinationError):
                            filter(member, destination)
                member = tarfile.TarInfo('regular.txt')
                self.assertEqual(filter(member, destination).name, 'regular.txt')
                directory = tarfile.TarInfo('.')
                directory.type = tarfile.DIRTYPE
                self.assertEqual(filter(directory, destination).name, '.')

    def junction(self, link: Path, target: Path) -> None:
        subprocess.run(
            ['cmd.exe', '/d', '/c', 'mklink', '/J', str(link).replace('/', '\\'), str(target).replace('/', '\\')],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )

    def test_pop_commands(self) -> None:
        client = poplib.POP3.__new__(poplib.POP3)
        client.set_debuglevel(0)
        client.encoding = 'utf-8'
        with patch.object(client, '_putline') as send, patch.object(client, '_getresp', return_value=b'+OK'):
            for char in (*range(32), 127):
                with self.subTest(char=char), self.assertRaises(ValueError):
                    client.user(f'name{chr(char)}QUIT')
            send.assert_not_called()
            client.user('valid')
            send.assert_called_once_with(b'USER valid')

    def test_cleanup_readonly_unicode_and_missing_tree(self) -> None:
        with tempfile.TemporaryDirectory(prefix='kitty-runtime-') as outer:
            tmp = tempfile.TemporaryDirectory(prefix='unicode-\N{SNOWMAN} space-', dir=outer)
            directory = Path(tmp.name) / 'nested'
            directory.mkdir()
            readonly = directory / 'readonly.txt'
            readonly.write_text('readonly', encoding='utf-8')
            readonly.chmod(stat.S_IREAD)
            tmp.cleanup()
            self.assertFalse(Path(tmp.name).exists())
            tmp.cleanup()

    def test_cleanup_does_not_follow_junctions(self) -> None:
        with tempfile.TemporaryDirectory() as outer:
            outside = Path(outer) / 'outside'
            outside.mkdir()
            sentinel = outside / 'keep.txt'
            sentinel.write_text('keep', encoding='utf-8')
            sentinel.chmod(stat.S_IREAD)
            before = sentinel.stat()
            with tempfile.TemporaryDirectory(dir=outer) as name:
                self.junction(Path(name) / 'link', outside)
            self.assertEqual(sentinel.read_text(encoding='utf-8'), 'keep')
            self.assertEqual(sentinel.stat().st_mode, before.st_mode)

    def test_replaced_cleanup_root_does_not_touch_target(self) -> None:
        with tempfile.TemporaryDirectory() as outer:
            outside = Path(outer) / 'outside'
            outside.mkdir()
            sentinel = outside / 'keep.txt'
            sentinel.write_text('keep', encoding='utf-8')
            tmp = tempfile.TemporaryDirectory(dir=outer)
            Path(tmp.name).rename(Path(outer) / 'moved')
            self.junction(Path(tmp.name), outside)
            tmp.cleanup()
            self.assertEqual(sentinel.read_text(encoding='utf-8'), 'keep')
            self.assertFalse(Path(tmp.name).exists())

    def test_cleanup_fails_closed_without_helper(self) -> None:
        with tempfile.TemporaryDirectory() as outer:
            tmp = tempfile.TemporaryDirectory(dir=outer)
            quiet = tempfile.TemporaryDirectory(dir=outer, ignore_cleanup_errors=True)
            with patch('tempfile._subprocess.run', side_effect=FileNotFoundError(errno.ENOENT, 'missing helper')):
                with self.assertRaises(FileNotFoundError):
                    tmp.cleanup()
                quiet.cleanup()
            self.assertTrue(Path(outer).exists())

    def test_cleanup_helper_errors_and_inherited_kittens(self) -> None:
        with tempfile.TemporaryDirectory() as outer:
            tmp = tempfile.TemporaryDirectory(dir=outer)
            quiet = tempfile.TemporaryDirectory(dir=outer, ignore_cleanup_errors=True)
            with patch('tempfile._subprocess.run', return_value=subprocess.CompletedProcess([], 1, stderr=b'cannot remove')):
                with self.assertRaises(PermissionError):
                    tmp.cleanup()
                quiet.cleanup()
            with patch.dict(os.environ, KITTY_KITTEN_RUN_MODULE='ssh_askpass'):
                tmp = tempfile.TemporaryDirectory(dir=outer)
                tmp.cleanup()
                self.assertFalse(Path(tmp.name).exists())

    def test_interpreter_shutdown_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as outer:
            code = f'import tempfile; tmp = tempfile.TemporaryDirectory(dir={outer!r})'
            subprocess.run([sys.executable, '+runpy', code], check=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW'))
            self.assertEqual(list(Path(outer).iterdir()), [])

    def test_concurrent_junction_swaps_preserve_outside_files(self) -> None:
        with tempfile.TemporaryDirectory() as outer:
            outside = Path(outer) / 'outside'
            outside.mkdir()
            sentinel = outside / 'keep.txt'
            sentinel.write_text('keep', encoding='utf-8')
            sentinel.chmod(stat.S_IREAD)
            mode = sentinel.stat().st_mode
            for _ in range(24):
                tmp = tempfile.TemporaryDirectory(dir=outer)
                tree = Path(tmp.name)
                (tree / 'child').mkdir()
                for i in range(50):
                    (tree / 'child' / str(i)).write_bytes(b'delete')
                self.junction(tree / 'link', outside)
                stop = threading.Event()
                started = threading.Event()

                def swap() -> None:
                    while not stop.is_set():
                        try:
                            (tree / 'child').rename(tree / 'hold')
                            (tree / 'link').rename(tree / 'child')
                            started.set()
                            (tree / 'child').rename(tree / 'link')
                            (tree / 'hold').rename(tree / 'child')
                        except OSError:
                            return

                worker = threading.Thread(target=swap)
                worker.start()
                try:
                    self.assertTrue(started.wait(5), 'Junction swapping did not start')
                    try:
                        tmp.cleanup()
                    except OSError:
                        # Concurrent mutation may prevent removal, but must not escape the tree.
                        pass
                finally:
                    stop.set()
                    worker.join()
                self.assertEqual(sentinel.read_text(encoding='utf-8'), 'keep')
                self.assertEqual(sentinel.stat().st_mode, mode)


if __name__ == '__main__':
    unittest.main(argv=[__file__])
