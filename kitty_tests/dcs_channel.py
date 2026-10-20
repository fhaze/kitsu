#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

import base64
import json
from contextlib import contextmanager
from unittest.mock import patch

from kitty.boss import Boss
from kitty.utils import log_error

from .base import BaseTest

TOKEN = 'the-dcs-channel-token'
WINDOW_ID = 7


class FakeWindow:
    def __init__(self) -> None:
        self.received: list[bytes] = []

    def handle_kitty_dcs(self, dcs: bytes) -> bool:
        self.received.append(bytes(dcs))
        return True


class Launched(Exception):
    pass


def launched(*a, **kw):
    raise Launched(f'{a} {kw}')


class TestBoss(Boss):
    # Anything that could start a process or open a window
    launch_urls = add_os_window = show_error = launched


def dcs_message(dcs: bytes, token: str = TOKEN, window_id: int = WINDOW_ID) -> bytes:
    # The same bytes tools/utils/dcs_channel.go sends
    return json.dumps(
        {'cmd': 'dcs', 'token': token, 'window_id': window_id, 'dcs': base64.standard_b64encode(dcs).decode()},
        separators=(',', ':'),
    ).encode()


def new_instance_message(**extra) -> bytes:
    # The message from the original report, it ran cmd.exe in an unpatched kitty
    msg = {
        'cmd': 'new_instance',
        'args': ['kitty', 'cmd.exe', '/d', '/c', 'echo', 'x>poc.txt'],
        'environ': {},
        'cwd': '.',
        'session_data': '',
        'session_arg': '',
        'session_path': '',
    }
    msg.update(extra)
    return json.dumps(msg).encode()


class TestDCSChannel(BaseTest):
    def setUp(self) -> None:
        super().setUp()
        self.boss = TestBoss.__new__(TestBoss)
        self.boss.dcs_channel_token = TOKEN
        self.boss.peer_data_map = {}
        self.window = FakeWindow()
        self.boss.window_id_map = {WINDOW_ID: self.window}  # type: ignore
        self.errors: list[str] = []

    @contextmanager
    def nothing_launched(self):
        setattr(log_error, 'redirect', self.errors.append)
        try:
            with (
                patch('kitty.boss.parse_args', launched),
                patch('kitty.boss.create_sessions', launched),
                patch('kittens.panel.main.handle_single_instance_command', launched),
            ):
                yield
        finally:
            delattr(log_error, 'redirect')

    def send(self, msg: bytes, peer_id: int = 1) -> None:
        with self.nothing_launched():
            self.assertIsNone(self.boss.peer_message_received(msg, peer_id, False))

    def assert_rejected(self, msg: bytes) -> None:
        del self.errors[:]
        self.send(msg)
        self.ae(self.window.received, [])
        self.assertTrue(any(e.startswith('Rejected') for e in self.errors), self.errors)

    def test_dcs_channel_rejects_single_instance_commands(self):
        self.assert_rejected(new_instance_message())
        self.assert_rejected(new_instance_message(token=TOKEN))
        self.assert_rejected(new_instance_message(args=['panel', 'cmd.exe']))
        self.assert_rejected(new_instance_message(cmdline_args_for_open=['file:///C:/Windows/System32/cmd.exe']))
        self.assert_rejected(dcs_message(b'@kitty-overlay-ready|', token=''))
        self.assert_rejected(dcs_message(b'@kitty-overlay-ready|', token=TOKEN[:-1]))
        self.assert_rejected(dcs_message(b'@kitty-overlay-ready|', token=TOKEN + 'x'))
        self.assert_rejected(b'\x1bP@kitty-cmd{"cmd":"launch","version":[0,49,2]}\x1b\\')
        self.assert_rejected(b'garbage')
        # The token is valid but JSON lets later keys override earlier ones
        valid_prefix = dcs_message(b'@kitty-overlay-ready|')[:-1]
        self.assert_rejected(valid_prefix + b',' + new_instance_message()[1:])
        self.assert_rejected(valid_prefix + b',"token":"x"}')

    def test_dcs_channel_accepts_tokened_dcs(self):
        self.send(dcs_message(b'@kitty-overlay-ready|'))
        self.send(dcs_message(b'@kitty-kitten-result|abc'))
        self.ae(self.window.received, [b'@kitty-overlay-ready|', b'@kitty-kitten-result|abc'])
        self.ae(self.errors, [])
        self.send(dcs_message(b'@kitty-overlay-ready|', window_id=WINDOW_ID + 1))
        self.ae(len(self.window.received), 2)

    def test_single_instance_socket_without_dcs_channel(self):
        # Without a DCS channel the talk socket is the single instance socket
        # and new_instance must still reach the single instance handler
        self.boss.dcs_channel_token = ''
        with self.nothing_launched(), self.assertRaises(Launched):
            self.boss.peer_message_received(new_instance_message(), 1, False)
        self.send(dcs_message(b'@kitty-overlay-ready|'))
        self.ae(self.window.received, [])
