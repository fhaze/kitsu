"""Build the signed, opt-in sparse package used for default-terminal discovery."""

import glob
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from xml.sax.saxutils import escape

from windows.signing import sign_command, sign_files


def package_version(version: str, tag: str) -> str:
    revision = 0
    if m := re.fullmatch(rf'v{re.escape(version)}-windows\.(\d+)', tag):
        revision = int(m[1])
    parts = version.split('.')
    if len(parts) != 3 or any(not v.isdecimal() or int(v) > 65535 for v in parts) or revision > 65535:
        raise ValueError(f'Invalid Windows package version: {version} ({tag})')
    return f'{version}.{revision}'


def makeappx() -> str:
    if p := os.environ.get('KITSU_MAKEAPPX') or shutil.which('makeappx.exe'):
        return p
    root = os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')
    candidates = glob.glob(os.path.join(root, 'Windows Kits', '10', 'bin', '*', 'x64', 'makeappx.exe'))
    if not candidates:
        raise SystemExit('Signed terminal identity needs the Windows SDK MakeAppx.exe (or set KITSU_MAKEAPPX)')
    return sorted(candidates, key=lambda p: tuple(map(int, Path(p).parts[-3].split('.'))))[-1]


def build_terminal_package(package: Path, version: str, arch: str) -> None:
    if not sign_command():
        print('Skipping terminal identity: set KITSU_SIGN_COMMAND to build a signed sparse MSIX')
        return
    env = dict(os.environ, KITSU_SIGNED_EXE=str(package / 'bin' / 'kitsu.exe'))
    publisher = (
        subprocess.check_output(
            [
                'powershell.exe',
                '-NoProfile',
                '-NonInteractive',
                '-Command',
                '(Get-AuthenticodeSignature -LiteralPath $env:KITSU_SIGNED_EXE).SignerCertificate.Subject',
            ],
            env=env,
        )
        .decode('utf-8-sig')
        .strip()
    )
    if not publisher:
        raise SystemExit('Cannot determine the terminal package publisher from the signed kitsu.exe')
    destination = package / 'terminal-identity.msix'
    with tempfile.TemporaryDirectory(prefix='kitsu-identity-') as tmp:
        stage = Path(tmp)
        text = Path('windows/terminal.appxmanifest').read_text(encoding='utf-8')
        # Windows package versions have four unsigned 16-bit components.
        text = text.replace('@PUBLISHER@', escape(publisher, {'"': '&quot;'})).replace(
            '@VERSION@', package_version(version, os.environ.get('KITTY_RELEASE_TAG', ''))
        )
        text = text.replace('@ARCH@', {'amd64': 'x64', 'x86_64': 'x64', 'aarch64': 'arm64', 'arm64': 'arm64'}[arch])
        (stage / 'AppxManifest.xml').write_text(text, encoding='utf-8')
        (stage / 'Public').mkdir()
        (stage / 'Assets').mkdir()
        env = dict(os.environ, KITSU_LOGO=str(Path('logo/kitsu.png').resolve()), KITSU_ASSETS=str(stage / 'Assets'))
        subprocess.check_call(
            [
                'powershell.exe',
                '-NoProfile',
                '-NonInteractive',
                '-Command',
                'Add-Type -AssemblyName System.Drawing; $src=[Drawing.Image]::FromFile($env:KITSU_LOGO); '
                'try { foreach ($item in @(@("StoreLogo.png",50),@("Logo.png",150),@("SmallLogo.png",44))) { '
                '$img=New-Object Drawing.Bitmap $item[1],$item[1]; $g=[Drawing.Graphics]::FromImage($img); '
                'try { $g.DrawImage($src,0,0,$item[1],$item[1]); '
                '$img.Save((Join-Path $env:KITSU_ASSETS $item[0]),[Drawing.Imaging.ImageFormat]::Png) '
                '} finally { $g.Dispose(); $img.Dispose() } } } finally { $src.Dispose() }',
            ],
            env=env,
        )
        # Sparse manifests refer to external binaries, so skip MakeAppx's
        # payload existence checks with /nv.
        sdk = Path(makeappx())
        subprocess.check_call([str(sdk), 'pack', '/d', str(stage), '/p', str(destination), '/nv', '/o'])
        manifest = Path('kitty/launcher/windows-app.manifest').read_text(encoding='utf-8')
        identity = (
            '<msix xmlns="urn:schemas-microsoft-com:msix.v1" packageName="FHaze.Kitsu.Terminal" '
            f'applicationId="Kitsu" publisher="{escape(publisher, {chr(34): "&quot;"})}" />'
        )
        (stage / 'launcher.manifest').write_text(manifest.replace('</assembly>', identity + '</assembly>'), encoding='utf-8')
        # Only the COM launcher carries sparse identity. Embedding it in the
        # normal launcher breaks startup on Server without the AppX services.
        exe = package / 'bin' / 'kitsu-handoff.exe'
        shutil.copy2(package / 'bin' / 'kitsu.exe', exe)
        subprocess.check_call([str(sdk.with_name('signtool.exe')), 'remove', '/s', str(exe)])
        subprocess.check_call([str(sdk.with_name('mt.exe')), '-nologo', '-manifest', str(stage / 'launcher.manifest'), f'-outputresource:{exe};#1'])
        sign_files([exe, destination], sign_command())
