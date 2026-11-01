"""Verify the upstream-fixed MSYS2 Python runtime and retain its TLS regressions."""

import hashlib
import os
import shutil
import subprocess
import sys
import sysconfig
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CERTIFICATE_FILES = ('distrusted.zip', 'pycacert.pem', 'keycert3.pem')


def require_vendor_runtime() -> None:
    if sys.platform != 'win32' or sysconfig.get_config_var('SOABI') != 'cp314-mingw_x86_64_ucrt_gnu':
        raise ValueError('The Windows runtime requires MSYS2 UCRT64 Python 3.14')
    if sys.version_info[:2] != (3, 14) or sys.version_info[:3] < (3, 14, 10):
        raise ValueError('Upgrade MSYS2 to Python >= 3.14.10 with the upstream TLS and tarfile security fixes')


def sha256(path: Path) -> str:
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def prepare(stdlib: Path) -> None:
    require_vendor_runtime()
    # Use the vendor's upstream regression fixtures, excluded from the package.
    original = Path(sysconfig.get_path('stdlib'))
    certs = ROOT / 'build/python-ssl/certdata'
    certs.mkdir(parents=True, exist_ok=True)
    for name in CERTIFICATE_FILES:
        shutil.copy2(original / 'test/certdata' / name, certs / name)
    env = dict(os.environ, KITSU_SSL_TEST_MODULE=str(stdlib / 'lib-dynload'), KITSU_SSL_TEST_CERTS=str(certs))
    subprocess.run([sys.executable, str(ROOT / 'windows/test_python_ssl.py')], check=True, env=env, timeout=60)


def verify(stdlib: Path, original: Path) -> None:
    require_vendor_runtime()
    extension = '_ssl' + sysconfig.get_config_var('EXT_SUFFIX')
    for directory in (stdlib / 'lib-dynload', original / 'lib-dynload'):
        if {p.name for p in directory.glob('_ssl*.pyd')} != {extension}:
            raise ValueError('Unexpected vendor SSL extension layout')
    dll = f'bin/libpython{sysconfig.get_python_version()}.dll'
    for packaged, vendor in (
        (stdlib / 'lib-dynload' / extension, original / 'lib-dynload' / extension),
        (stdlib.parents[1] / dll, original.parents[1] / dll),
    ):
        if sha256(packaged) != sha256(vendor):
            raise ValueError(f'The packaged Python runtime differs from the fixed MSYS2 vendor binary: {packaged.name}')
