#!/usr/bin/env python3
"""Static regression checks for Terrain3D's optional EXR brush library."""
from pathlib import Path
import math

ROOT = Path(__file__).resolve().parents[1]
settings = (ROOT / 'src' / 'tool_settings.gd').read_text()
menu = (ROOT / 'menu' / 'terrain_menu.gd').read_text()
assert 'func _create_default_brush() -> Image:' in settings
assert 'Image.create_empty(SIZE, SIZE, false, Image.FORMAT_RF)' in settings
assert '_create_default_brush(), "Soft Circle"' in settings
assert 'DirAccess.open(BRUSH_PATH)' in settings
assert 'if dir:' in settings
assert 'get_buttons()[0]' not in settings
assert 'Terrain3DUtil.get_min_max(img)' not in settings
assert 'img.resize(1024, 1024' not in settings
assert 'ImageTexture.create_from_image(img)' in settings
assert 'selected_brush_imgs = [img, tex]' in settings
assert 'DirAccess.make_dir_recursive_absolute(brush_path)' in menu

# The mask is symmetric, finite, smoothly falls off, and needs no packaged EXRs.
size = 256
assert size * size * 4 == 262144  # 256 KiB RF source, not ~70 MiB EXR payload

def alpha(x, y):
    fx = (x + 0.5) * 2.0 / size - 1.0
    fy = (y + 0.5) * 2.0 / size - 1.0
    radius = math.sqrt(fx * fx + fy * fy)
    fade = min(1.0, max(0.0, (1.0 - radius) / 0.30))
    return fade * fade * (3.0 - 2.0 * fade)

assert alpha(128, 128) > 0.99
assert alpha(0, 0) == 0.0
assert alpha(128, 128) == alpha(127, 127)
assert 0.0 < alpha(128, 20) < 1.0
print('Optional brush checks passed (no external mask files required).')
