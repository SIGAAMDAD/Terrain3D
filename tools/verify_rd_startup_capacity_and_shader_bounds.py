#!/usr/bin/env python3
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
coord = (root / "src/rendering/rd/terrain_3d_render_coordinator.cpp").read_text()
coord_h = (root / "src/rendering/rd/terrain_3d_render_coordinator.h").read_text()
contract = (root / "src/rendering/rd/render_world/terrain_3d_render_world_contract.h").read_text()
runtime = (root / "src/rendering/rd/render_world/terrain_3d_render_world_runtime.cpp").read_text()
runtime_rd_dir = root / "project/addons/terrain_3d/rd"
common = (runtime_rd_dir / "render_world_common_inc.glsl").read_text()
raster = (runtime_rd_dir / "render_world_raster.glsl").read_text()
vt_reset = (runtime_rd_dir / "render_world_vt_reset.glsl").read_text()
vt_predict = (runtime_rd_dir / "render_world_vt_predict.glsl").read_text()
vt_compact = (runtime_rd_dir / "render_world_vt_compact.glsl").read_text()
vt_gpu = (root / "src/rendering/rd/render_world/terrain_3d_render_world_vt_gpu.cpp").read_text()
shader_library = (root / "src/rendering/rd/terrain_3d_rd_shader_library.cpp").read_text()
assets_cpp = (root / "src/assets/terrain_3d_assets.cpp").read_text()

errors = []

def require(cond, message):
    if not cond:
        errors.append(message)

require("m_pRuntimeAllocatorZone" in coord_h and "m_bRuntimeAllocatorBound" in coord_h,
        "render coordinator does not retain explicit allocator owner binding")
init = re.search(r"void Terrain3DRenderCoordinator::initialize\([^\{]+\) \{(?P<body>.*?)\n\}", coord, re.S)
require(init is not None, "render coordinator initialize() not found")
if init:
    body = init.group("body")
    require("bindRuntimeAllocator( m_pRuntimeAllocatorZone" in body,
            "initialize() does not restore allocator ownership after shutdown()")
    cap = body.find("serviceColdCapacity();")
    pub = body.find("publishSnapshot();")
    require(cap >= 0 and pub >= 0 and cap < pub,
            "initial publication capacity is not established before publishSnapshot()")
    require(body.count("serviceColdCapacity();") >= 2 and body.count("publishSnapshot();") >= 2,
            "initialization does not retry publication after mesh-source capacity discovery")

require("m_bCachedInstanceMeshesValid" in coord and "mesh_bank_needed = m_bCachedInstanceMeshesValid" in coord,
        "mesh-bank cold sizing is not gated by completed immutable source discovery")
require("static_assert( sizeof( T3DVisibilityFrame ) == 416" in contract,
        "visibility-frame resource-count ABI is not 416 bytes")
for token in ("nMaterialCount", "nVtAllocationCount", "nVtPageCount"):
    require(token in contract and token in runtime, f"missing runtime resource count: {token}")

runtime_shader_files = sorted(runtime_rd_dir.rglob("*.glsl"))
for shader_path in runtime_shader_files:
    shader_source = shader_path.read_text()
    require(re.search(r"\.length\s*\(\s*\)", shader_source) is None,
            f"runtime-array .length() remains in loaded shader source: {shader_path.relative_to(root)}")

require("uvec4 resourceCounts;" in common,
        "runtime RenderWorld common Frame ABI is missing resourceCounts")
for token in ("f.resourceCounts.x", "f.resourceCounts.y", "f.resourceCounts.z"):
    require(token in raster, f"runtime raster shader does not use explicit resource bound {token}")
for token in ("f.resourceCounts.x", "f.resourceCounts.y", "f.resourceCounts.z", "pc.hashCount", "visibleCapacity", "f.counts1.w", "f.counts1.x"):
    require(token in vt_predict, f"runtime VT prediction shader is missing explicit bound {token}")
for token in ("pc.hashCount", "pc.requestCapacity"):
    require(token in vt_compact, f"runtime VT compact shader is missing explicit bound {token}")
require("pc.hashCount" in vt_reset, "runtime VT reset shader is missing explicit hash bound")
require(vt_reset.count("layout(push_constant") == 1 and vt_predict.count("layout(push_constant") == 1
        and vt_compact.count("layout(push_constant") == 1,
        "RenderWorld VT shaders do not share explicit dispatch push bounds")
common_struct_names = set(re.findall(r"\bstruct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", common))
vt_predict_block_names = set(re.findall(r"\bbuffer\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", vt_predict))
require(common_struct_names.isdisjoint(vt_predict_block_names),
        "RenderWorld VT prediction reuses a common struct name as an SSBO block name")
require("buffer VtCounterBuffer {" in vt_predict,
        "RenderWorld VT prediction counter SSBO does not use a collision-safe block name")
require("t3dAssetsSelectPackedSurfaceArrayFormat" in assets_cpp,
        "packed-surface array normalization no longer selects a native compressed canonical format")
require("!bPackedPbr || !t3dAssetsIsNativePackedSurfaceFormat( secondaryLayout.m_eFormat )" in assets_cpp,
        "packed-surface normalization regressed to unconditional RGBA8 expansion")
require("sizeof( T3DVTDispatchPushConstants ) == 16u" in vt_gpu,
        "RenderWorld VT C++ push ABI is not fixed at 16 bytes")
for token in ("m_ResetPush", "m_TierPush[tier]", "m_CompactPush"):
    require(token in vt_gpu and f"compute_list_set_push_constant( nList, {token}" in vt_gpu,
            f"RenderWorld VT dispatch does not publish explicit bounds via {token}")
for shader_name in ("render_world_vt_reset.glsl", "render_world_vt_predict.glsl", "render_world_vt_compact.glsl", "render_world_raster.glsl"):
    require(shader_name in shader_library, f"shader library no longer references audited runtime shader: {shader_name}")

if errors:
    for error in errors:
        print("FAIL:", error)
    sys.exit(1)
print("PASS: RD startup publication capacity and explicit shader resource bounds")
