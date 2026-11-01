"""Build CPython's fixed _ssl extension for the private MSYS2 3.14 runtime.

Remove this workaround when MSYS2 ships Python >= 3.14.10. The source archive
contains the upstream CVE-2026-115557 fix and its offline regression fixture.
Only the package copy is replaced; the installed MSYS2 runtime is untouched.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import sysconfig
import tarfile
from pathlib import Path
from urllib.request import urlopen

VERSION = '3.14.10'
SHA256 = 'de414a57119abef52784d8993e341d53aed635fc2b05c2607d9f98a2c5b72032'
URL = f'https://www.python.org/ftp/python/{VERSION}/Python-{VERSION}.tar.xz'
ROOT = Path(__file__).resolve().parents[1]
METADATA = '_ssl-build.json'


def sha256(path: Path) -> str:
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def prepare_source(archive: Path, work: Path) -> Path:
    if sha256(archive) != SHA256:
        raise ValueError('CPython SSL source archive checksum mismatch')
    source = work / f'Python-{VERSION}'
    # The destination is a fixed directory under our build directory.
    source.resolve().relative_to(work.resolve())
    if source.exists():
        shutil.rmtree(source)
    prefixes = (
        f'Python-{VERSION}/Modules/_ssl',
        f'Python-{VERSION}/Modules/clinic/_ssl',
        f'Python-{VERSION}/Modules/socketmodule.h',
        f'Python-{VERSION}/Lib/test/certdata/',
        f'Python-{VERSION}/LICENSE',
    )
    with tarfile.open(archive) as tar:
        tar.extractall(work, members=(m for m in tar if m.name.startswith(prefixes)), filter='data')
    # MSYS2 patches 0017 and 0059: preserve native Windows certificate stores
    # and the MinGW-configured socket feature definitions.
    # https://github.com/msys2/MINGW-packages/tree/master/mingw-w64-python
    for name in ('_ssl.c', 'clinic/_ssl.c.h'):
        path = source / 'Modules' / name
        text = path.read_text(encoding='utf-8')
        if name == '_ssl.c':
            old = '#ifdef _MSC_VER\n\nstatic PyObject*\ncertEncodingType'
            if text.count(old) != 1:
                raise ValueError('CPython SSL Windows certificate-store source changed')
            text = text.replace(old, old.replace('_MSC_VER', '_WIN32'))
            text = text.replace('#endif /* _MSC_VER */', '#endif /* _WIN32 */')
        else:
            text = text.replace('defined(_MSC_VER)', 'defined(_WIN32)')
        path.write_text(text, encoding='utf-8', newline='\n')
    path = source / 'Modules/socketmodule.h'
    text = path.read_text(encoding='utf-8')
    old = '#  define HAVE_ADDRINFO\n#  define HAVE_SOCKADDR_STORAGE\n'
    if text.count(old) != 1:
        raise ValueError('CPython SSL socket header changed')
    path.write_text(text.replace(old, '#ifndef __MINGW32__\n' + old + '#endif\n'), encoding='utf-8', newline='\n')
    return source


def build(stdlib: Path) -> None:
    if sys.platform != 'win32' or sysconfig.get_config_var('SOABI') != 'cp314-mingw_x86_64_ucrt_gnu':
        raise ValueError('The SSL backport requires MSYS2 UCRT64 Python 3.14')
    if not (3, 14, 7) <= sys.version_info[:3] <= (3, 14, 9):
        raise ValueError('Review/remove the SSL backport for this Python version; MSYS2 Python >= 3.14.10 includes the upstream fix')
    work = ROOT / 'build/python-ssl'
    work.mkdir(parents=True, exist_ok=True)
    archive = work / f'Python-{VERSION}.tar.xz'
    if not archive.exists():
        with urlopen(URL, timeout=120) as response, archive.open('wb') as out:
            shutil.copyfileobj(response, out)
    source = prepare_source(archive, work)
    modules = source / 'Modules'
    include = Path(sysconfig.get_path('include'))
    extension = '_ssl' + sysconfig.get_config_var('EXT_SUFFIX')
    output = work / extension
    subprocess.run(
        [
            'gcc',
            '-shared',
            '-O2',
            '-Wall',
            '-fno-strict-aliasing',
            '-D_WIN32_WINNT=0x0601',
            f'-I{include}',
            f'-I{include / "internal"}',
            f'-I{modules}',
            str(modules / '_ssl.c'),
            f'-L{sysconfig.get_config_var("LIBDIR")}',
            '-lpython3.14',
            '-lssl',
            '-lcrypto',
            '-lws2_32',
            '-lcrypt32',
            '-o',
            str(output),
        ],
        check=True,
    )
    # Import and exercise the exact compiled module in a new interpreter.
    # Compromised-chain regressions use MemoryBIO; ordinary socket TLS is local.
    env = dict(os.environ, KITSU_SSL_TEST_MODULE=str(work), KITSU_SSL_TEST_CERTS=str(source / 'Lib/test/certdata'))
    subprocess.run([sys.executable, str(ROOT / 'windows/test_python_ssl.py')], check=True, env=env, timeout=60)
    destination = stdlib / 'lib-dynload'
    if {p.name for p in destination.glob('_ssl*.pyd')} != {extension}:
        raise ValueError('Unexpected packaged SSL extension layout')
    shutil.copy2(output, destination / extension)
    shutil.copy2(source / 'LICENSE', destination / '_ssl-LICENSE.txt')
    (destination / METADATA).write_text(
        json.dumps(
            {
                'source_version': VERSION,
                'source_sha256': SHA256,
                'python_version': list(sys.version_info[:3]),
                'extension': extension,
                'extension_sha256': sha256(output),
            },
            indent=2,
        )
        + '\n',
        encoding='utf-8',
    )


def verify(stdlib: Path) -> None:
    directory = stdlib / 'lib-dynload'
    info = json.loads((directory / METADATA).read_text(encoding='utf-8'))
    extension = '_ssl' + sysconfig.get_config_var('EXT_SUFFIX')
    if (
        info.get('source_version') != VERSION
        or info.get('source_sha256') != SHA256
        or info.get('python_version') != list(sys.version_info[:3])
        or info.get('extension') != extension
        or {p.name for p in directory.glob('_ssl*.pyd')} != {extension}
        or sha256(directory / extension) != info.get('extension_sha256')
    ):
        raise ValueError('The packaged SSL security backport does not match its tested build')
