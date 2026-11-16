"""Offline regressions for the rebuilt CPython extensions, including FIPS policy."""

import os
import sys
from pathlib import Path

sys.path.insert(0, os.environ['KITSU_SECURITY_TEST_MODULE'])
import _hashlib
import _socket
import ctypes
import socket
import ssl
import subprocess
import unittest


class PythonSecurityTest(unittest.TestCase):
    def test_exact_extensions(self) -> None:
        for module in (_hashlib, _socket):
            with self.subTest(module=module.__name__):
                self.assertEqual(Path(module.__file__).parent.resolve(), Path(os.environ['KITSU_SECURITY_TEST_MODULE']).resolve())

    def test_getaddrinfo_rejects_null_bytes(self) -> None:
        for getaddrinfo in (_socket.getaddrinfo, socket.getaddrinfo):
            for host in ('local\0host', b'local\0host', 'localhost\0', b'localhost\0', '\0localhost', b'\0localhost', '\0', b'\0', 'b\xfccher\0.example'):
                with self.subTest(host=host, api=getaddrinfo), self.assertRaisesRegex(ValueError, 'host'):
                    getaddrinfo(host, 80)
            for port in ('8\x000', b'8\x000', 'http\0', b'http\0', '\0', b'\0'):
                with self.subTest(port=port, api=getaddrinfo), self.assertRaisesRegex(ValueError, 'port'):
                    getaddrinfo('127.0.0.1', port)

    def test_ordinary_socket_and_hash_operations(self) -> None:
        for port in (80, '80', b'80', None):
            self.assertTrue(socket.getaddrinfo('127.0.0.1', port, socket.AF_INET, socket.SOCK_STREAM, 0, socket.AI_NUMERICHOST))
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.settimeout(5)
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            with socket.create_connection(listener.getsockname(), timeout=5) as client:
                peer, _ = listener.accept()
                with peer:
                    peer.settimeout(5)
                    client.sendall(b'private socket module')
                    self.assertEqual(peer.recv(1024), b'private socket module')
        self.assertEqual(_hashlib.openssl_sha256(b'abc').hexdigest(), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')

    def test_fips_policy_in_fresh_process(self) -> None:
        launcher = '+runpy' if Path(sys.executable).stem.lower() in ('kitsu', 'kitty') else '-c'
        env = dict(os.environ, KITSU_SECURITY_FIPS_PROBE='1')
        subprocess.run(
            [sys.executable, launcher, f'import runpy; runpy.run_path({str(Path(__file__).resolve())!r}, run_name="__main__")'],
            check=True, env=env, timeout=30,
        )


def fips_probe() -> None:
    # No FIPS provider is needed: the required property must reject MD5 even
    # when the only installed provider is the ordinary default provider.
    crypto = ctypes.CDLL(f'libcrypto-{ssl.OPENSSL_VERSION_INFO[0]}-x64.dll')
    enable = crypto.EVP_default_properties_enable_fips
    enable.argtypes = (ctypes.c_void_p, ctypes.c_int)
    enable.restype = ctypes.c_int
    case = unittest.TestCase()
    case.assertEqual(enable(None, 1), 1)
    try:
        case.assertEqual(_hashlib.get_fips_mode(), 1)
        for constructor in (_hashlib.openssl_md5, lambda **kw: _hashlib.new('md5', **kw)):
            for _ in range(2):
                case.assertEqual(constructor(usedforsecurity=False).hexdigest(), 'd41d8cd98f00b204e9800998ecf8427e')
                with case.assertRaises(ValueError):
                    constructor(usedforsecurity=True)
    finally:
        case.assertEqual(enable(None, 0), 1)


if __name__ == '__main__':
    if os.environ.get('KITSU_SECURITY_FIPS_PROBE'):
        fips_probe()
    else:
        unittest.main(argv=[__file__])
