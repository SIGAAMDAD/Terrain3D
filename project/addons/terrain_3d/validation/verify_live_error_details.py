#!/usr/bin/env python3
"""Retain actionable diagnostics for WORLD provider and async RD preview failures."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[4]

def main():
    clip_h = (ROOT / 'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h').read_text()
    clip = (ROOT / 'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp').read_text()
    fwd = (ROOT / 'src/rendering/rd/forward/terrain_3d_forward_pass_preparer.cpp').read_text()
    preview = (ROOT / 'src/rendering/rd/preview/terrain_3d_rd_preview_service.cpp').read_text()
    assert 'getForwardPrepareFailureStage' in clip_h
    for term in ('provider_stage=', 'provider_error=', 'patch_count=', 'material_backend=',
                 'nFailureStage == 5u ? &rServices.m_renderer : NULL'):
        assert term in fwd, term
    for term in ('UCMP WORLD provider could not publish the region map',
                 'UCMP WORLD provider could not write frame uniforms',
                 'UCMP WORLD provider could not write the clipmap patch'):
        assert term in clip, term
    for term in ('preview_source_or_shader_unavailable', 'shared_vt_acquisition',
                 'preview_target_or_draw', 'readback_scheduling', 'stage=readback_size'):
        assert term in preview, term
    addon = ROOT / 'project/addons/terrain_3d'
    found = 0
    for p in addon.rglob('*'):
        if p.suffix not in ('.tscn', '.tres'):
            continue
        text = p.read_text(encoding='utf-8')
        for m in re.finditer(r'\[ext_resource[^\n]*uid="(uid://[^\"]+)"[^\n]*path="(res://[^\"]+)"', text):
            target = m.group(2)
            if not target.startswith('res://addons/terrain_3d/'):
                continue
            uid_path = addon / (target.removeprefix('res://addons/terrain_3d/') + '.uid')
            if uid_path.is_file():
                assert m.group(1) == uid_path.read_text(encoding='utf-8').strip(), str(p)
                found += 1
    assert found > 0
    print(f'PASS: native failure detail contracts and {found} resolved addon UIDs')

if __name__ == '__main__':
    main()
