"""Offline CPython SSL regression tests, run during the private module build.

The distrusted.zip fixture and ordinary certificates come from the checksum-
verified CPython source archive. Its public ZIP password is from test_ssl.py.
"""

import contextlib
import os
import socket
import sys
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, os.environ['KITSU_SSL_TEST_MODULE'])
import _ssl  # noqa: E402
import ssl  # noqa: E402

CERTS = Path(os.environ['KITSU_SSL_TEST_CERTS'])


def handshake(client_context: ssl.SSLContext, server_context: ssl.SSLContext, hostname: str = 'localhost') -> str | None:
    ci, co, si, so = (ssl.MemoryBIO() for _ in range(4))
    client = client_context.wrap_bio(ci, co, server_hostname=hostname)
    server = server_context.wrap_bio(si, so, server_side=True)
    for _ in range(10):
        with contextlib.suppress(ssl.SSLWantReadError):
            client.do_handshake()
        if co.pending:
            si.write(co.read())
        with contextlib.suppress(ssl.SSLWantReadError):
            server.do_handshake()
        if so.pending:
            ci.write(so.read())
    client.do_handshake()
    server.do_handshake()
    client.write(b'offline TLS regression')
    si.write(co.read())
    assert server.read() == b'offline TLS regression'
    return client.version()


class PythonSSLTest(unittest.TestCase):
    def test_exact_extension_and_windows_stores(self) -> None:
        self.assertEqual(Path(_ssl.__file__).parent.resolve(), Path(os.environ['KITSU_SSL_TEST_MODULE']).resolve())
        self.assertTrue(callable(ssl.enum_certificates))
        self.assertGreater(ssl.create_default_context().cert_store_stats()['x509_ca'], 0)

    def test_compromised_chain_is_rejected(self) -> None:
        for mode in (ssl.CERT_OPTIONAL, ssl.CERT_REQUIRED):
            for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
                with self.subTest(mode=mode, version=version), tempfile.TemporaryDirectory() as tmp:
                    with zipfile.ZipFile(CERTS / 'distrusted.zip') as z:
                        z.extractall(tmp, pwd=b'Brave Sir Robin')
                    certdir = Path(tmp) / 'distrusted'
                    client = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                    client.verify_mode = mode
                    client.minimum_version = client.maximum_version = version
                    client.load_verify_locations(str(certdir / 'distrusted_root.pem'))
                    server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    server.load_cert_chain(str(certdir / 'distrusted_keycert.pem'))
                    with self.assertRaises(ssl.SSLCertVerificationError) as cm:
                        handshake(client, server)
                    self.assertEqual(cm.exception.verify_code, 28)

    def test_ordinary_verified_tls_and_hostname_check(self) -> None:
        for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
            with self.subTest(version=version):
                client = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                client.load_verify_locations(str(CERTS / 'pycacert.pem'))
                client.minimum_version = client.maximum_version = version
                server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                server.load_cert_chain(str(CERTS / 'keycert3.pem'))
                self.assertEqual(handshake(client, server), version.name.replace('TLSv1_', 'TLSv1.'))
                # A valid chain still requires the requested hostname to match.
                with self.assertRaises(ssl.SSLCertVerificationError):
                    handshake(client, server, 'mismatch.invalid')

    def test_normal_tls_over_local_sockets(self) -> None:
        # Exercise the native _socket C API as used by urllib, as well as BIOs.
        client_context = ssl.create_default_context(cafile=str(CERTS / 'pycacert.pem'))
        client_context.minimum_version = ssl.TLSVersion.TLSv1_2
        server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.minimum_version = ssl.TLSVersion.TLSv1_2
        server_context.load_cert_chain(str(CERTS / 'keycert3.pem'))
        previous_timeout = socket.getdefaulttimeout()
        try:
            socket.setdefaulttimeout(5)
            left, right = socket.socketpair()
        finally:
            socket.setdefaulttimeout(previous_timeout)
        left.settimeout(5)
        right.settimeout(5)
        errors: list[Exception] = []

        def serve() -> None:
            try:
                with server_context.wrap_socket(right, server_side=True) as server:
                    self.assertEqual(server.recv(16), b'hello')
                    server.sendall(b'world')
            except Exception as e:
                errors.append(e)

        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        try:
            with client_context.wrap_socket(left, server_hostname='localhost') as client:
                client.sendall(b'hello')
                self.assertEqual(client.recv(16), b'world')
        finally:
            worker.join(10)
            left.close()
            right.close()
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])


if __name__ == '__main__':
    unittest.main(argv=[__file__], verbosity=2)
