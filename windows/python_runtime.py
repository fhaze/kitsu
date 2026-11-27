import hashlib
from pathlib import Path
from zipfile import ZipFile

STDLIB_FIXES = {
    'CVE-2025-15367': 'Backported CPython POP3 control-character validation: https://github.com/python/cpython/commit/b234a2b67539f787e191d2ef19a7cbdce32874e7',
    'CVE-2026-12345': 'Replaced Windows TemporaryDirectory cleanup with kitten __remove_tree__, using Go >=1.26 handle-relative os.RemoveAll: https://go.dev/cl/661575',
    'CVE-2026-115559': 'Discard pre-TLS IMAP state: https://github.com/python/cpython/pull/160370',
    'CVE-2026-115791': 'Use the CPython 3.14 archive CLI safe-path backport: https://github.com/python/cpython/pull/161706',
    'CVE-2026-124249': 'Apply the CPython 3.14 cookie expiry policy backport: https://github.com/python/cpython/commit/ac1d49f4fbb33941475c5aa3cb663ab39d64cb73',
    'CVE-2026-103995': 'Validate HTTP response lengths against the RFC 9112 grammar: https://github.com/python/cpython/pull/150752',
    'CVE-2026-16052': 'Validate cookie attribute separators before serialization: https://github.com/python/cpython/pull/161711',
    'CVE-2026-19552': 'Check hostnames when get_server_certificate verifies a certificate: https://github.com/python/cpython/pull/161507',
}

ARCHIVE_CLI_IMPORTS = {
    'tarfile.py': 'def main():\n    import argparse\n',
    'zipfile/__init__.py': 'def main(args=None):\n    import argparse\n',
    'gzip.py': 'def main():\n    from argparse import ArgumentParser\n',
}
ARCHIVE_CLI_SAFE_PATH = """\
    # Prevent files extracted by the CLI from shadowing stdlib modules.
    path0 = getattr(sys, '_path0', None)
    if path0 is not None and path0 != sys._stdlib_dir:
        sys.path[:] = [path for path in sys.path if path != path0]
"""

