#!/usr/bin/env python
"""Regenerate the Windows ICO from kitsu.png (requires Pillow)."""

from pathlib import Path

from PIL import Image

base = Path(__file__).resolve().parent
with Image.open(base / 'kitsu.png') as image:
    image.convert('RGBA').save(base / 'kitsu.ico', sizes=[(n, n) for n in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)])
