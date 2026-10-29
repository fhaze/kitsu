import hashlib
from pathlib import Path
from zipfile import ZipFile

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

TAR_FILTER_CHECK = '''    if os.path.commonpath([target_path, dest_path]) != dest_path:
        raise OutsideDestinationError(member, target_path)
    # Limit permissions (no high bits, and go-w)
'''
TAR_FILTER_FIXED = '''    if os.path.commonpath([target_path, dest_path]) != dest_path:
        raise OutsideDestinationError(member, target_path)
    # A link must not replace the extraction directory itself.
    if ((member.islnk() or member.issym())
            and os.path.normpath(name) == os.curdir):
        raise OutsideDestinationError(member, target_path)
    # Limit permissions (no high bits, and go-w)
'''


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
    tarfile = (stdlib / 'tarfile.py').read_text(encoding='utf-8')
    if tarfile.count(TAR_FILTER_CHECK) != 1 or TAR_FILTER_FIXED in tarfile:
        raise ValueError('The MSYS2 tarfile source changed; review the CPython security backport')
    # CPython 3.14 fix: https://github.com/python/cpython/commit/0901ad4ffb22605f12b705d1c1197f0a01f0ba0b
    tarfile = tarfile.replace(TAR_FILTER_CHECK, TAR_FILTER_FIXED)
    return {'poplib.py': poplib, 'tempfile.py': tempfile, 'tarfile.py': tarfile}


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
            *(f'lib/{stdlib.name}/{name}' for stdlib in (package / 'lib').glob('python*') for name in ('poplib.py', 'tempfile.py', 'tarfile.py')),
        ]
        for name in required:
            if f'{package.name}/{name}' not in z.namelist():
                raise ValueError(f'The ZIP is missing a security-patched runtime file: {name}')