IMAP_STARTTLS = """\
        typ, dat = self._simple_command(name)
        if typ == 'OK':
            self.sock = ssl_context.wrap_socket(self.sock,
"""
IMAP_STARTTLS_FIXED = """\
        typ, dat = self._simple_command(name)
        if typ == 'OK':
            if any(self._readbuf):
                raise self.abort('unexpected data after STARTTLS response')
            # Discard cached responses and close the old plaintext reader.
            self._readbuf = []
            self.untagged_responses = {}
            self._file.close()
            self.sock = ssl_context.wrap_socket(self.sock,
"""
COOKIE_EXPIRY = """\
        elif expires <= self._now:
            # Expiry date in past is request to delete cookie.  This can't be
            # in DefaultCookiePolicy, because can't delete cookies there.
            try:
                self.clear(domain, path, name)
            except KeyError:
                pass
            _debug("Expiring cookie, domain='%s', path='%s', name='%s'",
                   domain, path, name)
            return None

        return Cookie(version,
                      name, value,
                      port, port_specified,
                      domain, domain_specified, domain_initial_dot,
                      path, path_specified,
                      secure,
                      expires,
                      discard,
                      comment,
                      comment_url,
                      rest)
"""
COOKIE_EXPIRY_FIXED = """\

        cookie = Cookie(version,
                        name, value,
                        port, port_specified,
                        domain, domain_specified, domain_initial_dot,
                        path, path_specified,
                        secure,
                        expires,
                        discard,
                        comment,
                        comment_url,
                        rest)

        if expires is not None and expires <= self._now:
            # RFC 6265 requires the domain check before removing a stored cookie.
            if self._policy.set_ok(cookie, request):
                try:
                    self.clear(domain, path, name)
                except KeyError:
                    pass
                _debug("Expiring cookie, domain='%s', path='%s', name='%s'",
                       domain, path, name)
            return None

        return cookie
"""
CONTENT_LENGTH = """\
        length = self.headers.get("content-length")
        if length and not self.chunked:
            try:
                self.length = int(length)
            except ValueError:
                self.length = None
            else:
                if self.length < 0:  # ignore nonsensical negative lengths
                    self.length = None
"""
CONTENT_LENGTH_FIXED = """\
        length = self.headers.get("content-length")
        if length is not None:
            length = length.strip(' \\t')
        if length and not self.chunked and re.fullmatch(r'[0-9]+', length):
            try:
                self.length = int(length)
            except ValueError:
                self.length = None
"""
CHUNK_SIZE = '        try:\n            return int(line, 16)\n'
CHUNK_SIZE_FIXED = """\
        line = line.rstrip()
        try:
            if not re.fullmatch(rb'[0-9a-fA-F]+', line):
                raise ValueError("invalid chunk size")
            return int(line, 16)
"""
COOKIE_ATTRIBUTE_SET = """\
    def __setitem__(self, K, V):
        K = K.lower()
        if not K in self._reserved:
            raise CookieError("Invalid attribute %r" % (K,))
        if _has_control_character(K, V):
            raise CookieError(f"Control characters are not allowed in cookies {K!r} {V!r}")
        dict.__setitem__(self, K, V)

    def setdefault(self, key, val=None):
        key = key.lower()
        if key not in self._reserved:
            raise CookieError("Invalid attribute %r" % (key,))
        if _has_control_character(key, val):
            raise CookieError("Control characters are not allowed in cookies %r %r" % (key, val,))
        return dict.setdefault(self, key, val)
"""
COOKIE_ATTRIBUTE_SET_FIXED = """\
    def _check_attribute(self, key, val):
        key = key.lower()
        if key not in self._reserved:
            raise CookieError(f"Invalid attribute {key!r}")
        val_str = str(val)
        if _has_control_character(key, val_str):
            raise CookieError(f"Control characters are not allowed in cookies {key!r} {val!r}")
        # Comment is quoted like a cookie value and can contain semicolons.
        if key != 'comment' and ';' in val_str:
            raise CookieError(f"Reserved character ';' is not allowed in cookie attribute value {key!r} {val!r}")
        return key

    def __setitem__(self, K, V):
        K = self._check_attribute(K, V)
        dict.__setitem__(self, K, V)

    def setdefault(self, key, val=None):
        key = self._check_attribute(key, val)
        return dict.setdefault(self, key, val)
"""
COOKIE_ATTRIBUTE_UPDATE = """\
            key = key.lower()
            if key not in self._reserved:
                raise CookieError("Invalid attribute %r" % (key,))
            if _has_control_character(key, val):
                raise CookieError("Control characters are not allowed in "
                                  f"cookies {key!r} {val!r}")
            data[key] = val
"""
COOKIE_ATTRIBUTE_UPDATE_FIXED = '            key = self._check_attribute(key, val)\n            data[key] = val\n'
CERTIFICATE_CONTEXT = '                                     cert_reqs=cert_reqs,\n                                     cafile=ca_certs)\n'
CERTIFICATE_CONTEXT_FIXED = (
    '                                     cert_reqs=cert_reqs,\n'
    '                                     check_hostname=ca_certs is not None,\n'
    '                                     cafile=ca_certs)\n'
)
CERTIFICATE_DOC = "    If 'ca_certs' is specified, validate the server cert against it.\n"
CERTIFICATE_DOC_FIXED = (
    "    If 'ca_certs' is specified, validate the server cert against it and\n"
    '    check that it matches the hostname.\n'
)
PATCHED_STDLIB_FILES = (
    'poplib.py', 'tempfile.py', 'imaplib.py', 'http/cookiejar.py',
    'http/client.py', 'http/cookies.py', 'ssl.py', *ARCHIVE_CLI_IMPORTS,
)


POP_COMMAND = """        line = bytes(line, self.encoding)
        self._putline(line)
"""
POP_COMMAND_FIXED = """        line = bytes(line, self.encoding)
        if re.search(b'[\\x00-\\x1F\\x7F]', line):
            raise ValueError('Control characters not allowed in commands')
        self._putline(line)
"""

