#!/usr/bin/env python3

import hashlib
import json
import os
import sys
from functools import cache
from pathlib import Path
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


def runtime_components(package: Path, msys_root: Path, package_prefix: str) -> list[dict[str, object]]:
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
    components = []
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
        components.append({
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
        })
    if not any(component['name'] == 'python' for component in components):
        raise ValueError('The packaged MSYS2 Python runtime was not identified')
    return components


def main() -> None:
    package, output = map(Path, sys.argv[1:])
    msys_root = Path(sys.executable).parent.parent.parent
    components = runtime_components(package, msys_root, os.environ['MINGW_PACKAGE_PREFIX'])
    output.write_text(json.dumps({
        'bomFormat': 'CycloneDX',
        'specVersion': '1.5',
        'version': 1,
        'components': components,
    }, indent=2) + '\n', encoding='utf-8')
    print(f'Recorded {len(components)} shipped MSYS2 runtime packages in {output}')


if __name__ == '__main__':
    main()
