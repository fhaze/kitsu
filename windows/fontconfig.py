"""Build the Windows fontconfig dependency with UTF-8, extended-length paths."""

import hashlib
import json
import os
import shutil
import subprocess
import tarfile
from pathlib import Path
from urllib.request import urlopen

VERSION = '2.18.3'
SHA256 = '9ae01e1d53acdef56010c5451cd34aa41d325b2faccd8606448d8fa01b2496b3'
URL = f'https://gitlab.freedesktop.org/fontconfig/fontconfig/-/archive/{VERSION}/fontconfig-{VERSION}.tar.gz'
ROOT = Path(__file__).resolve().parents[1]


def replace(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding='utf-8')
    if old not in text:
        raise ValueError(f'Fontconfig {path.name} changed: {old!r}')
    path.write_text(text.replace(old, new), encoding='utf-8', newline='\n')


def patch(source: Path) -> None:
    src = source / 'src'
    for name in ('win32-compat.c', 'win32-compat.h'):
        shutil.copy2(ROOT / 'kitty' / name, src / name)
    shutil.copy2(ROOT / 'windows/fontconfig-utf8.h', src)
    replace(src / 'fcfreetype.c', '#include "fcint.h"', '#define KITSU_FC_FREETYPE\n#include "fcint.h"')
    replace(src / 'fcint.h', '#endif /* _FC_INT_H_ */', '#include "fontconfig-utf8.h"\n#endif /* _FC_INT_H_ */')
    replace(src / 'meson.build', "  'fcatomic.c',", "  'win32-compat.c',\n  'fcatomic.c',")
    replace(
        source / 'meson.build',
        'c_args = []',
        "c_args = []\nadd_project_arguments('-D_FILE_OFFSET_BITS=64', language: 'c')\n"
        "add_project_link_arguments('-lws2_32', '-liphlpapi', '-lbcrypt', '-ladvapi32', '-lwtsapi32', '-lshell32', language: 'c')",
    )
    # FcStat's Win32 implementation has MAX_PATH buffers and ANSI APIs.
    stat = src / 'fcstat.c'
    text = stat.read_text(encoding='utf-8')
    start = text.index('    WIN32_FILE_ATTRIBUTE_DATA wfad;')
    end = text.index('\n}\n\n#else', start)
    body = '''    if (kitty_win32_stat ((const char *)file, statb) != 0)
        return -1;
    char *full = realpath ((const char *)file, NULL);
    if (!full)
        return -1;
    statb->st_ino = FcStringHash ((const FcChar8 *)full);
    free (full);
    return 0;'''
    stat.write_text(text[:start] + body + text[end:], encoding='utf-8', newline='\n')
    for name in ('fcatomic.c', 'fccompat.c'):
        replace(src / name, '#  define mkdir(path, mode) _mkdir (path)', '/* mkdir is supplied by win32-compat.h. */')
    replace(src / 'fcdefault.c', 'char buf[MAX_PATH + 1];', 'char buf[FC_MAX_FILE_LEN + 1];')
    replace(src / 'fcstr.c', 'if (size == 0)\n\tperror ("GetFullPathName");', 'if (size <= 0 || size >= sizeof (full))\n\treturn NULL;')
    replace(src / 'fccfg.c', '[1000]', '[FC_MAX_FILE_LEN + 128]')
    xml = src / 'fcxml.c'
    replace(xml, 'buffer[1000]', 'buffer[FC_MAX_FILE_LEN + 128]')
    replace(xml, 'szFPath[MAX_PATH + 1]', 'szFPath[FC_MAX_FILE_LEN + 128]')
    replace(xml, 'MAX_PATH - 1 - strlen (szFPath)', 'sizeof (szFPath) - 1 - strlen (szFPath)')
    replace(xml, 'data = malloc (1000);', 'data = malloc (FC_MAX_FILE_LEN + 128);')
    replace(xml, 'GetTempPath (800, (LPSTR)data)', 'GetTempPath (FC_MAX_FILE_LEN, (LPSTR)data)')
    replace(xml, 'rc > 800', 'rc >= FC_MAX_FILE_LEN')
    text = xml.read_text(encoding='utf-8')
    start = text.index('    if (!pGetSystemWindowsDirectory) {')
    end = text.index('\n}\n#endif  // _WIN32', start)
    xml.write_text(
        text[:start] + '    pGetSystemWindowsDirectory = kitsu_fc_windows_dir;\n    pSHGetFolderPathA = kitsu_fc_folder;' + text[end:],
        encoding='utf-8',
        newline='\n',
    )


def build(destination: Path) -> None:
    work = ROOT / 'build/fontconfig'
    work.mkdir(parents=True, exist_ok=True)
    archive = work / f'fontconfig-{VERSION}.tar.gz'
    if not archive.exists():
        with urlopen(URL, timeout=120) as response, archive.open('wb') as out:
            shutil.copyfileobj(response, out)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        archive.unlink()
        raise ValueError('Fontconfig source archive checksum mismatch')
    inputs = [archive, Path(__file__), ROOT / 'windows/fontconfig-utf8.h', ROOT / 'kitty/win32-compat.c', ROOT / 'kitty/win32-compat.h']
    toolchain = subprocess.check_output(['gcc', '-dumpmachine']) + subprocess.check_output(['pkg-config', '--modversion', 'freetype2', 'expat'])
    fingerprint = hashlib.sha256(toolchain + b''.join(p.read_bytes() for p in inputs)).hexdigest()
    stamp = work / 'source.sha256'
    output = work / 'out/libfontconfig-1.dll'
    if not output.exists() or not stamp.exists() or stamp.read_text() != fingerprint:
        for name in (f'fontconfig-{VERSION}', 'out'):
            shutil.rmtree(work / name, ignore_errors=True)
        with tarfile.open(archive) as tar:
            tar.extractall(work, filter='data')
        source = work / f'fontconfig-{VERSION}'
        patch(source)
        subprocess.run(
            [
                'meson',
                'setup',
                str(work / 'out'),
                str(source),
                '--default-library=shared',
                '--buildtype=release',
                '--wrap-mode=nodownload',
                f'--prefix={Path(os.environ["MINGW_PREFIX"]).as_posix()}',
                '-Ddoc=disabled',
                '-Dtests=disabled',
                '-Dtools=disabled',
                '-Dcache-build=disabled',
                '-Dnls=disabled',
            ],
            check=True,
        )
        subprocess.run(['meson', 'compile', '-C', str(work / 'out')], check=True)
        stamp.write_text(fingerprint)
    shutil.copy2(output, destination / output.name)
    shutil.copy2(work / f'fontconfig-{VERSION}/COPYING', destination / 'fontconfig-LICENSE.txt')
    (destination / 'fontconfig-build.json').write_text(
        json.dumps(
            {
                'version': VERSION,
                'source_sha256': SHA256,
                'dll_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
            }
        ),
        encoding='utf-8',
    )