# Go's handle-relative Windows deletion: https://go.dev/cl/661575
TEMP_CLEANUP = """    @classmethod
    def _rmtree(cls, name, ignore_errors=False, repeated=False):
        helper = _os.path.join(_os.path.dirname(__file__), '..', '..', 'bin', 'kitten.exe')
        env = dict(_os.environ)
        env.pop('KITTY_KITTEN_RUN_MODULE', None)
        try:
            result = _subprocess.run(
                [helper, '__remove_tree__', _os.fsdecode(name)],
                stdout=_subprocess.DEVNULL, stderr=_subprocess.PIPE,
                creationflags=_subprocess.CREATE_NO_WINDOW, env=env,
            )
            if result.returncode:
                raise OSError(_errno.EACCES, result.stderr.decode('utf-8', 'replace'), name)
        except OSError:
            if not ignore_errors:
                raise

"""

TEMP_CLEANUP_SIGNATURES = (
    '    @classmethod\n    def _rmtree(cls, name, ignore_errors=False, repeated=False):\n',
    '    @classmethod\n    def _rmtree(cls, name, ignore_errors=False, repeated=False, dir_fd=None,\n'
    '                fullname=None):\n',
)

TAR_DESTINATION = """    if os.path.commonpath([target_path, dest_path]) != dest_path:
        raise OutsideDestinationError(member, target_path)
    # Limit permissions (no high bits, and go-w)
"""
TAR_DESTINATION_FIXED = """    if os.path.commonpath([target_path, dest_path]) != dest_path:
        raise OutsideDestinationError(member, target_path)
    # A link member named after the destination directory itself
    # would replace it with a (sym)link, redirecting the destination
    # for all subsequent members.
    if ((member.islnk() or member.issym())
            and os.path.normpath(name) == os.curdir):
        raise OutsideDestinationError(member, target_path)
    # Limit permissions (no high bits, and go-w)
"""
TAR_DATA_DESTINATION = """            # A link member that resolves to the destination directory itself
            # would replace it with a (sym)link, redirecting the destination
            # for all subsequent members.
            if target_path == dest_path:
                raise OutsideDestinationError(member, target_path)
"""


def checked_tarfile(source: str) -> str:
    # Reviewed upstream fix in Python 3.14.10: preserve it without reapplying.
    if source.count(TAR_DESTINATION_FIXED) == 1 and TAR_DESTINATION not in source and TAR_DATA_DESTINATION not in source:
        return source
    # Retain the exact reviewed older shape for source-level compatibility.
    if source.count(TAR_DESTINATION) == 1 and source.count(TAR_DATA_DESTINATION) == 1 and TAR_DESTINATION_FIXED not in source:
        return source.replace(TAR_DESTINATION, TAR_DESTINATION_FIXED).replace(TAR_DATA_DESTINATION, '')
    raise ValueError('The MSYS2 tarfile source changed; review the CPython security fix')


def patch_stdlib_source(name: str, source: str, original: str, fixed: str) -> str:
    if source.count(original) != 1 or fixed in source:
        raise ValueError(f'The MSYS2 {name} source changed; review the CPython security backport')
    return source.replace(original, fixed)


