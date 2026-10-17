#!/usr/bin/env python
"""Regenerate the Windows ICO and runtime icon from kitsu.png (requires Pillow)."""

from pathlib import Path

from PIL import Image

base = Path(__file__).resolve().parent
with Image.open(base / 'kitsu.png') as image:
    if image.width != image.height:
        raise ValueError(f'kitsu.png must be square, got {image.width}x{image.height}')
    image = image.convert('RGBA')
    image.save(base / 'kitsu.ico', sizes=[(n, n) for n in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)])
    image.resize((128, 128), Image.Resampling.LANCZOS).save(base / 'kitsu-128.png')
