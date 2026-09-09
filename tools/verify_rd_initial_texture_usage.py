#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
instance = (ROOT / "src/rendering/rd/terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8")
mesh = (ROOT / "src/rendering/rd/terrain_3d_rd_mesh_cache.cpp").read_text(encoding="utf-8")
lighting = (ROOT / "src/rendering/rd/terrain_3d_rd_forward_lighting.cpp").read_text(encoding="utf-8")
clipmap = (ROOT / "src/rendering/rd/terrain_3d_rd_clipmap_renderer.cpp").read_text(encoding="utf-8")
manager = (ROOT / "src/rendering/rd/terrain_3d_rd_texture_manager.cpp").read_text(encoding="utf-8")

sampling_update = "RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT"
checks = {
    "instance fallback initial upload is update-capable": sampling_update in instance
        and "rTexture = pRd->texture_create( format, view, initial );" in instance,
    "instance fallback creation failure is explicit": "Initial-data fallback texture creation failed" in instance
        and "Failed to create Terrain3D instance fallback texture" in instance,
    "imported material initial upload is update-capable": "layout.m_nUsageBits = ( uint64_t )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT )" in mesh
        and "| ( uint64_t )( RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT );" in mesh,
    "old imported sampling-only override is removed": "layout.m_nUsageBits = ( uint64_t )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT );" not in mesh,
    "forward lighting initial-data fallbacks are update-capable": lighting.count(sampling_update) >= 4
        and lighting.count("texture_create( format, view, initial_data )") >= 4,
    "clipmap initial-data fallbacks are update-capable": "format2d->set_usage_bits( " + sampling_update + " );" in clipmap
        and "format_array->set_usage_bits( " + sampling_update + " );" in clipmap,
    "texture manager rejects invalid initial-upload usage": "Rejected RD texture initial upload without TEXTURE_USAGE_CAN_UPDATE_BIT" in manager
        and "rLayout.m_nUsageBits & ( uint64_t )( RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT )" in manager,
}

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL") + ": " + name)
if failed:
    print(f"RD initial texture usage verifier failed: {len(failed)} check(s)", file=sys.stderr)
    sys.exit(1)
print("PASS: all non-empty cold initial-data fallback/material texture paths retain CAN_UPDATE usage")
