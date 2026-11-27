#!/usr/bin/env python3

import hashlib
import json
import os
import re
import subprocess
import sys
import sysconfig
from datetime import datetime, timezone
from functools import cache
from pathlib import Path
from typing import Any
from urllib.parse import quote


def read_fields(path: Path) -> dict[str, list[str]]:
    fields: dict[str, list[str]] = {}
    key = ''
    for line in path.read_text(encoding='utf-8').splitlines():
        if line.startswith('%') and line.endswith('%'):
            key = line.strip('%')
            fields[key] = []
        elif key and line:
            fields[key].append(line)
    return fields


def runtime_components(package: Path, msys_root: Path, package_prefix: str) -> list[dict[str, Any]]:
    @cache
    def digest(path: Path) -> bytes:
        with path.open('rb') as f:
            return hashlib.file_digest(f, 'sha256').digest()

    binaries: dict[str, list[Path]] = {}
    for path in package.rglob('*'):
        if path.is_file() and path.suffix.lower() in ('.dll', '.exe', '.pyd'):
            binaries.setdefault(path.name.lower(), []).append(path)
    if not binaries:
        raise ValueError(f'No packaged Windows binaries found in {package}')
    components: list[dict[str, Any]] = []
    for entry in sorted((msys_root / 'var/lib/pacman/local').iterdir()):
        if not (entry / 'desc').is_file():
            continue
        fields = read_fields(entry / 'desc')
        name = fields['NAME'][0]
        if not name.startswith(package_prefix + '-'):
            continue
        locations = set()
        for filename in read_fields(entry / 'files').get('FILES', []):
            for path in binaries.get(Path(filename).name.lower(), []):
                source = msys_root / filename
                if source.is_file() and digest(source) == digest(path):
                    locations.add(path.relative_to(package).as_posix())
        if not locations:
            continue
        installed_version = fields['VERSION'][0]
        version = installed_version.split(':')[-1].rsplit('-', 1)[0]
        upstream_name = name.removeprefix(package_prefix + '-')
        components.append(
            {
                'type': 'library',
                'bom-ref': name,
                'name': upstream_name,
                'version': version,
                'purl': f'pkg:generic/{quote(upstream_name, safe="")}@{quote(version, safe="")}',
                'properties': [
                    {'name': 'msys2:package', 'value': name},
                    {'name': 'msys2:version', 'value': installed_version},
                    *({'name': 'kitty:packaged-file', 'value': filename} for filename in sorted(locations)),
                ],
            }
        )
    if not any(component['name'] == 'python' for component in components):
        raise ValueError('The packaged MSYS2 Python runtime was not identified')
    from windows import python_security

    stdlib = package / 'lib/python3.14'
    if (stdlib / 'lib-dynload' / python_security.METADATA).is_file():
        python = next(c for c in components if c['name'] == 'python')
        extensions = python_security.verify(stdlib)
        python['properties'].extend(
            {'name': 'kitty:packaged-file', 'value': p.relative_to(package).as_posix()} for p in extensions
        )
        python['properties'].extend([
            {'name': 'kitty:python-security-source', 'value': python_security.SHA256},
            {'name': 'kitty:python-security-recipe', 'value': python_security.recipe_sha256()},
        ])
    fontconfig_info = package / 'bin/fontconfig-build.json'
    if fontconfig_info.is_file():
        info = json.loads(fontconfig_info.read_text(encoding='utf-8'))
        dll = package / 'bin/libfontconfig-1.dll'
        if digest(dll).hex() != info['dll_sha256']:
            raise ValueError('The source-built fontconfig DLL does not match its build metadata')
        components.append(
            {
                'type': 'library',
                'bom-ref': 'fontconfig-source',
                'name': 'fontconfig',
                'version': info['version'],
                'purl': f'pkg:generic/fontconfig@{quote(info["version"], safe="")}',
                'properties': [
                    {'name': 'kitty:packaged-file', 'value': 'bin/libfontconfig-1.dll'},
                    {'name': 'kitty:source-sha256', 'value': info['source_sha256']},
                ],
            }
        )
    return components


