#!/usr/bin/env python
# License: GPLv3 Copyright: 2025, Kovid Goyal <kovid at kovidgoyal.net>

import json
import os
import subprocess
import tempfile
from types import SimpleNamespace
from unittest.mock import Mock, patch

from kitty import update_check
from kitty.constants import Version
from kitty.update import ReleaseInfo, is_newer, parse_release_tag, windows_setup_asset

from .base import BaseTest


class TestUpdate(BaseTest):
    def test_lazy_update_check_imports(self):
        from kitty.constants import kitty_exe

        code = (
            'import sys, kitty.boss; before=set(sys.modules); import kitty.update_check; '
            'added=set(sys.modules)-before; assert not added.intersection({"ssl", "http.client", "urllib.request"}), added'
        )
        result = subprocess.run([kitty_exe(), '+runpy', code], capture_output=True, timeout=30)
        self.ae(result.returncode, 0, result.stderr.decode('utf-8', 'replace'))

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


class TestUpdateCheck(BaseTest):
    def setUp(self):
        super().setUp()
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.now = 2000000.0
        self.notifications = []
        self.boss = SimpleNamespace(
            update_check_process=None, set_update_check_process=Mock(),
            notification_manager=SimpleNamespace(send_new_version_notification=lambda *args: self.notifications.append(args) or len(self.notifications)),
        )
        for target, value in (
            ('is_windows', True), ('get_boss', lambda: self.boss), ('installed_release_info', lambda: parse_release_tag('v0.49.2-windows.1')),
            ('notification_pending', False),
        ):
            p = patch.object(update_check, target, value)
            p.start()
            self.addCleanup(p.stop)
        for p in (
            patch.object(update_check.update_check_cache_path, 'override', os.path.join(self.temp.name, 'state.json'), create=True),
            patch.object(update_check.version_notification_log, 'override', os.path.join(self.temp.name, 'legacy.txt'), create=True),
            patch.object(update_check.time, 'time', lambda: self.now),
        ):
            p.start()
            self.addCleanup(p.stop)

    def test_check_cache_and_creation(self):
        update_check.process_current_release('v0.49.2-windows.9')
        state = update_check.read_update_check_state()
        self.ae((state.last_check, state.latest_tag, state.last_notified_tag), (self.now, 'v0.49.2-windows.9', ''))
        self.ae(self.notifications[0][0], '0.49.2-windows.9')
        with patch.object(update_check.subprocess, 'Popen') as spawn:
            self.assertTrue(update_check.update_check())
            spawn.assert_not_called()
        self.ae(len(self.notifications), 1)
        self.notifications[-1][1]()
        self.ae(update_check.read_update_check_state().last_notified_tag, 'v0.49.2-windows.9')
        self.notifications.clear()
        update_check.process_current_release('v0.49.2-windows.9')
        self.assertFalse(self.notifications)

    def test_failed_creation_retries_and_async_failure_rolls_back(self):
        update_check.process_current_release('v0.49.2-windows.9')
        self.notifications[0][2]()
        self.ae(update_check.read_update_check_state().last_notified, 0)
        update_check.notify_cached_release(update_check.read_update_check_state())
        self.ae(len(self.notifications), 2)
        self.notifications[-1][1]()
        self.assertTrue(update_check.read_update_check_state().last_notified)
        self.notifications[-1][2]()
        self.ae(update_check.read_update_check_state().last_notified_tag, '')

    def test_cooldown_announces_only_latest(self):
        update_check.process_current_release('v0.49.2-windows.2')
        self.notifications[-1][1]()
        self.notifications.clear()
        for tag in ('v0.49.2-windows.3', 'v0.49.2-windows.9'):
            update_check.process_current_release(tag)
        self.assertFalse(self.notifications)
        self.now += update_check.NOTIFY_COOLDOWN
        update_check.notify_cached_release(update_check.read_update_check_state())
        self.ae([n[0] for n in self.notifications], ['0.49.2-windows.9'])
        self.notifications[-1][1]()
        self.notifications.clear()
        self.now += update_check.NOTIFY_COOLDOWN
        update_check.process_current_release('v0.49.2-windows.3')
        self.assertFalse(self.notifications)

    def test_old_builds_and_legacy_dropped_notifications(self):
        update_check.process_current_release('v0.49.2-windows.1')
        update_check.process_current_release('v0.48.2-windows.99')
        self.assertFalse(self.notifications)
        update_check.save_notification('v0.49.2-windows.9')
        update_check.process_current_release('v0.49.2-windows.9')
        self.ae(len(self.notifications), 1)

    def test_invalid_cache_and_release(self):
        for raw in ('broken', 'null', '[]', '{"last_check": "wrong", "last_notified": -1, "latest_tag": 7}', '{"last_check": NaN}'):
            with open(update_check.update_check_cache_path(), 'w') as f:
                f.write(raw)
            self.ae(update_check.read_update_check_state(), update_check.UpdateCheckState())
        update_check.process_current_release('not a release')
        self.assertFalse(self.notifications)
        self.ae(update_check.read_update_check_state().last_check, 0)

    def test_stale_cache_and_repeat_timer(self):
        update_check.save_update_check_state(update_check.UpdateCheckState(self.now - 3600, 'v0.49.2-windows.1'))
        process = SimpleNamespace(pid=1234)
        with patch.object(update_check.subprocess, 'Popen', return_value=process) as spawn, patch.object(update_check, 'monitor_pid') as monitor:
            with patch.object(update_check, 'add_timer') as timer:
                update_check.run_update_check(3600)
                self.ae(spawn.call_count, 1)
                monitor.assert_called_once_with(1234)
                self.boss.set_update_check_process.assert_called_once_with(process)
                repeat, interval = timer.call_args.args
                self.ae(interval, 3600)
                repeat(None)
                self.ae(spawn.call_count, 2)
        with patch.object(update_check.subprocess, 'Popen', side_effect=OSError('offline')), patch.object(update_check, 'add_timer') as timer:
            update_check.run_update_check(3600)
            timer.assert_called_once()
        with open(update_check.update_check_cache_path()) as f:
            self.ae(json.load(f)['last_notified'], 0)

    def test_worker_failure_and_in_flight_check(self):
        with patch.object(update_check, 'fetch_release', side_effect=SystemExit('offline')):
            self.ae(update_check.get_released_version(), '')
        self.boss.update_check_process = object()
        with patch.object(update_check.subprocess, 'Popen') as spawn:
            self.assertTrue(update_check.update_check())
            spawn.assert_not_called()

    def test_filtered_notifications_can_retry(self):
        with patch.object(self.boss.notification_manager, 'send_new_version_notification', return_value=None):
            update_check.process_current_release('v0.49.2-windows.9')
        self.assertFalse(update_check.notification_pending)
        self.ae(update_check.read_update_check_state().last_notified, 0)
        update_check.notify_cached_release(update_check.read_update_check_state())
        self.ae(len(self.notifications), 1)

    def test_notification_failure_does_not_overwrite_later_success(self):
        update_check.process_current_release('v0.49.2-windows.9')
        self.notifications[-1][1]()
        current = update_check.read_update_check_state()._replace(last_notified=self.now + 5, last_notified_tag='v0.49.2-windows.10')
        update_check.save_update_check_state(current)
        self.notifications[-1][2]()
        self.ae(update_check.read_update_check_state(), current)
