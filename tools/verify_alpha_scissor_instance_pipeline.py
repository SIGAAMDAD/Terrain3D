#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
instancer = (root / "src/instances/terrain_3d_instancer.cpp").read_text()
texmgr = (root / "src/rendering/rd/terrain_3d_rd_texture_manager.cpp").read_text()
mesh = (root / "src/rendering/rd/terrain_3d_rd_mesh_cache.cpp").read_text()
shader = (root / "project/addons/terrain_3d/rd/terrain_instances.glsl").read_text()

checks = [
    ("planar instance bounds accepted", "rSourceMeshAabb.has_surface()" in instancer),
    ("volume-only planar rejection removed", "rSourceMeshAabb.has_volume()" not in instancer),
    ("BC1 cutout alpha preserved (linear)", "DATA_FORMAT_BC1_RGBA_UNORM_BLOCK" in texmgr),
    ("BC1 cutout alpha preserved (sRGB)", "DATA_FORMAT_BC1_RGBA_SRGB_BLOCK" in texmgr),
    ("negative/invalid Image mip count rejected", "encoded_mipmaps < 0" in texmgr),
    ("mip count bounded by dimensions", "t3dRdMaximumMipCount" in texmgr and "rLayout.m_nMipmaps > maximum_mipmaps" in texmgr),
    ("bad imported mip chain stripped", "upload->clear_mipmaps()" in mesh),
    ("initial-data material texture keeps update usage", "layout.m_nUsageBits = ( uint64_t )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT )" in mesh and "TEXTURE_USAGE_CAN_UPDATE_BIT" in mesh),
    ("bad optional material texture does not abort mesh upload", "retaining mesh with fallback texture" in mesh),
    ("alpha scissor shader discard remains enabled", "MATERIAL_ALPHA_SCISSOR" in shader and "base_color.a < material.pbr.w" in shader),
]

failed = [name for name, ok in checks if not ok]
if failed:
    for name in failed:
        print("FAIL:", name)
    raise SystemExit(1)
for name, _ in checks:
    print("PASS:", name)
print("PASS: alpha-scissor instance pipeline contract")
