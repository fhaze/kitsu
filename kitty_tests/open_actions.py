#!/usr/bin/env python
# License: GPL v3 Copyright: 2018, Kovid Goyal <kovid at kovidgoyal.net>


import os
from contextlib import contextmanager
from unittest.mock import patch

from kitty.utils import get_editor

from .base import BaseTest


@contextmanager
def patch_env(**kw):
    orig = os.environ.copy()
    for k, v in kw.items():
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v
    yield
    os.environ.clear()
    os.environ.update(orig)


class TestOpenActions(BaseTest):
    def test_editor_command_quoting(self):
        from kitty.open_actions import actions_for_url

        self.set_options()
        editor = [r'C:\Program Files\Editor\editor.exe', '--wait', 'argument with spaces']
        with patch('kitty.open_actions.get_editor', return_value=editor):
            actions = tuple(actions_for_url('file:///tmp/moo.txt', 'protocol file\naction launch $EDITOR $FILE_PATH'))
        self.ae(actions[0].args, (*editor, '/tmp/moo.txt'))

    def test_parsing_of_open_actions(self):
        from kitty.open_actions import KeyAction, actions_for_url

        self.set_options()
        spec = """
protocol file
mime text/*
fragment_matches .
AcTion launch $EDITOR $FILE_PATH $FRAGMENT
action

protocol file
mime text/*
action ignored

ext py,txt
action one
action two
"""

        def actions(url):
            with patch_env(FILE_PATH='notgood'):
                return tuple(actions_for_url(url, spec))

        def single(url, func, *args):
            acts = actions(url)
            self.ae(len(acts), 1)
            self.ae(acts[0].func, func)
            self.ae(acts[0].args, args)

        single('file://hostname/tmp/moo.txt#23', 'launch', *get_editor(), '/tmp/moo.txt', '23')
        single('some thing.txt', 'ignored')
        self.ae(actions('x:///a.txt'), (KeyAction('one', ()), KeyAction('two', ())))