def patched_sources(stdlib: Path) -> dict[str, str]:
    poplib = (stdlib / 'poplib.py').read_text(encoding='utf-8')
    if poplib.count(POP_COMMAND) != 1:
        raise ValueError('The MSYS2 poplib source changed; review the CPython security backport')
    poplib = poplib.replace(POP_COMMAND, POP_COMMAND_FIXED)
    tempfile = (stdlib / 'tempfile.py').read_text(encoding='utf-8')
    signatures = [signature for signature in TEMP_CLEANUP_SIGNATURES if tempfile.count(signature) == 1]
    cleanup = '    @classmethod\n    def _cleanup('
    import_shutil = 'import shutil as _shutil\n'
    if len(signatures) != 1 or tempfile.count(cleanup) != 1 or tempfile.count(import_shutil) != 1:
        raise ValueError('The MSYS2 tempfile source changed; review the CPython security backport')
    start = tempfile.index(signatures[0])
    end = tempfile.index(cleanup)
    if end <= start:
        raise ValueError('The MSYS2 tempfile source changed; review the CPython security backport')
    tempfile = tempfile[:start] + TEMP_CLEANUP + tempfile[end:]
    tempfile = tempfile.replace(import_shutil, import_shutil + 'import subprocess as _subprocess\n', 1)
    # CPython 3.14 backport: https://github.com/python/cpython/commit/0901ad4
    tarfile = checked_tarfile((stdlib / 'tarfile.py').read_text(encoding='utf-8'))
    sources = {'poplib.py': poplib, 'tempfile.py': tempfile, 'tarfile.py': tarfile}
    for name, original, fixed in (
        ('imaplib.py', IMAP_STARTTLS, IMAP_STARTTLS_FIXED),
        ('http/cookiejar.py', COOKIE_EXPIRY, COOKIE_EXPIRY_FIXED),
        ('http/client.py', CONTENT_LENGTH, CONTENT_LENGTH_FIXED),
        ('http/client.py', CHUNK_SIZE, CHUNK_SIZE_FIXED),
        ('http/cookies.py', COOKIE_ATTRIBUTE_SET, COOKIE_ATTRIBUTE_SET_FIXED),
        ('http/cookies.py', COOKIE_ATTRIBUTE_UPDATE, COOKIE_ATTRIBUTE_UPDATE_FIXED),
        ('ssl.py', CERTIFICATE_CONTEXT, CERTIFICATE_CONTEXT_FIXED),
        ('ssl.py', CERTIFICATE_DOC, CERTIFICATE_DOC_FIXED),
        *((name, original, original + ARCHIVE_CLI_SAFE_PATH) for name, original in ARCHIVE_CLI_IMPORTS.items()),
    ):
        source = sources.get(name)
        if source is None:
            source = (stdlib / name).read_text(encoding='utf-8')
        sources[name] = patch_stdlib_source(name, source, original, fixed)
    return sources


def harden_python(stdlib: Path) -> None:
    # CPython POP3 fix: https://github.com/python/cpython/commit/b234a2b67539f787e191d2ef19a7cbdce32874e7
    for name, source in patched_sources(stdlib).items():
        (stdlib / name).write_text(source, encoding='utf-8', newline='\n')


def verify_python(package: Path, original_stdlib: Path) -> None:
    stdlib = package / 'lib' / original_stdlib.name
    for name, expected in patched_sources(original_stdlib).items():
        if (stdlib / name).read_bytes() != expected.encode('utf-8'):
            raise ValueError(f'The shipped Python security backport was not verified: {name}')
    if not (package / 'bin/kitten.exe').is_file():
        raise ValueError('The native Windows cleanup helper is missing')
    from windows.python_ssl import verify

    verify(stdlib, original_stdlib)
    from windows.python_security import verify as verify_native_security

    verify_native_security(stdlib)


def sha256(path: Path) -> str:
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def verify_archive(package: Path, archive: Path) -> None:
    with ZipFile(archive) as z:
        for entry in z.infolist():
            path = Path(entry.filename)
            if path.is_absolute() or '..' in path.parts or path.parts[0] != package.name:
                raise ValueError(f'Unexpected ZIP path: {entry.filename}')
            if not entry.is_dir():
                with z.open(entry) as f:
                    digest = hashlib.sha256()
                    while chunk := f.read(1024 * 1024):
                        digest.update(chunk)
                    expected = digest.hexdigest()
                if sha256(package.parent / path) != expected:
                    raise ValueError(f'The tested package differs from the ZIP: {entry.filename}')
        required = [
            'bin/kitten.exe',
            *(
                f'lib/{stdlib.name}/{name}'
                for stdlib in (package / 'lib').glob('python*')
                for name in PATCHED_STDLIB_FILES
            ),
        ]
        for stdlib in (package / 'lib').glob('python*'):
            extensions = list((stdlib / 'lib-dynload').glob('_ssl*.pyd'))
            if len(extensions) != 1:
                raise ValueError('Unexpected packaged SSL extension layout')
            required.append(extensions[0].relative_to(package).as_posix())
            for module in ('_socket', '_hashlib'):
                extensions = list((stdlib / 'lib-dynload').glob(module + '*.pyd'))
                if len(extensions) != 1:
                    raise ValueError(f'Unexpected packaged {module} extension layout')
                required.append(extensions[0].relative_to(package).as_posix())
            required.append(f'lib/{stdlib.name}/lib-dynload/_security-build.json')
            required.append(f'bin/lib{stdlib.name}.dll')
        for name in required:
            if f'{package.name}/{name}' not in z.namelist():
                raise ValueError(f'The ZIP is missing a security-patched runtime file: {name}')
