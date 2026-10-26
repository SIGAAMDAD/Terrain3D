#!/usr/bin/env python3
"""ST06 no-legacy-FAR retirement gate. Exit 1 while active consumers remain.

This intentionally covers only shipping paths; archival tests/documentation are
excluded. The gate is expected to fail before actual FAR removal.
"""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[5]
checks = {
    "FAR VT consumer": ("src", re.compile(r"\bT3D_VT_CONSUMER_FAR\b")),
    "FAR cooker": ("src", re.compile(r"\bTerrain3DFarRegionalCooker\b")),
    "FAR GPU allocation": ("src", re.compile(r"\bgetFarSurfaceAllocationCold\b|\bm_szFarSurfaceAllocations\b|\bm_nFarSurface")),
    "FAR runtime manifest": ("src", re.compile(r"far_surface_vt=")),
    "FAR shader sampler": ("project/addons/terrain_3d/rd", re.compile(r"\bsample_far_surface_v1\b|\bt3d_far_height_v1\b")),
}

total = 0
for name, (directory, pattern) in checks.items():
    matches = []
    for path in sorted((root / directory).rglob('*')):
        if path.suffix not in {'.cpp', '.h', '.glsl', '.glslinc', '.gdshader', '.gdshaderinc'}:
            continue
        try:
            lines = path.read_text(encoding='utf-8').splitlines()
        except (OSError, UnicodeError):
            continue
        for number, line in enumerate(lines, 1):
            if pattern.search(line) and not line.lstrip().startswith('//'):
                matches.append(f'{path.relative_to(root)}:{number}')
    total += len(matches)
    print(f'{name}: {len(matches)} active references')
    for match in matches[:15]:
        print('  ' + match)
    if len(matches) > 15:
        print(f'  ... {len(matches) - 15} further references')
print(f'ST06 FAR retirement gate: {"PASS" if total == 0 else "BLOCKED"} ({total} references)')
sys.exit(0 if total == 0 else 1)
