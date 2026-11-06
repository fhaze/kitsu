"""Exercise upstream features and Windows adaptations with the packaged runtime."""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULES = (
    'windows_defaults', 'layout', 'sessions', 'slang', 'graphics', 'keys', 'glfw',
    'parser', 'datatypes', 'notifications', 'open_actions', 'options', 'file_transmission',
    'docks', 'program_status',
)


def run_embedded() -> None:
    import importlib
    import unittest

    sys.path.insert(0, str(ROOT))
    from kitty.fonts.common import all_fonts_map
    from kitty_tests.main import env_for_python_tests

    suite = unittest.TestSuite(
        unittest.defaultTestLoader.loadTestsFromModule(importlib.import_module(f'kitty_tests.{name}'))
        for name in MODULES
    )
    all_fonts_map(True)
    with env_for_python_tests():
        result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())


def main() -> None:
    if sys.argv[1:] == ['--embedded']:
        run_embedded()
    package = Path(sys.argv[1]).resolve()
    script = str(Path(__file__).resolve())
    code = f'import runpy,sys; sys.argv=[{script!r}, "--embedded"]; runpy.run_path({script!r},run_name="__main__")'
    env = dict(os.environ)
    env.setdefault('SLANGC', str(package / 'bin/slangc.exe'))
    result = subprocess.run(
        [str(package / 'bin/kitsu.exe'), '+runpy', code], cwd=ROOT, env=env,
        stdout=sys.stdout, stderr=sys.stderr, timeout=600,
    )
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
