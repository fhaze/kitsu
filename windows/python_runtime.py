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


def patched_sources(stdlib: Path) -> dict[str, str]:
    poplib = (stdlib / 'poplib.py').read_text(encoding='utf-8')
    if poplib.count(POP_COMMAND) != 1:
        raise ValueError('The MSYS2 poplib source changed; review the CPython security backport')
    poplib = poplib.replace(POP_COMMAND, POP_COMMAND_FIXED)
    tempfile = (stdlib / 'tempfile.py').read_text(encoding='utf-8')
    start = tempfile.index('    @classmethod\n    def _rmtree(cls, name, ignore_errors=False, repeated=False):\n')
    end = tempfile.index('    @classmethod\n    def _cleanup(', start)
    tempfile = tempfile[:start] + TEMP_CLEANUP + tempfile[end:]
    tempfile = tempfile.replace('import shutil as _shutil\n', 'import shutil as _shutil\nimport subprocess as _subprocess\n', 1)
    return {'poplib.py': poplib, 'tempfile.py': tempfile}


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
        required = ['bin/kitten.exe', *(f'lib/{stdlib.name}/{name}' for stdlib in (package / 'lib').glob('python*') for name in ('poplib.py', 'tempfile.py'))]
        for name in required:
            if f'{package.name}/{name}' not in z.namelist():
                raise ValueError(f'The ZIP is missing a security-patched runtime file: {name}')
