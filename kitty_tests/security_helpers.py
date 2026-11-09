#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

import ast
import io
import os
import tarfile
import tempfile
import unittest
from pathlib import Path


def load_tar_helper():
    # Exercise the source helper without requiring a compiled fast_data_types module.
    path = Path(__file__).resolve().parents[1] / 'kitty' / 'utils.py'
    tree = ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
    tree.body = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'extract_all_from_tarfile_safely']
    namespace = {}
    exec(compile(tree, str(path), 'exec'), namespace)
    return namespace['extract_all_from_tarfile_safely']


extract_all_from_tarfile_safely = load_tar_helper()


def archive(*entries):
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode='w') as tf:
        for name, kind, value in entries:
            member = tarfile.TarInfo(name)
            member.type = kind
            if kind == tarfile.REGTYPE:
                member.size = len(value)
                tf.addfile(member, io.BytesIO(value))
            else:
                member.linkname = value
                tf.addfile(member)
    data.seek(0)
    return tarfile.open(fileobj=data)


class SafeTarExtraction(unittest.TestCase):
    def test_reject_escape_across_runtime_defaults(self):
        for default in (None, tarfile.fully_trusted_filter, tarfile.data_filter):
            for errorlevel in (0, 1, 2):
                for kind in ('sibling_prefix', 'symlink', 'nested_symlinks', 'hardlink', 'hardlink_through_symlink'):
                    with self.subTest(default=default, errorlevel=errorlevel, kind=kind), tempfile.TemporaryDirectory() as root:
                        dest, outside = Path(root) / 'dest', Path(root) / 'dest-evil'
                        dest.mkdir()
                        outside.mkdir()
                        victim = outside / 'victim'
                        victim.write_bytes(b'unchanged')
                        entries = [('../dest-evil/victim', tarfile.REGTYPE, b'changed')]
                        if kind == 'symlink':
                            entries = [('escape', tarfile.SYMTYPE, '../dest-evil'), ('escape/victim', tarfile.REGTYPE, b'changed')]
                        elif kind == 'nested_symlinks':
                            entries = [('a', tarfile.SYMTYPE, 'b'), ('b', tarfile.SYMTYPE, '../dest-evil'), ('a/victim', tarfile.REGTYPE, b'changed')]
                        elif kind == 'hardlink':
                            entries = [('link', tarfile.LNKTYPE, '../dest-evil/victim')]
                        elif kind == 'hardlink_through_symlink':
                            entries = [('escape', tarfile.SYMTYPE, '../dest-evil'), ('link', tarfile.LNKTYPE, 'escape/victim')]
                        with archive(*entries) as tf:
                            tf.extraction_filter = default
                            tf.errorlevel = errorlevel
                            with self.assertRaises(tarfile.FilterError):
                                extract_all_from_tarfile_safely(tf, str(dest))
                            self.assertEqual(tf.errorlevel, errorlevel)
                        self.assertEqual(victim.read_bytes(), b'unchanged')
                        self.assertFalse((dest / 'link').exists())

    def test_absolute_member_and_link_targets(self):
        for kind in (tarfile.REGTYPE, tarfile.SYMTYPE, tarfile.LNKTYPE):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as root:
                dest, victim = Path(root) / 'dest', Path(root) / 'victim'
                dest.mkdir()
                victim.write_bytes(b'unchanged')
                entry = (str(victim), kind, b'changed') if kind == tarfile.REGTYPE else ('link', kind, str(victim))
                with archive(entry) as tf:
                    try:
                        extract_all_from_tarfile_safely(tf, str(dest))
                    except tarfile.FilterError:
                        pass
                self.assertEqual(victim.read_bytes(), b'unchanged')
                self.assertFalse((dest / 'link').exists())

    def test_internal_data_and_links(self):
        with tempfile.TemporaryDirectory() as root:
            dest = Path(root)
            with archive(('dir/file', tarfile.REGTYPE, b'data'), ('hard', tarfile.LNKTYPE, 'dir/file')) as tf:
                tf.errorlevel = 0
                extract_all_from_tarfile_safely(tf, root)
                self.assertEqual(tf.errorlevel, 0)
            self.assertEqual((dest / 'dir/file').read_bytes(), b'data')
            self.assertEqual((dest / 'hard').read_bytes(), b'data')
            probe = dest / 'probe'
            try:
                os.symlink('dir', probe, target_is_directory=True)
            except OSError as e:
                if e.errno == 1 or getattr(e, 'winerror', None) == 1314:
                    self.skipTest(f'symlink creation privilege unavailable: {e}')
                raise
            with archive(('file', tarfile.REGTYPE, b'data'), ('sym', tarfile.SYMTYPE, 'file')) as tf:
                extract_all_from_tarfile_safely(tf, root)
            self.assertEqual((dest / 'sym').read_bytes(), b'data')

    def test_preexisting_destination_symlink(self):
        for is_dir in (False, True):
            with self.subTest(is_dir=is_dir), tempfile.TemporaryDirectory() as root:
                dest, outside = Path(root) / 'dest', Path(root) / 'outside'
                dest.mkdir()
                outside.mkdir()
                victim = outside / 'victim'
                victim.write_bytes(b'unchanged')
                try:
                    os.symlink(outside if is_dir else victim, dest / 'existing', target_is_directory=is_dir)
                except OSError as e:
                    if e.errno == 1 or getattr(e, 'winerror', None) == 1314:
                        self.skipTest(f'symlink creation privilege unavailable: {e}')
                    raise
                entry = 'existing/victim' if is_dir else 'existing'
                with archive((entry, tarfile.REGTYPE, b'changed')) as tf:
                    with self.assertRaises(tarfile.FilterError):
                        extract_all_from_tarfile_safely(tf, str(dest))
                self.assertEqual(victim.read_bytes(), b'unchanged')

    def test_special_files_are_rejected(self):
        for kind in (tarfile.FIFOTYPE, tarfile.CHRTYPE, tarfile.BLKTYPE):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as root:
                with archive(('special', kind, '')) as tf:
                    tf.errorlevel = 0
                    with self.assertRaises(tarfile.SpecialFileError):
                        extract_all_from_tarfile_safely(tf, root)
                    self.assertEqual(tf.errorlevel, 0)
                self.assertFalse((Path(root) / 'special').exists())


if __name__ == '__main__':
    unittest.main()
