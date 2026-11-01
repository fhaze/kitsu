import sys
import unittest
import uuid

if sys.platform == 'win32':
    import winreg

    from windows.test_terminal_handoff import registry


@unittest.skipUnless(sys.platform == 'win32', 'Windows registry')
class HandoffRegistryTest(unittest.TestCase):
    def test_new_parent_keys_are_removed_on_success_and_failure(self):
        for fail in (False, True):
            name = 'KitsuHandoffTest-' + uuid.uuid4().hex
            path = 'Software\\Classes\\' + name
            try:
                with registry(name + '\\Parent\\Leaf', 'value', 'temporary'):
                    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path + '\\Parent\\Leaf') as key:
                        self.assertEqual(winreg.QueryValueEx(key, 'value'), ('temporary', winreg.REG_SZ))
                    if fail:
                        raise RuntimeError('fixture failed')
            except RuntimeError:
                self.assertTrue(fail)
            with self.assertRaises(FileNotFoundError):
                winreg.OpenKey(winreg.HKEY_CURRENT_USER, path)

    def test_existing_keys_and_value_types_are_preserved(self):
        name = 'KitsuHandoffTest-' + uuid.uuid4().hex
        path = 'Software\\Classes\\' + name
        try:
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, path) as key:
                winreg.SetValueEx(key, 'value', 0, winreg.REG_DWORD, 42)
            with registry(name, 'value', 'temporary'):
                with registry(name + '\\Parent\\Leaf', '', 'nested'):
                    pass
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
                self.assertEqual(winreg.QueryValueEx(key, 'value'), (42, winreg.REG_DWORD))
                self.assertEqual(winreg.QueryInfoKey(key)[:2], (0, 1))
        finally:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)


if __name__ == '__main__':
    unittest.main()
