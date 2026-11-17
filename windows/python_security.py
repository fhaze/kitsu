"""Backport CPython 3.14 socket and hashlib fixes into the private runtime.

Remove these builds when MSYS2 ships the fixes. The installed runtime, including
its upstream-fixed TLS extension, is never modified.
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
METADATA = '_security-build.json'
MODULES = {'_socket': 'socketmodule.c', '_hashlib': '_hashopenssl.c'}
FIXES = {
    'CVE-2026-116566': 'https://github.com/python/cpython/commit/71a0b044789efe2583ff9b9e3d28528e9dd20f7f',
    'CVE-2026-119170': 'https://github.com/python/cpython/commit/7d9da9208fc3dc126bebca6844dfe312afc43c34',
}


def sha256(path: Path) -> str:
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def recipe_sha256() -> str:
    # Windows builds and Linux scan-input validation use different Git EOLs.
    return hashlib.sha256(Path(__file__).read_text(encoding='utf-8').encode('utf-8')).hexdigest()


def download_source(archive: Path) -> None:
    partial = archive.with_suffix('.download')
    try:
        with urlopen(URL, timeout=120) as response, partial.open('wb') as out:
            shutil.copyfileobj(response, out)
        if sha256(partial) != SHA256:
            raise ValueError('CPython security source archive checksum mismatch')
        partial.replace(archive)
    finally:
        partial.unlink(missing_ok=True)


def replace(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise ValueError('The CPython security backport source changed; review the upstream fix')
    return source.replace(old, new)


def patch_sources(modules: Path) -> None:
    socket = (modules / 'socketmodule.c').read_text(encoding='utf-8')
    # MSYS2's 0017-socketmodule-add-MINGW-support.patch (C/header changes only).
    socket = replace(socket, '#define IPPROTO_ICMP IPPROTO_ICMP', '#ifdef _MSC_VER\n#define IPPROTO_ICMP IPPROTO_ICMP')
    socket = replace(socket, '#define IPPROTO_SCTP IPPROTO_SCTP  // WinSock2 only', '#define IPPROTO_SCTP IPPROTO_SCTP  // WinSock2 only\n#endif')
    socket = replace(socket, '#elif defined(_MSC_VER) && _MSC_VER>1201', '#elif (defined(_MSC_VER) && _MSC_VER>1201) || defined(__MINGW32__)')
    header = modules / 'socketmodule.h'
    header.write_text(
        replace(
            header.read_text(encoding='utf-8'),
            '#  define HAVE_ADDRINFO\n#  define HAVE_SOCKADDR_STORAGE\n',
            '#ifndef __MINGW32__\n#  define HAVE_ADDRINFO\n#  define HAVE_SOCKADDR_STORAGE\n#endif\n',
        ),
        encoding='utf-8', newline='\n',
    )
    # CPython's reviewed 3.14 backport (FIXES above), before any DNS resolution.
    socket = replace(socket, '    const char *hptr, *pptr;\n', '    const char *hptr, *pptr;\n    Py_ssize_t hlen = 0, plen = 0;\n')
    socket = replace(socket, '        hptr = PyBytes_AS_STRING(idna);\n', '        hptr = PyBytes_AS_STRING(idna);\n        hlen = PyBytes_GET_SIZE(idna);\n')
    socket = replace(socket, '        hptr = PyBytes_AsString(hobj);\n', '        hptr = PyBytes_AS_STRING(hobj);\n        hlen = PyBytes_GET_SIZE(hobj);\n')
    socket = replace(
        socket, '    if (PyIndex_Check(pobj)) {\n',
        '    if (hptr != NULL && strlen(hptr) != (size_t)hlen) {\n'
        '        PyErr_SetString(PyExc_ValueError,\n'
        '                        "getaddrinfo() host must not contain null characters");\n'
        '        goto err;\n'
        '    }\n    if (PyIndex_Check(pobj)) {\n',
    )
    for obj in ('pstr', 'pobj'):
        socket = replace(socket, f'        pptr = PyUnicode_AsUTF8({obj});\n', f'        pptr = PyUnicode_AsUTF8AndSize({obj}, &plen);\n')
    socket = replace(socket, '        pptr = PyBytes_AS_STRING(pobj);\n', '        pptr = PyBytes_AS_STRING(pobj);\n        plen = PyBytes_GET_SIZE(pobj);\n')
    socket = replace(
        socket, '#if defined(__APPLE__) && defined(AI_NUMERICSERV)\n',
        '    if (pptr != NULL && strlen(pptr) != (size_t)plen) {\n'
        '        PyErr_SetString(PyExc_ValueError,\n'
        '                        "getaddrinfo() port must not contain null characters");\n'
        '        goto err;\n'
        '    }\n#if defined(__APPLE__) && defined(AI_NUMERICSERV)\n',
    )
    (modules / 'socketmodule.c').write_text(socket, encoding='utf-8', newline='\n')
    path = modules / '_hashopenssl.c'
    source = replace(
        path.read_text(encoding='utf-8'),
        '    int fips = disable_fips_property(py_ht);\n    if (fips == -1) {\n        goto finally;\n    }\n',
        '    int disable_fips = disable_fips_property(py_ht);\n    if (disable_fips == -1) {\n        goto finally;\n    }\n'
        '    int fips = !disable_fips;\n',
    )
    path.write_text(source, encoding='utf-8', newline='\n')


def prepare_source(archive: Path, work: Path) -> Path:
    if sha256(archive) != SHA256:
        raise ValueError('CPython security source archive checksum mismatch')
    source = work / f'Python-{VERSION}'
    if source.exists():
        shutil.rmtree(source)
    with tarfile.open(archive) as tar:
        prefixes = (f'Python-{VERSION}/Modules/', f'Python-{VERSION}/LICENSE')
        tar.extractall(work, members=(m for m in tar if m.name.startswith(prefixes)), filter='data')
    patch_sources(source / 'Modules')
    return source


def build(stdlib: Path) -> None:
    if (
        sys.platform != 'win32' or sysconfig.get_config_var('SOABI') != 'cp314-mingw_x86_64_ucrt_gnu'
        or sys.version_info[:3] != (3, 14, 10)
    ):
        raise ValueError('Review/remove the native security backports: they require MSYS2 UCRT64 Python 3.14.10')
    work = ROOT / 'build/python-security'
    work.mkdir(parents=True, exist_ok=True)
    archive = work / f'Python-{VERSION}.tar.xz'
    if not archive.exists():
        download_source(archive)
    source = prepare_source(archive, work)
    include = Path(sysconfig.get_path('include'))
    destination = stdlib / 'lib-dynload'
    extensions = {}
    for name, filename in MODULES.items():
        extension = name + sysconfig.get_config_var('EXT_SUFFIX')
        if {p.name for p in destination.glob(name + '*.pyd')} != {extension}:
            raise ValueError(f'Unexpected packaged {name} extension layout')
        output = work / extension
        subprocess.run(
            [
                'gcc', '-shared', '-O2', '-Wall', '-fno-strict-aliasing', '-D_WIN32_WINNT=0x0601',
                f'-I{include}', f'-I{include / "internal"}', f'-I{source / "Modules"}',
                str(source / 'Modules' / filename), f'-L{sysconfig.get_config_var("LIBDIR")}',
                '-lpython3.14', '-lcrypto', '-lws2_32', '-liphlpapi', '-lrpcrt4', '-o', str(output),
            ],
            check=True,
        )
        shutil.copy2(output, destination / extension)
        extensions[extension] = sha256(output)
    env = dict(os.environ, KITSU_SECURITY_TEST_MODULE=str(destination))
    subprocess.run([sys.executable, str(ROOT / 'windows/test_python_security.py')], check=True, env=env, timeout=60)
    shutil.copy2(source / 'LICENSE', destination / '_security-LICENSE.txt')
    (destination / METADATA).write_text(
        json.dumps(
            {
                'source_version': VERSION, 'source_sha256': SHA256, 'recipe_sha256': recipe_sha256(),
                'python_version': list(sys.version_info[:3]), 'extensions': extensions,
            }, indent=2,
        ) + '\n', encoding='utf-8',
    )


def verify(stdlib: Path) -> list[Path]:
    directory = stdlib / 'lib-dynload'
    info = json.loads((directory / METADATA).read_text(encoding='utf-8'))
    extensions = {name + sysconfig.get_config_var('EXT_SUFFIX') for name in MODULES}
    if (
        info.get('source_version') != VERSION or info.get('source_sha256') != SHA256
        or info.get('recipe_sha256') != recipe_sha256() or info.get('python_version') != list(sys.version_info[:3])
        or set(info.get('extensions', {})) != extensions
        or {p.name for name in MODULES for p in directory.glob(name + '*.pyd')} != extensions
        or any(sha256(directory / name) != info['extensions'][name] for name in extensions)
    ):
        raise ValueError('The packaged native Python security fixes do not match their tested build')
    return [directory / name for name in sorted(extensions)]
