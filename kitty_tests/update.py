#!/usr/bin/env python
# License: GPLv3 Copyright: 2025, Kovid Goyal <kovid at kovidgoyal.net>

from kitty.constants import Version
from kitty.update import ReleaseInfo, is_newer, parse_release_tag, windows_setup_asset

from .base import BaseTest


class TestUpdate(BaseTest):
    def test_windows_setup_asset(self):
        for name in ('kitsu-0.49.2-windows-x86_64-setup.exe', 'kitty-0.49.2-windows-x86_64-setup.exe'):
            asset = {'name': name, 'browser_download_url': 'https://example.org/' + name, 'digest': 'sha256:' + 'a' * 64}
            release = {'tag_name': 'v0.49.2-windows.1', 'assets': [asset]}
            self.assertEqual(windows_setup_asset(release), (name, asset['browser_download_url'], 'a' * 64))
            asset.pop('digest')
            with self.assertRaisesRegex(SystemExit, 'no SHA-256 digest'):
                windows_setup_asset(release)
        for name in ('kitsu-0.49.2-windows-arm64-setup.exe', 'kitsu-0.49.2-windows-x86_64.zip'):
            with self.assertRaisesRegex(SystemExit, 'no Windows Kitsu installer'):
                windows_setup_asset({'assets': [{'name': name}]})

    def test_parse_release_tag(self):
        def t(tag, base, build):
            info = parse_release_tag(tag)
            self.assertIsNotNone(info, tag)
            self.assertEqual(info.base, base, tag)
            self.assertEqual(info.build, build, tag)
            self.assertEqual(info.tag, tag)

        t('v0.48.2-windows.12', Version(0, 48, 2), 12)
        t('0.48.2', Version(0, 48, 2), 0)
        t('v0.49.0', Version(0, 49, 0), 0)
        t('v0.48.2-windows.0', Version(0, 48, 2), 0)
        for bad in ('nightly', 'v1.2', 'v0.48.2-windows.x', '', 'v0.48.2.1', 'v0.48.2-beta'):
            self.assertIsNone(parse_release_tag(bad), bad)

    def test_is_newer(self):
        def ri(major, minor, patch, build=0):
            return ReleaseInfo(Version(major, minor, patch), build, '')

        self.assertTrue(is_newer(ri(0, 49, 0), ri(0, 48, 2, 99)))
        self.assertTrue(is_newer(ri(0, 48, 2, 13), ri(0, 48, 2, 12)))
        self.assertFalse(is_newer(ri(0, 48, 2, 12), ri(0, 48, 2, 12)))
        self.assertFalse(is_newer(ri(0, 48, 2), ri(0, 48, 2)))
        self.assertFalse(is_newer(ri(0, 48, 1, 99), ri(0, 48, 2)))
        self.assertFalse(is_newer(ri(0, 48, 2, 11), ri(0, 48, 2, 12)))