def verify_scan_inputs(archive: Path, runtime: dict[str, Any], binaries: dict[str, Any], vex: dict[str, Any]) -> None:
    from windows import python_security
    from windows.python_runtime import STDLIB_FIXES

    with archive.open('rb') as f:
        version = hashlib.file_digest(f, 'sha256').hexdigest()
    product = f'pkg:generic/kitsu-windows@{version}'
    component = runtime['metadata']['component']
    if component['purl'] != product or component['version'] != version or component['name'] != 'kitsu-windows':
        raise ValueError('The runtime inventory does not describe this ZIP')
    if binaries['source']['name'] != 'kitsu-windows' or binaries['source']['version'] != version:
        raise ValueError('The binary inventory does not describe this ZIP')
    if {s['vulnerability']['name'] for s in vex['statements']} != {*STDLIB_FIXES, *python_security.FIXES}:
        raise ValueError('Unexpected Python security attestations')
    python = next(c for c in runtime['components'] if c['name'] == 'python')
    vendor_version = re.fullmatch(r'3\.14\.(\d+)', python['version'])
    if vendor_version is None or int(vendor_version[1]) < 10:
        raise ValueError('The inventory requires upstream-fixed MSYS2 Python >= 3.14.10')
    python_product = f'pkg:generic/python@{python["version"]}?kitty_package={version}'
    if python['purl'] != python_product:
        raise ValueError('The Python identity is not restricted to this ZIP')
    for statement in vex['statements']:
        if statement['status'] != 'fixed' or statement['products'] != [{'@id': product}, {'@id': python_product}]:
            raise ValueError('The security attestation is not restricted to this fixed ZIP')
    paths = {p['value'] for p in python['properties'] if p['name'] == 'kitty:packaged-file'}
    if not any(p.startswith('lib/python3.14/lib-dynload/_ssl.') and p.endswith('.pyd') for p in paths):
        raise ValueError('The fixed vendor SSL extension was not inventoried')
    properties = {p['name']: p['value'] for p in python['properties'] if p['name'] != 'kitty:packaged-file'}
    if (
        properties.get('kitty:python-security-source') != python_security.SHA256
        or properties.get('kitty:python-security-recipe') != python_security.recipe_sha256()
        or any(not any(p.startswith(f'lib/python3.14/lib-dynload/{name}.') and p.endswith('.pyd') for p in paths) for name in python_security.MODULES)
    ):
        raise ValueError('The native Python security fixes were not inventoried with verified provenance')
    python_binaries = [c for c in binaries['artifacts'] if c['name'].lower() in ('python', 'cpython')]
    if not python_binaries:
        raise ValueError('The Python binaries were not identified')
    for binary in python_binaries:
        locations = {p['path'].lstrip('/') for p in binary['locations']}
        if binary['version'] != python['version'] or not locations or not locations <= paths:
            raise ValueError('An additional Python binary is not covered by the verified runtime fixes')


def main() -> None:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    if sys.argv[1] == '--verify-scan-inputs':
        archive, runtime, binaries, vex = map(Path, sys.argv[2:])
        verify_scan_inputs(archive, *(json.loads(p.read_text(encoding='utf-8')) for p in (runtime, binaries, vex)))
        return
    package, output, archive = map(Path, sys.argv[1:])
    from windows.python_runtime import STDLIB_FIXES, sha256, verify_archive, verify_python
    from windows.python_security import FIXES

    verify_python(package, Path(sysconfig.get_path('stdlib')))
    verify_archive(package, archive)
    build_info = subprocess.check_output(['go', 'version', '-m', str(package / 'bin/kitten.exe')], encoding='utf-8')
    go_version = re.search(r'\bgo1\.(\d+)\.', build_info.splitlines()[0])
    if go_version is None or int(go_version[1]) < 26:
        raise ValueError('The cleanup helper must use Go >= 1.26 with Windows symlink-race protection')
    artifact_version = sha256(archive)
    product = f'pkg:generic/kitsu-windows@{artifact_version}'
    msys_root = Path(sys.executable).parent.parent.parent
    components = runtime_components(package, msys_root, os.environ['MINGW_PACKAGE_PREFIX'])
    python = next(c for c in components if c['name'] == 'python')
    python['purl'] += f'?kitty_package={artifact_version}'
    output.write_text(
        json.dumps(
            {
                'bomFormat': 'CycloneDX',
                'specVersion': '1.5',
                'version': 1,
                'metadata': {'component': {'type': 'application', 'name': 'kitsu-windows', 'version': artifact_version, 'purl': product}},
                'components': components,
            },
            indent=2,
        )
        + '\n',
        encoding='utf-8',
    )
    fixes = {
        **STDLIB_FIXES,
        **{cve: f'Compiled and regression-tested the CPython 3.14 native security backport: {url}' for cve, url in FIXES.items()},
    }
    vex = {
        '@context': 'https://openvex.dev/ns/v0.2.0',
        '@id': f'https://github.com/fhaze/kitsu/security/vex/{artifact_version}',
        'author': 'https://github.com/fhaze/kitsu',
        'timestamp': datetime.now(timezone.utc).isoformat(),
        'version': 1,
        'statements': [
            {'vulnerability': {'name': cve}, 'products': [{'@id': product}, {'@id': python['purl']}], 'status': 'fixed', 'action_statement': detail}
            for cve, detail in fixes.items()
        ],
    }
    output.with_name('windows-runtime.vex.json').write_text(json.dumps(vex, indent=2) + '\n', encoding='utf-8')
    print(f'Recorded {len(components)} shipped MSYS2 runtime packages in {output}')


if __name__ == '__main__':
    main()
