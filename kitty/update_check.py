#!/usr/bin/env python
# License: GPLv3 Copyright: 2019, Kovid Goyal <kovid at kovidgoyal.net>

import json
import math
import os
import subprocess
import time
from contextlib import suppress
from collections.abc import Callable
from typing import NamedTuple

from .config import atomic_save
from .constants import Version, cache_dir, helper_process_popen_kwargs, is_windows, kitty_exe, version, website_url
from .fast_data_types import add_timer, get_boss, monitor_pid
from .update import fetch_release, installed_release_info, is_newer, parse_release_tag
from .utils import log_error, open_url

FORK_RELEASES_URL = 'https://github.com/fhaze/kitsu/releases/latest'
CHANGELOG_URL = website_url('changelog')
RELEASED_VERSION_URL = website_url() + 'current-version.txt'
CHECK_INTERVAL = 24 * 60 * 60.0
NOTIFY_COOLDOWN = 3 * 24 * 60 * 60.0


class Notification(NamedTuple):
    version: str
    time_of_last_notification: float
    notification_count: int


def notification_activated() -> None:
    open_url(FORK_RELEASES_URL if is_windows else CHANGELOG_URL)


def version_notification_log() -> str:
    override = getattr(version_notification_log, 'override', None)
    if isinstance(override, str):
        return override
    return os.path.join(cache_dir(), 'new-version-notifications-1.txt')


def notify_new_version(
    release_version: str, on_created: Callable[[], None], on_creation_failed: Callable[[], None] | None = None,
) -> None:
    get_boss().notification_manager.send_new_version_notification(release_version, on_created, on_creation_failed)


def update_check_cache_path() -> str:
    override = getattr(update_check_cache_path, 'override', None)
    return override if isinstance(override, str) else os.path.join(cache_dir(), 'kitsu-update-check.json')


class UpdateCheckState(NamedTuple):
    last_check: float = 0
    latest_tag: str = ''
    last_notified: float = 0
    last_notified_tag: str = ''


def read_update_check_state() -> UpdateCheckState:
    try:
        with open(update_check_cache_path(), encoding='utf-8') as f:
            data = json.load(f)
        if not isinstance(data, dict):
            return UpdateCheckState()
        fields = {}
        for key in ('last_check', 'last_notified'):
            value = data.get(key, 0)
            fields[key] = value if isinstance(value, (int, float)) and math.isfinite(value) and value >= 0 else 0
        for key in ('latest_tag', 'last_notified_tag'):
            value = data.get(key, '')
            fields[key] = value if isinstance(value, str) and parse_release_tag(value) else ''
        return UpdateCheckState(**fields)
    except (OSError, ValueError, TypeError):
        return UpdateCheckState()


def save_update_check_state(state: UpdateCheckState) -> None:
    try:
        atomic_save(json.dumps(state._asdict()).encode('utf-8'), update_check_cache_path())
    except OSError as e:
        log_error(f'Failed to save update check state: {e}')


def notify_cached_release(state: UpdateCheckState) -> None:
    info = parse_release_tag(state.latest_tag)
    previous = parse_release_tag(state.last_notified_tag)
    now = time.time()
    if (
        info is None or not is_newer(info, installed_release_info()) or (previous is not None and not is_newer(info, previous))
        or (state.last_notified and now - state.last_notified < NOTIFY_COOLDOWN)
    ):
        return

    previous_state: UpdateCheckState | None = None
    notified_at = 0.0

    def created() -> None:
        nonlocal previous_state, notified_at
        current = read_update_check_state()
        previous_state, notified_at = current, time.time()
        save_update_check_state(current._replace(last_notified=notified_at, last_notified_tag=info.tag))

    def failed() -> None:
        current = read_update_check_state()
        if previous_state is not None and current.last_notified == notified_at and current.last_notified_tag == info.tag:
            save_update_check_state(current._replace(last_notified=previous_state.last_notified, last_notified_tag=previous_state.last_notified_tag))

    notify_new_version(info.tag.lstrip('v'), created, failed)


def get_released_version() -> str:
    if is_windows:
        try:
            return str(fetch_release('latest')['tag_name'])
        except (Exception, SystemExit):
            return ''
    from urllib.request import urlopen

    try:
        raw = urlopen(RELEASED_VERSION_URL).read().decode('utf-8').strip()
    except Exception:
        raw = '0.0.0'
    return str(raw)


def parse_line(line: str) -> Notification:
    version, timestamp, count = line.split(',')
    return Notification(version, float(timestamp), int(count))


def read_cache() -> dict[str, Notification]:
    notified_versions = {}
    with suppress(FileNotFoundError):
        with open(version_notification_log()) as f:
            for line in f:
                try:
                    n = parse_line(line)
                except Exception:
                    continue
                notified_versions[n.version] = n
    return notified_versions


def already_notified(key: str) -> bool:
    return key in read_cache()


def save_notification(key: str) -> None:
    notified_versions = read_cache()
    if key in notified_versions:
        v = notified_versions[key]
        notified_versions[key] = v._replace(time_of_last_notification=time.time(), notification_count=v.notification_count + 1)
    else:
        notified_versions[key] = Notification(key, time.time(), 1)
    lines = []
    for k in sorted(notified_versions):
        n = notified_versions[k]
        lines.append(f'{n.version},{n.time_of_last_notification},{n.notification_count}')
    atomic_save('\n'.join(lines).encode('utf-8'), version_notification_log())


def process_current_release(raw: str) -> None:
    if is_windows:
        info = parse_release_tag(raw)
        if info is not None:
            state = read_update_check_state()._replace(last_check=time.time(), latest_tag=info.tag)
            save_update_check_state(state)
            notify_cached_release(state)
        return
    release_version = Version(*tuple(map(int, raw.split('.'))))
    if release_version > version and not already_notified(raw):
        notify_new_version('.'.join(map(str, release_version)), lambda: save_notification(raw))


def run_worker() -> None:
    with suppress(BrokenPipeError):  # happens if parent process is killed before us
        print(get_released_version())


def update_check(interval: float = CHECK_INTERVAL) -> bool:
    if is_windows:
        state = read_update_check_state()
        if 0 <= time.time() - state.last_check < interval:
            notify_cached_release(state)
            return True
    if get_boss().update_check_process is not None:
        return True
    try:
        p = subprocess.Popen(
            [kitty_exe(), '+runpy', 'from kitty.update_check import run_worker; run_worker()'], stdout=subprocess.PIPE, **helper_process_popen_kwargs()
        )
    except Exception as e:
        log_error(f'Failed to run kitty for update check, with error: {e}')
        return False
    monitor_pid(p.pid)
    get_boss().set_update_check_process(p)
    return True


def run_update_check(interval: float = CHECK_INTERVAL) -> None:
    def update_check_callback(timer_id: int | None) -> None:
        update_check(interval)

    update_check(interval)
    add_timer(update_check_callback, interval)
