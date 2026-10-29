#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kitsu contributors

import sys
from unittest.mock import MagicMock as Mock
from unittest.mock import patch

from kitty import default_terminal as dt

from .base import BaseTest


class DefaultTerminalTest(BaseTest):
    def setUp(self):
        super().setUp()
        p = patch.object(sys, 'platform', 'win32')
        p.start()
        self.addCleanup(p.stop)

    def test_delegation_pair(self):
        self.assertTrue(dt.is_kitsu((dt.CONSOLE_CLSID.lower(), dt.TERMINAL_CLSID.lower())))
        self.assertFalse(dt.is_kitsu((None, None)))
        self.assertFalse(dt.is_kitsu((dt.AUTOMATIC_CLSID, dt.TERMINAL_CLSID)))

    def test_opt_in_and_restore(self):
        registry = Mock(REG_SZ=1)
        previous = (None, '{OTHER-TERMINAL}')
        with (
            patch.dict(sys.modules, winreg=registry),
            patch.object(dt, 'read_delegation', return_value=previous),
            patch.object(dt, 'write_delegation') as write,
        ):
            dt.enable()
            write.assert_called_once_with((dt.CONSOLE_CLSID, dt.TERMINAL_CLSID))
            self.ae(registry.SetValueEx.call_args.args[-1], '[null, "{OTHER-TERMINAL}"]')
        with (
            patch.dict(sys.modules, winreg=registry),
            patch.object(dt, 'read_delegation', return_value=previous),
            patch.object(dt, 'write_delegation') as write,
        ):
            dt.restore()
            write.assert_not_called()

    def test_repeated_enable_preserves_backup(self):
        registry = Mock(REG_SZ=1)
        with patch.dict(sys.modules, winreg=registry), patch.object(dt, 'read_delegation', return_value=(dt.CONSOLE_CLSID, dt.TERMINAL_CLSID)):
            dt.enable()
            registry.SetValueEx.assert_not_called()

    def test_restore_missing_values(self):
        registry = Mock(REG_SZ=1)
        registry.OpenKey.return_value.__enter__ = Mock(return_value=registry)
        registry.OpenKey.return_value.__exit__ = Mock(return_value=False)
        registry.QueryValueEx.return_value = ('[null, null]', 1)
        with (
            patch.dict(sys.modules, winreg=registry),
            patch.object(dt, 'read_delegation', return_value=(dt.CONSOLE_CLSID, dt.TERMINAL_CLSID)),
            patch.object(dt, 'write_delegation') as write,
        ):
            dt.restore()
            write.assert_called_once_with((None, None))

    def test_enable_rolls_back_failed_write(self):
        registry = Mock(REG_SZ=1)
        previous = ('old-console', 'old-terminal')
        with (
            patch.dict(sys.modules, winreg=registry),
            patch.object(dt, 'read_delegation', return_value=previous),
            patch.object(dt, 'write_delegation', side_effect=[OSError('write failed'), None]) as write,
        ):
            with self.assertRaises(OSError):
                dt.enable()
            self.ae(write.call_args_list[-1].args, (previous,))
