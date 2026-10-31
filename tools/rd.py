#!/usr/bin/env python3
"""Low-noise Terrain3D RenderingDevice workflow driver for Codex/Windows."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
EXPECTED_BRANCH = "experiment/rendering-device"
REFERENCE_TAG = "rd-alpha-pre-cleanbreak-reference"
WORK_DIR = REPO / "docs" / "rd_work"
LOG_DIR = REPO / ".rd_logs"
TOOLS_DIR = REPO / ".rd_tools"
BATCH_FILE = REPO / "docs" / "rd_codex_batches.tsv"
SOURCE_MAP_FILE = REPO / "docs" / "rd_source_map.tsv"
CONTRACT_FILE = REPO / "docs" / "rendering_device_completion_contract.md"
SYMBOL_BASELINE_FILE = WORK_DIR / "symbol_baseline.tsv"
FINAL_ALLOWLIST_FILE = WORK_DIR / "final_symbol_allowlist.tsv"
DEFAULT_WINDOWS_GODOT_ROOT = Path(r"D:\godot-4.7.2-stable")
DEFAULT_ENGINE_SOURCE = Path(os.environ.get("RD_GODOT_SOURCE", str(TOOLS_DIR / "godot-4.7.2-src")))
FIXTURE_ARCHIVE = REPO / "validation" / "terrain3d_00_02.zip"
FIXTURE_DIR = TOOLS_DIR / "validation_data" / "terrain3d_00_02"
FIXTURE_MANIFEST = REPO / "docs" / "rd_reference" / "terrain3d_00_02_manifest.tsv"

SCAN_ROOTS = [
    REPO / "src",
    REPO / "project" / "addons" / "terrain_3d",
]
TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl",
    ".gd", ".cs", ".json", ".glsl", ".glslinc", ".gdshader", ".gdshaderinc",
    ".tscn", ".tres", ".cfg", ".gdextension", ".md", ".patch", ".diff",
}
SYMBOLS = {
    "RenderingServer": re.compile(r"\bRenderingServer\b"),
    "MultiMesh": re.compile(r"\bMultiMesh\b"),
    "mesh_create": re.compile(r"\bmesh_create\b"),
    "instance_create": re.compile(r"\binstance_create\b"),
    "instance_set_base": re.compile(r"\binstance_set_base\b"),
    "material_set_param": re.compile(r"\bmaterial_set_param\b"),
    "Hybrid": re.compile(r"\bHybrid\b"),
    "TODO": re.compile(r"\bTODO\b"),
    "FIXME": re.compile(r"\bFIXME\b"),
    "placeholder": re.compile(r"\bplaceholder\b", re.IGNORECASE),
}


def capture(command: list[str], *, cwd: Path = REPO) -> str:
    return subprocess.check_output(command, cwd=str(cwd), text=True, stderr=subprocess.STDOUT).strip()


def git_checkout_available() -> bool:
    return (REPO / ".git").exists()


def current_branch() -> str:
    if not git_checkout_available():
        return "source-archive"
    return capture(["git", "rev-parse", "--abbrev-ref", "HEAD"])


def current_commit() -> str:
    if not git_checkout_available():
        return "unavailable"
    return capture(["git", "rev-parse", "HEAD"])


def log_path(name: str) -> Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    clean = re.sub(r"[^A-Za-z0-9_.-]+", "_", name)
    return LOG_DIR / f"{clean}.log"


def run_quiet(
    command: list[str] | str,
    *,
    label: str,
    cwd: Path = REPO,
    env: dict[str, str] | None = None,
    timeout: int | None = None,
    tail_lines: int = 40,
) -> bool:
    path = log_path(label)
    try:
        result = subprocess.run(
            command,
            cwd=str(cwd),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
        output = result.stdout or ""
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or "") if isinstance(exc.stdout, str) else ""
        path.write_text(output, encoding="utf-8", errors="ignore")
        print(f"FAIL {label}: timeout; log={path.relative_to(REPO)}")
        if output:
            print("\n".join(output.splitlines()[-tail_lines:]))
        return False
    except OSError as exc:
        path.write_text(str(exc), encoding="utf-8")
        print(f"FAIL {label}: {exc}; log={path.relative_to(REPO)}")
        return False

    path.write_text(output, encoding="utf-8", errors="ignore")
    if result.returncode == 0:
        print(f"OK {label}")
        return True
    print(f"FAIL {label}: exit={result.returncode}; log={path.relative_to(REPO)}")
    if output:
        print("\n".join(output.splitlines()[-tail_lines:]))
    return False


def branch_check() -> bool:
    if not git_checkout_available():
        print("SKIP branch check: clean source archive has no .git metadata")
        return True
    branch = current_branch()
    if branch != EXPECTED_BRANCH:
        print(f"FAIL branch={branch}; expected={EXPECTED_BRANCH}")
        return False
    print(f"OK branch={branch}")
    return True


def reference_check() -> bool:
    if not git_checkout_available():
        print("SKIP frozen reference tag check: clean source archive has no .git metadata")
        return True
    try:
        ref = capture(["git", "rev-parse", f"{REFERENCE_TAG}^{{}}"] )
    except subprocess.CalledProcessError:
        print(f"FAIL missing reference tag={REFERENCE_TAG}")
        return False
    if not ref.startswith("5d9e8b1c264867e147d5b9d1992acb883f82adb5"):
        print(f"FAIL reference tag moved: {ref}")
        return False
    print("OK frozen reference")
    return True


def diff_check() -> bool:
    if not git_checkout_available():
        print("SKIP git diff --check: clean source archive has no .git metadata")
        return True
    result = subprocess.run(["git", "diff", "--check"], cwd=str(REPO), text=True)
    if result.returncode != 0:
        print("FAIL git diff --check")
        return False
    print("OK git diff --check")
    return True


def build_contract_check() -> bool:
    path = REPO / "project" / "addons" / "terrain_3d" / "terrain.gdextension"
    text = path.read_text(encoding="utf-8", errors="ignore")
    expected = (
        'windows.debug.x86_64 = "res://addons/terrain_3d/bin/libterrain.windows.debug.x86_64.dll"',
        'windows.release.x86_64 = "res://addons/terrain_3d/bin/libterrain.windows.release.x86_64.dll"',
    )
    missing = [entry for entry in expected if entry not in text]
    if missing:
        print("FAIL Windows GDExtension library names do not match SConstruct/MSVC output")
        return False
    print("OK Windows GDExtension build/load names")
    return True


def rd_shader_abi_check() -> bool:
    shader_dir = REPO / "project" / "addons" / "terrain_3d" / "rd"
    shader_names = (
        "terrain_clipmap_near.glsl",
        "terrain_clipmap_middle.glsl",
        "terrain_clipmap_far.glsl",
        "terrain_cull.glsl",
    )
    expected_patch_fields = (
        "origin", "scale", "lod", "tier", "stitch_x", "stitch_z", "flags",
        "bounds_min_y", "bounds_max_y", "bounds_half_extent",
        "previous_origin_x", "previous_origin_z", "previous_scale", "padding_0", "padding_1",
    )
    for name in shader_names:
        path = shader_dir / name
        source = path.read_text(encoding="utf-8", errors="ignore")
        match = re.search(r"struct\s+PatchData\s*\{([^}]*)\}\s*;", source, re.DOTALL)
        fields = tuple(re.findall(r"\b(?:vec2|float|uint)\s+(\w+)\s*;", match.group(1))) if match else ()
        if fields != expected_patch_fields:
            print(f"FAIL RD PatchData ABI mismatch: {path.relative_to(REPO)}")
            return False
        if name.startswith("terrain_clipmap_") and not re.search(
            r"layout\s*\(\s*push_constant\s*,\s*std430\s*\)\s*uniform\s+DrawData", source
        ):
            print(f"FAIL RD direct-draw push constants missing: {path.relative_to(REPO)}")
            return False
    print("OK RD C++/GLSL patch and direct-draw ABI declarations")
    return True


def rd_batch_c_material_check() -> bool:
    checks: list[tuple[str, bool]] = []

    types = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    renderer = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_clipmap_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    coordinator = (REPO / "src" / "rendering" / "rd" / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    shader_library = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_shader_library.cpp").read_text(encoding="utf-8", errors="ignore")
    material_cpp = (REPO / "src" / "rendering" / "terrain_3d_material.cpp").read_text(encoding="utf-8", errors="ignore")
    editor_ui = (REPO / "project" / "addons" / "terrain_3d" / "src" / "ui.gd").read_text(encoding="utf-8", errors="ignore")
    sampler_start = renderer.find("bool Terrain3DRdClipmapRenderer::ensureSamplers")
    sampler_end = renderer.find("bool Terrain3DRdClipmapRenderer::ensureBuffers", sampler_start)
    sampler_body = renderer[sampler_start:sampler_end] if sampler_start >= 0 and sampler_end > sampler_start else ""

    checks += [
        ("material layer ABI size", 'sizeof( t3dRdMaterialLayer_t ) == 80u' in types),
        ("material layer integral texture index", 'uint32_t m_nPhysicalLayer' in types),
        ("material layer shared micro VT allocation", 'uint32_t m_nMicroVTAllocation' in types),
        ("material layer integral flags", 'uint32_t m_nFlags' in types),
        ("tier material ABI size", 'sizeof( t3dRdTierMaterialConfig_t ) == 48u' in types),
        ("material config ABI size", 'sizeof( t3dRdMaterialConfig_t ) == 384u' in types),
        ("material config tier offset", 'offsetof( t3dRdMaterialConfig_t, m_szTier ) == 240u' in types),
        ("snapshot owns fixed material config", 't3dRdMaterialConfig_t m_materialConfig{}' in types),
        ("persistent material config buffer", 'm_hMaterialConfigBuffer' in renderer and 'storage_buffer_create( material_config_bytes )' in renderer),
        ("material config descriptor binding", 't3dRdBufferUniform( 13' in renderer),
        ("derived cache descriptor", 't3dRdTextureUniform( 14' in renderer),
        ("middle composite descriptor", 't3dRdTextureUniform( 15' in renderer),
        ("middle height descriptor", 't3dRdTextureUniform( 16' in renderer),
        ("middle region descriptor", 't3dRdTextureUniform( 17' in renderer),
        ("far height descriptor", 't3dRdTextureUniform( 18' in renderer),
        ("far color descriptor", 't3dRdTextureUniform( 19' in renderer),
        ("far albedo descriptor", 't3dRdTextureUniform( 20' in renderer),
        ("world-noise cache descriptor", 't3dRdTextureUniform( 21' in renderer),
        ("editor overlay ABI size", 'sizeof( t3dRdEditorDecal_t ) == 48u' in types and 'sizeof( t3dRdEditorOverlayConfig_t ) == 192u' in types),
        ("editor overlay ABI offsets", 'offsetof( t3dRdEditorOverlayConfig_t, m_fHeightmapBlackHeight ) == 144u' in types and 'offsetof( t3dRdEditorOverlayConfig_t, m_fContourColorR ) == 160u' in types and 'offsetof( t3dRdEditorOverlayConfig_t, m_fInstancerCellSize ) == 176u' in types),
        ("snapshot owns fixed editor overlay", 't3dRdEditorOverlayConfig_t m_editorOverlayConfig{}' in types),
        ("persistent editor overlay buffer", 'm_hEditorOverlayBuffer' in renderer and 'storage_buffer_create( editor_overlay_bytes )' in renderer),
        ("editor overlay descriptor", 't3dRdBufferUniform( 23' in renderer),
        ("editor brush descriptor", 't3dRdTextureUniform( 24' in renderer),
        ("direct editor brush publication", 'ensureEditorBrushTexture' in coordinator and 'replacement.create( image )' in coordinator and 'm_editorBrushTexture.swap( replacement )' in coordinator and 'm_hEditorBrushTextureRd' in coordinator),
        ("runtime editor state survives native material absence", 'if ( strName.begins_with( "_" ) )' in material_cpp and '&& m_hMaterial.is_valid()' not in material_cpp[material_cpp.find('void Terrain3DMaterial::setShaderParam'):material_cpp.find('Variant Terrain3DMaterial::getShaderParam')]),
        ("runtime texture object retained for RD publication", 'm_runtimeMaterialParams[strName] = rValue;' in material_cpp and 'const Ref<Texture> pTex = value;' in material_cpp and 'setRuntimeMaterialParam( param, pTex.is_valid() ? Variant( pTex ) : Variant() );' in material_cpp and 'setRuntimeMaterialParam( param, pTex->get_rid() )' not in material_cpp),
        ("editor UI publishes through Terrain3DMaterial", '_set_editor_material_param("_editor_brush_texture", editor_brush_texture)' in editor_ui and 'plugin.terrain.material.set_shader_param(p_name, p_value)' in editor_ui),
        ("editor UI avoids direct native editor-param writes", re.search(r'RenderingServer\.material_set_param\(mat_rid,\s*"_(?:editor|region_map)', editor_ui) is None),
        ("tier-specific material samplers", 'm_hszMaterialSamplers[4]' in (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_clipmap_renderer.h").read_text(encoding="utf-8", errors="ignore") and 'm_nszBoundTierFiltering' in renderer),
        ("material and map sampler retries are independent", 'if ( !m_hszMaterialSamplers[i].is_valid() )' in sampler_body and 'if ( !m_hszMapSamplers[i].is_valid() )' in sampler_body and 'continue;' not in sampler_body),
        ("anisotropic material sampler modes", 'set_use_anisotropy' in renderer and 'set_anisotropy_max' in renderer),
        ("packed logical+physical region publication", '0x80000000u' in coordinator and 'getLogicalRegionMap()' in coordinator),
        ("direct world-noise cache publication", 'm_worldNoiseCache.create' in coordinator and 'get_seamless_image( 1024, 1024' in coordinator),
        ("direct derived publication", 'getDerivedMapsRid()' in coordinator),
        ("direct middle publication", 'getMiddleCompositeMapsRid()' in coordinator and 'getMiddleHeightMapsRid()' in coordinator),
        ("direct far publication", 'getFarHeightMapRid()' in coordinator and 'getFarColorMapRid()' in coordinator and 'getFarAlbedoArrayRid()' in coordinator),
        ("shader loader rejects compile errors", 'get_stage_compile_error' in shader_library and 'RD shader compile failed' in shader_library),
    ]

    shader_dir = REPO / "project" / "addons" / "terrain_3d" / "rd"
    expected_material_config_fields = (
        "feature_flags", "debug_flags", "output_flags", "background_mode", "projection_mode",
        "tessellation_material_mode", "world_noise_mode", "reserved_0", "displacement_sharpness",
        "blend_sharpness", "distant_normal_scale", "ground_level", "region_blend", "bias_distance",
        "mipmap_bias", "depth_blur", "auto_slope", "auto_height_reduction", "auto_base_texture",
        "auto_overlay_texture", "dual_scale_texture", "reserved_1", "dual_scale_reduction",
        "tri_scale_reduction", "dual_scale_far", "dual_scale_near", "noise1_scale", "noise2_scale",
        "noise1_rotation_x", "noise1_rotation_y", "macro_variation1_r", "macro_variation1_g",
        "macro_variation1_b", "macro_variation_slope", "macro_variation2_r", "macro_variation2_g",
        "macro_variation2_b", "macro_variation_padding", "noise1_offset_x", "noise1_offset_y",
        "world_noise_cache_period", "world_noise_lod_distance", "world_noise_scale", "world_noise_height",
        "world_noise_max_octaves", "world_noise_min_octaves", "world_noise_cache_enabled",
        "world_noise_fragment_normals", "world_noise_offset_x", "world_noise_offset_y", "world_noise_offset_z",
        "world_noise_padding", "silhouette_r", "silhouette_g", "silhouette_b", "silhouette_a",
        "contour_step", "editor_overlay_opacity", "reserved_2", "reserved_3", "tier",
    )
    expected_tier_fields = (
        "sampling_mode", "normal_mode", "color_mode", "surface_response", "texture_surface_mode",
        "lighting_model", "detiling_mode", "filtering", "authored_color_contribution",
        "wetness_contribution", "material_lod_bias", "quality_lod_offset",
    )
    expected_layer_fields = ("color", "pbr0", "pbr1", "displacement", "physical_layer", "flags")

    def struct_fields(source: str, struct_name: str) -> list[tuple[str, ...]]:
        bodies = re.findall(rf"struct\s+{struct_name}\s*\{{(.*?)\}};", source, flags=re.S)
        result: list[tuple[str, ...]] = []
        for body in bodies:
            names: list[str] = []
            for declaration in body.split(";"):
                declaration = declaration.strip()
                if not declaration:
                    continue
                token = declaration.split()[-1]
                names.append(token.split("[")[0])
            result.append(tuple(names))
        return result

    required_shader_tokens = (
        "struct MaterialLayer",
        "uint physical_layer;",
        "uint flags;",
        "struct MaterialConfig",
        "MaterialConfigBuffer",
        "binding = 13",
        "binding = 14",
        "binding = 15",
        "binding = 16",
        "binding = 17",
        "binding = 18",
        "binding = 19",
        "binding = 20",
        "binding = 21",
        "binding = 23",
        "binding = 24",
        "struct EditorOverlayConfig",
        "editor_brush_texture",
        "T3D_FEATURE_WORLD_NOISE_CACHE",
        "cache_mode",
        "logical_region_present(",
        "read_control(",
        "decode_control(",
        "sample_detailed_material(",
        "sample_middle_composite(",
        "sample_far_surface(",
        "auto_control(",
        "project_material_uv(",
        "apply_detiling(",
        "T3D_FEATURE_DUAL_SCALING",
        "apply_macro_variation(",
        "apply_fragment_world_noise_normal(",
        "world_noise_uses_fragment_normals(",
        "light_material(",
        "apply_fog(",
        "fractal_contour_lines(",
        "apply_editor_decals(",
        "debug_visualization(",
        "editor_overlay.heightmap_contour",
        "editor_overlay.contour_color",
        "editor_overlay.instancer_grid",
        "discard;",
    )
    for tier, name in enumerate(("terrain_clipmap_near.glsl", "terrain_clipmap_middle.glsl", "terrain_clipmap_far.glsl")):
        source = (shader_dir / name).read_text(encoding="utf-8", errors="ignore")
        checks.append((f"{name} tier identity", source.count(f"#define T3D_TIER {tier}") == 2))
        checks.append((f"{name} vertex+fragment stages", source.count("#[vertex]") == 1 and source.count("#[fragment]") == 1))
        checks.append((f"{name} balanced braces", source.count("{") == source.count("}")))
        config_layouts = struct_fields(source, "MaterialConfig")
        tier_layouts = struct_fields(source, "TierMaterialConfig")
        layer_layouts = struct_fields(source, "MaterialLayer")
        checks.append((f"{name} material config GLSL ABI", len(config_layouts) == 2 and all(fields == expected_material_config_fields for fields in config_layouts)))
        checks.append((f"{name} tier config GLSL ABI", len(tier_layouts) == 2 and all(fields == expected_tier_fields for fields in tier_layouts)))
        checks.append((f"{name} layer GLSL ABI", len(layer_layouts) == 1 and layer_layouts[0] == expected_layer_fields))
        checks.append((f"{name} material parity surface", all(token in source for token in required_shader_tokens)))
        checks.append((f"{name} world-noise cache declared in both stages", source.count("binding = 21") == 2))
        checks.append((f"{name} editor overlay fragment bindings", source.count("binding = 23") == 1 and source.count("binding = 24") == 1))
        checks.append((f"{name} editor and debug parity", "apply_editor_decals(lit)" in source and "((control >> 1u) & 1u) != 0u" in source and "white_height < black_height" in source))
        checks.append((f"{name} no opaque sampler assignment", re.search(r"\bsampler\w*\s+\w+\s*=", source) is None))

    failed = [label for label, passed in checks if not passed]
    if failed:
        for label in failed:
            print(f"FAIL Batch C material contract: {label}")
        print(f"Batch C material contract: {len(checks) - len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch C material contract: {len(checks)}/{len(checks)} passed")
    return True



def rd_batch_d_instance_check() -> bool:
    checks: list[tuple[str, bool]] = []
    types = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    mesh_h = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_mesh_cache.h").read_text(encoding="utf-8", errors="ignore")
    mesh_cpp = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_mesh_cache.cpp").read_text(encoding="utf-8", errors="ignore")
    instance_h = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_instance_renderer.h").read_text(encoding="utf-8", errors="ignore")
    instance_cpp = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    instancer_h = (REPO / "src" / "instances" / "terrain_3d_instancer.h").read_text(encoding="utf-8", errors="ignore")
    instancer_cpp = (REPO / "src" / "instances" / "terrain_3d_instancer.cpp").read_text(encoding="utf-8", errors="ignore")
    coordinator_h = (REPO / "src" / "rendering" / "rd" / "terrain_3d_render_coordinator.h").read_text(encoding="utf-8", errors="ignore")
    coordinator_cpp = (REPO / "src" / "rendering" / "rd" / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    shader_library = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_shader_library.cpp").read_text(encoding="utf-8", errors="ignore")
    shader = (REPO / "project" / "addons" / "terrain_3d" / "rd" / "terrain_instances.glsl").read_text(encoding="utf-8", errors="ignore")
    vcxproj = (REPO / "Terrain3D.vcxproj").read_text(encoding="utf-8", errors="ignore")
    filters = (REPO / "Terrain3D.vcxproj.filters").read_text(encoding="utf-8", errors="ignore")

    render_cell_match = re.search(r"typedef struct t3dRenderCell_s\s*\{(.*?)\}\s*t3dRenderCell_t;", instancer_h, re.S)
    render_cell_body = render_cell_match.group(1) if render_cell_match else ""
    runtime_multimesh_calls = (
        "multimesh_create(", "multimesh_allocate_data(", "multimesh_set_buffer(",
        "instance_create(", "instance_set_base(", "instance_set_scenario(",
    )

    checks += [
        ("generation-safe mesh handle ABI", "sizeof( t3dRdMeshHandle_t ) == 8u" in types and "m_nGeneration" in types),
        ("generation-safe surface handle ABI", "sizeof( t3dRdSurfaceHandle_t ) == 8u" in types),
        ("48-byte static mesh vertex ABI", "sizeof( t3dRdMeshVertex_t ) == 48u" in types),
        ("32-byte packed instance ABI", "sizeof( t3dRdInstanceRecord_t ) == 32u" in types),
        ("Godot RGBA32 is repacked to GPU byte order", "t3dRdPackGodotRgba32ForGpu" in types and "t3dRdPackGodotRgba32ForGpu" in instancer_cpp and "t3dRdPackGodotRgba32ForGpu" in mesh_cpp),
        ("80-byte extracted material ABI", "sizeof( t3dRdInstanceMaterial_t ) == 80u" in types),
        ("mesh source publication", "t3dRdMeshSourceSnapshot_t" in mesh_h and "buildSourceSnapshot" in mesh_h),
        ("large shared static mesh arenas", "s_nMinVertexArenaBytes = 4u * 1024u * 1024u" in mesh_cpp and "s_nMinIndexArenaBytes = 2u * 1024u * 1024u" in mesh_cpp),
        ("explicit arena growth", "nextArenaCapacity" in mesh_h and "while ( capacity < nRequiredBytes ) capacity <<= 1u" in mesh_cpp),
        ("shared vertex arena", "vertex_buffer_create" in mesh_cpp and "T3D.Instance.MeshVertexArena" in mesh_cpp),
        ("shared U32 index arena", mesh_cpp.count("index_buffer_create") >= 1 and "T3D.Instance.MeshIndexArena32" in mesh_cpp and "MeshIndexArena16" not in mesh_cpp),
        ("universal U32 production indices", "INDEX_BUFFER_FORMAT_UINT32" in mesh_cpp and "INDEX_BUFFER_FORMAT_UINT16" not in mesh_cpp and "m_index16Bytes" not in mesh_h and "m_nIndexFormat" not in mesh_h),
        ("generation checked mesh resolution", "rHandle.m_nGeneration == m_nGeneration" in mesh_cpp),
        ("material subset extraction", "Object::cast_to<BaseMaterial3D>" in mesh_cpp and "TEXTURE_ALBEDO" in mesh_cpp and "TEXTURE_NORMAL" in mesh_cpp and "TEXTURE_ORM" in mesh_cpp),
        ("separate roughness texture/channel parity", "TEXTURE_ROUGHNESS" in mesh_cpp and "get_roughness_texture_channel" in mesh_cpp and "surface_layout" in mesh_cpp and "T3D_VT_MATERIAL_SURFACE_LAYOUT_MASK" in mesh_cpp and "surface_layout" in shader and "material_texture_channel" in shader),
        ("material table deduplicates packed state", "std::memcmp( &rMaterials[i], &packed, sizeof( packed ) ) == 0" in mesh_cpp),
        ("material texture identity participates in source fingerprint", "t3dRdHashTexture" in mesh_cpp and "texture_rid.get_id()" in mesh_cpp),
        ("direct RD imported material textures", "Terrain3DRdTextureManager::buildLayout" in mesh_cpp and "pManager->createTexture" in mesh_cpp and "getTextureRid" in mesh_h),
        ("BaseMaterial vertex color and normal scale extracted", "FLAG_ALBEDO_FROM_VERTEX_COLOR" in mesh_cpp and "FLAG_SRGB_VERTEX_COLOR" in mesh_cpp and "get_normal_scale" in mesh_cpp),
        ("BaseMaterial cull/shading/AO state extracted", "CULL_FRONT" in mesh_cpp and "SHADING_MODE_UNSHADED" in mesh_cpp and "FEATURE_AMBIENT_OCCLUSION" in mesh_cpp),
        ("material edits participate in source fingerprint", "t3dRdHashMaterial" in mesh_cpp and "get_metallic" in mesh_cpp and "get_roughness" in mesh_cpp),
        ("mesh shape metadata participates in source fingerprint", "_surface_get_array_len" in mesh_cpp and "_surface_get_array_index_len" in mesh_cpp and "mesh->get_aabb()" in mesh_cpp),
        ("logical instancer publishes immutable RD scene", "buildRdInstanceSnapshot" in instancer_cpp and "std::shared_ptr<const t3dRdInstanceSceneSnapshot_t>" in instancer_h),
        ("logical cell instance ranges are deduplicated", "std::unordered_map<t3dCellUpdateKey_t, t3dRdCellRange_t" in instancer_cpp and "m_nFirstInstance" in instancer_cpp),
        ("instance publication generation invalidation", "markRdPublicationDirty" in instancer_cpp and "m_nRdPublicationGeneration" in instancer_h),
        ("runtime render cells own no GPU RIDs", bool(render_cell_body) and "RID" not in render_cell_body and "MultiMesh" not in render_cell_body),
        ("runtime instancer has no MultiMesh/RS creation calls", all(token not in instancer_cpp for token in runtime_multimesh_calls)),
        ("MultiMesh retained only as authoring input", "void Terrain3DInstancer::addMultimesh" in instancer_cpp and "The runtime renderer does not consume this representation" in instancer_h),
        ("persistent instance buffers", all(token in instance_h for token in ("m_hInstanceBuffer", "m_hVisibleIndexBuffer", "m_hMaterialBuffer", "m_hIndirectBuffer"))),
        ("publication-time visibility/indirect capacity", "packet_capacity" in instance_cpp and "visible_index_capacity" in instance_cpp and "visible_index_capacity +=" in instance_cpp and "cell.m_nInstanceCount" in instance_cpp and "indirect_bytes" in instance_cpp),
        ("dirty instance span detection", "std::memcmp( previous + offset" in instance_cpp and "first_changed" in instance_cpp and "last_changed" in instance_cpp),
        ("dirty instance subrange upload", "buffer_update( m_hInstanceBuffer, first_byte, uploaded_bytes" in instance_cpp and "instance_dirty_upload_ranges" in instance_cpp),
        ("coarse AABB culling", "isCellVisible" in instance_cpp and "m_bounds.get_center()" in instance_cpp and "corners[8]" in instance_cpp),
        ("visibility-layer culling", "cell.m_nVisibilityLayers & nVisibilityMask" in instance_cpp),
        ("distance/LOD culling", "m_fLodBegin" in instance_cpp and "m_fLodEnd" in instance_cpp),
        ("visibility-range fade-self parity", "m_fFadeMargin" in instance_cpp and "T3D_RD_INSTANCE_FADE_ACTIVE" in instance_cpp and "fade_unorm" in instance_cpp and "INSTANCE_FADE_ACTIVE" in shader and "gl_FragCoord" in shader),
        ("mesh/material grouping", "m_nSurfaceSlot" in instance_cpp and "m_nMaterial" in instance_cpp and "m_nOverlayMaterial" in instance_cpp),
        ("visible indices flattened after grouping", "m_visibleIndices.insert" in instance_cpp and "group.m_indices" in instance_cpp),
        ("indexed indirect submission", "draw_list_draw_indirect" in instance_cpp and "t3dRdIndexedIndirectCommand_t" in instance_h),
        ("debug direct-draw path", "TERRAIN3D_RD_INSTANCE_DIRECT_DRAW" in coordinator_cpp and "draw_list_draw( list, true" in instance_cpp),
        ("depth packets published", "m_depthPackets" in instance_h and "m_depthPackets.push_back" in instance_cpp),
        ("shadow packets published", "m_shadowPackets" in instance_h and "m_shadowPackets.push_back" in instance_cpp and "T3D_RD_INSTANCE_CAST_SHADOWS" in instance_cpp),
        ("instance shader loaded by RD shader library", "terrain_instances.glsl" in shader_library),
        ("instance shader has vertex+fragment stages", shader.count("#[vertex]") == 1 and shader.count("#[fragment]") == 1),
        ("instance shader descriptor ABI", all(token in shader for token in ("binding = 0", "binding = 1", "binding = 2", "binding = 3", "InstanceRecord", "InstanceMaterial"))),
        ("instance shader consumes packed transforms/colors", "unpackSnorm2x16" in shader and "unpackHalf2x16" in shader and "unpackUnorm4x8" in shader),
        ("instance material descriptor set is invariant VT/global state", all(token in instance_cpp for token in ("ensureMaterialVtUniformSet", "buildMaterialVtUniformSet", "T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE", "T3D_RW_GPU_BUFFER_VT_PAGE_TABLE")) and "m_materialBindings" not in instance_cpp),
        ("instance shader samples albedo normal and surface through VT banks", all(token in shader for token in ("vt_albedo_bank0", "vt_normal_bank0", "vt_surface_bank0", "vt_allocations", "vt_page_table", "material.vtAllocation", "overlay.vtAllocation"))),
        ("instance material filtering/repeat sampler policy", "m_nSamplerClass" in mesh_cpp and "T3D_VT_MATERIAL_FILTER_MASK" in mesh_cpp and "t3d_inst_effective_sampler_class" in shader and "t3d_inst_sampler_repeats" in shader),
        ("material alpha-scissor and vertex-color parity", "MATERIAL_ALPHA_SCISSOR" in shader and "MATERIAL_VERTEX_COLOR" in shader and "discard" in shader and "srgb_to_linear" in shader),
        ("alpha blend and depth-prepass material extraction", "TRANSPARENCY_ALPHA" in mesh_cpp and "TRANSPARENCY_ALPHA_DEPTH_PRE_PASS" in mesh_cpp and "T3D_RD_INSTANCE_ALPHA_BLEND" in mesh_cpp and "T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS" in mesh_cpp),
        ("transparent cells stay independently sortable", "!alpha_blend && !range_fade" in instance_cpp and "m_fSortDistanceSq" in instance_h and "std::stable_sort" in instance_cpp),
        ("alpha depth prepass is two-pass with color writes disabled", "m_szPipeline[9]" in instance_h and "opacity_mode == 2u" in instance_cpp and "set_write_r( false )" in instance_cpp and "T3D_RD_INSTANCE_DEPTH_PREPASS_PASS" in instance_cpp and "INSTANCE_DEPTH_PREPASS_PASS" in shader),
        ("alpha color pass uses persistent blend variants", "BLEND_FACTOR_SRC_ALPHA" in instance_cpp and "BLEND_FACTOR_ONE_MINUS_SRC_ALPHA" in instance_cpp and "pipeline_index += 3u" in instance_cpp),
        ("material cull modes use persistent pipeline variants", "m_szPipeline[9]" in instance_h and all(token in instance_cpp for token in ("POLYGON_CULL_BACK", "POLYGON_CULL_FRONT", "POLYGON_CULL_DISABLED", "T3D_RD_INSTANCE_CULL_FRONT"))),
        ("shadow double-sided state is separate from color culling", "T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED" in types and "T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED" in instancer_cpp and "T3D_RD_INSTANCE_DOUBLE_SIDED" in mesh_cpp),
        ("immutable mesh/scene snapshot publication", "std::shared_ptr<const t3dRdMeshSourceSnapshot_t>" in coordinator_h and "std::shared_ptr<const t3dRdInstanceSceneSnapshot_t>" in coordinator_h),
        ("instances render independently of terrain patch presence", "const bool bHasTerrain" in coordinator_cpp and "const bool instances_ok = m_instanceRenderer.render" in coordinator_cpp and "!bHasTerrain ||" in coordinator_cpp),
        ("instance diagnostics surfaced", "instance_depth_packets" in coordinator_cpp and "instance_shadow_packets" in coordinator_cpp and "visible_instances" in coordinator_cpp),
        ("Visual Studio project includes Batch D sources", "terrain_3d_rd_instance_renderer.cpp" in vcxproj and "terrain_3d_rd_mesh_cache.cpp" in vcxproj and "terrain_3d_rd_instance_renderer.h" in vcxproj and "terrain_3d_rd_mesh_cache.h" in vcxproj),
        ("Visual Studio filters include Batch D sources", "terrain_3d_rd_instance_renderer.cpp" in filters and "terrain_3d_rd_mesh_cache.cpp" in filters),
    ]

    failed = [label for label, passed in checks if not passed]
    if failed:
        for label in failed:
            print(f"FAIL Batch D instance contract: {label}")
        print(f"Batch D instance contract: {len(checks) - len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch D instance contract: {len(checks)}/{len(checks)} passed")
    return True


def rd_batch_e_hlod_check() -> bool:
    checks: list[tuple[str, bool]] = []
    types = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    mesh_h = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_mesh_cache.h").read_text(encoding="utf-8", errors="ignore")
    mesh_cpp = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_mesh_cache.cpp").read_text(encoding="utf-8", errors="ignore")
    instance_h = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_instance_renderer.h").read_text(encoding="utf-8", errors="ignore")
    instance_cpp = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    instancer_cpp = (REPO / "src" / "instances" / "terrain_3d_instancer.cpp").read_text(encoding="utf-8", errors="ignore")
    streamer_h = (REPO / "src" / "hlod" / "terrain_3d_hlod_streamer.h").read_text(encoding="utf-8", errors="ignore")
    streamer_cpp = (REPO / "src" / "hlod" / "terrain_3d_hlod_streamer.cpp").read_text(encoding="utf-8", errors="ignore")
    coordinator_h = (REPO / "src" / "rendering" / "rd" / "terrain_3d_render_coordinator.h").read_text(encoding="utf-8", errors="ignore")
    coordinator_cpp = (REPO / "src" / "rendering" / "rd" / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    shader_library = (REPO / "src" / "rendering" / "rd" / "terrain_3d_rd_shader_library.cpp").read_text(encoding="utf-8", errors="ignore")
    shader = (REPO / "project" / "addons" / "terrain_3d" / "rd" / "terrain_hlod_cull.glsl").read_text(encoding="utf-8", errors="ignore")

    hlod_native_calls = (
        "instance_create(", "instance_set_base(", "instance_set_scenario(",
        "instance_set_transform(", "instance_set_cast_shadows_setting(", "free_rid(",
    )
    checks += [
        ("HLOD entry owns no native render RID", "RID m_hInstance" not in streamer_h and "m_hInstance" not in streamer_cpp),
        ("HLOD streamer performs no native instance allocation/submission", all(token not in streamer_cpp for token in hlod_native_calls) and "RenderingServer" not in streamer_cpp),
        ("HLOD publication has deterministic generation", "m_nRdPublicationGeneration" in streamer_h and "markRdPublicationDirty" in streamer_cpp),
        ("HLOD mesh source keys use a disjoint stable namespace", "t3dRdHlodMeshSourceKey" in types and "0x2000000000000000ull" in types and "t3dRdHlodMeshSourceKey" in streamer_cpp),
        ("regular mesh source keys remain disjoint", "0x1000000000000000ull" in types and "t3dRdRegularMeshSourceKey" in instancer_cpp),
        ("active HLOD packages publish shared mesh sources", "fillRdMeshSources" in streamer_cpp and "t3dRdExternalMeshSource_t" in streamer_cpp and "entry.m_ackage->getMesh()" in streamer_cpp),
        ("active HLOD packages publish immutable instance snapshot", "buildRdInstanceSnapshot" in streamer_cpp and "T3D_RD_INSTANCE_HLOD" in streamer_cpp and "m_nMeshSourceKey = rdMeshSourceKey" in streamer_cpp),
        ("HLOD publication uses package-local bounds", "getLocalBounds()" in streamer_cpp and "t3dTransformHlodBounds" in streamer_cpp),
        ("retiring HLOD remains published until deterministic deactivation", "if ( !entry.m_bActive || entry.m_ackage.is_null()" in streamer_cpp and "m_bRetiring" not in streamer_cpp[streamer_cpp.find("void Terrain3DHLODStreamer::fillRdMeshSources"):streamer_cpp.find("std::shared_ptr<const t3dRdInstanceSceneSnapshot_t>")]),
        ("CPU HLOD projected-error selection is preserved", "packageAcceptable" in streamer_cpp and "m_fTargetErrorPixels" in streamer_cpp and "m_fHysteresis" in streamer_cpp),
        ("CPU HLOD residency budgets are preserved", all(token in streamer_cpp for token in ("m_nLoadBudget", "m_nUnloadBudget", "m_nMemoryBudgetMb", "m_nTransitionOvercommitBytesLastTick"))),
        ("no-hole parent promotion/retirement is preserved", "hasActiveDescendant" in streamer_cpp and "replacementReady" in streamer_cpp and "m_nRetireReadyFrames >= 2" in streamer_cpp),
        ("shared mesh cache accepts external HLOD sources", "t3dRdExternalMeshSource_t" in mesh_h and "rExternalMeshes" in mesh_cpp),
        ("HLOD-only mesh publication does not require regular instance assets", "rAssets.is_null() && rExternalMeshes.empty()" in mesh_cpp and "const int mesh_count = rAssets.is_valid() ? rAssets->getMeshCount() : 0" in mesh_cpp),
        ("HLOD residency churn updates only changed shared-arena spans", "m_lastUploadedSnapshot" in mesh_h and "upload_changed_span" in mesh_cpp and "std::memcmp( current + first, previous + first, 32u )" in mesh_cpp),
        ("external HLOD source keys participate in fingerprint", "hash = t3dRdHashU64( hash, source.m_nSourceKey )" in mesh_cpp),
        ("duplicate external source keys are rejected", "Duplicate HLOD mesh source key in RD publication" in mesh_cpp),
        ("external source keys cannot alias regular/shared mesh sources", "HLOD mesh source key collides with an existing RD mesh source" in mesh_cpp),
        ("source-key lookup returns current generation handle", "getMeshHandleBySourceKey" in mesh_cpp and "return t3dRdMeshHandle_t{ i, m_nGeneration }" in mesh_cpp),
        ("mesh resolution retains generation validation", "rHandle.m_nGeneration == m_nGeneration" in mesh_cpp),
        ("coordinator caches HLOD publication independently", "m_cachedHlodInstanceScene" in coordinator_h and "m_cachedHlodMeshSources" in coordinator_h and "hlod_changed" in coordinator_cpp),
        ("coordinator merges HLOD into shared mesh cache", "computeSourceFingerprint(\n\t\t\tinstance_assets, m_cachedHlodMeshSources )" in coordinator_cpp and "m_cachedHlodMeshSources," in coordinator_cpp),
        ("coordinator merges regular and HLOD instance tables with offset", "instance_offset" in coordinator_cpp and "cell.m_nFirstInstance += instance_offset" in coordinator_cpp),
        ("HLOD uses shared regular-instance renderer", "m_instanceRenderer.initialize( m_shaderLibrary.getInstances()," in coordinator_cpp and "m_shaderLibrary.getHlodCull()" in coordinator_cpp and "Terrain3DRdHlodRenderer" not in coordinator_cpp),
        ("HLOD instance flag is explicit and outside fade mask", "T3D_RD_INSTANCE_HLOD = 1u << 25u" in types and "T3D_RD_INSTANCE_FADE_MASK" in types),
        ("HLOD cull descriptor ABI is fixed", "sizeof( t3dRdHlodCullDescriptor_t ) == 48u" in types and "offsetof( t3dRdHlodCullDescriptor_t, m_nVisibilityLayers ) == 32u" in types),
        ("persistent HLOD descriptor resources exist", "m_hHlodCullDescriptorBuffer" in instance_h and "m_hHlodCullUniformSet" in instance_h and "m_nHlodDescriptorCapacityBytes" in instance_h),
        ("HLOD compute shader is loaded and required", "terrain_hlod_cull.glsl" in shader_library and "m_hlodCull" in shader_library),
        ("HLOD compute shader has frame/descriptor/indirect bindings", all(token in shader for token in ("binding = 0", "binding = 1", "binding = 2", "HlodCullDescriptor", "IndexedIndirectCommand"))),
        ("HLOD compute shader has fixed push ABI", "layout(push_constant, std430)" in shader and all(token in shader for token in ("descriptor_count", "visibility_mask", "command_count"))),
        ("HLOD GPU culling performs 8-corner clip test", "vec4 corners[8]" in shader and shader.count("corners[") >= 9 and "bounds_visible" in shader),
        ("HLOD GPU visibility uses layer mask", "descriptor.visibility_layers & cull_data.visibility_mask" in shader),
        ("HLOD GPU visibility writes indirect instance count", "commands[descriptor.command_index].instance_count" in shader),
        ("regular cells remain CPU culled while HLOD is GPU culled", "( bShadowPass || !hlod )" in instance_cpp and "( !bShadowPass && hlod )" in instance_cpp),
        ("HLOD packets remain independently addressable for GPU cull", "!hlod && !alpha_blend" in instance_cpp and "m_hlodDescriptors.reserve" in instance_cpp),
        ("multi-surface HLOD visibility reserves one visible span per surface", "visible_index_capacity += ( uint64_t )( cell.m_nInstanceCount ) * ( uint64_t )( lod->m_nSurfaceCount )" in instance_cpp),
        ("HLOD descriptor references post-sort indirect command index", "descriptor.m_nCommandIndex = command_index" in instance_cpp and "std::stable_sort( m_drawPackets.begin()" in instance_cpp),
        ("HLOD cull dispatch updates persistent indirect buffer", "m_hIndirectBuffer" in instance_cpp and "dispatchHlodCull" in instance_cpp and "compute_list_dispatch" in instance_cpp),
        ("HLOD cull bindings are invalidated when indirect storage grows", "if ( replace_indirect && m_hHlodCullUniformSet.is_valid() )" in instance_cpp and "m_context.freeResource( m_hHlodCullUniformSet )" in instance_cpp),
        ("HLOD cull inserts compute-to-draw barrier", "compute_list_add_barrier" in instance_cpp and "dispatchHlodCull( nVisibilityMask )" in instance_cpp),
        ("HLOD cull has no CPU readback or sync", "buffer_get_data" not in instance_cpp[instance_cpp.find("bool Terrain3DRdInstanceRenderer::dispatchHlodCull"):instance_cpp.find("bool Terrain3DRdInstanceRenderer::buildGpuPacketTopology")] and ".sync(" not in instance_cpp[instance_cpp.find("bool Terrain3DRdInstanceRenderer::dispatchHlodCull"):instance_cpp.find("bool Terrain3DRdInstanceRenderer::buildGpuPacketTopology")]),
        ("HLOD shadow policy is published", "entry.m_nLevel <= m_nMaxShadowLevel" in streamer_cpp and "T3D_RD_INSTANCE_CAST_SHADOWS" in streamer_cpp),
        ("HLOD participates in shared shadow packets", "m_shadowPackets.push_back" in instance_cpp and "bShadowPass" in instance_cpp and "created.m_packet.m_cullBounds = cell.m_bounds" in instance_cpp and "created.m_packet.m_nVisibilityLayers = cell.m_nVisibilityLayers" in instance_cpp),
        ("HLOD imported materials use shared mesh material/VT path", "t3dRdAppendMeshSource" in mesh_cpp and "t3dRdAddMaterial" in mesh_cpp and "getVtMaterials" in instance_cpp and "ensureMaterialVtUniformSet" in instance_cpp),
        ("HLOD deactivation releases package and invalidates publication", "entry.m_ackage.unref()" in streamer_cpp and "if ( wasPublished ) markRdPublicationDirty()" in streamer_cpp),
        ("HLOD renderer exposes cull diagnostics", all(token in instance_cpp for token in ("hlod_gpu_cull_descriptors", "hlod_gpu_cull_dispatches", "hlod_descriptor_capacity_bytes"))),
    ]

    failed = [label for label, passed in checks if not passed]
    if failed:
        for label in failed:
            print(f"FAIL Batch E HLOD contract: {label}")
        print(f"Batch E HLOD contract: {len(checks) - len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch E HLOD contract: {len(checks)}/{len(checks)} passed")
    return True

def rd_batch_f1_forward_bridge_check() -> bool:
    checks: list[tuple[str, bool]] = []
    rd_dir = REPO / "src" / "rendering" / "rd"
    patch_path = REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch"
    readme_path = REPO / "engine_patch" / "README.md"
    doc_path = REPO / "docs" / "rd_batch_forward_plus_bridge.md"
    required = [
        rd_dir / "terrain_3d_rd_forward_plus_bridge.h",
        rd_dir / "terrain_3d_rd_forward_plus_bridge.cpp",
        patch_path,
        readme_path,
        doc_path,
    ]
    checks.append(("required F1 files exist", all(path.is_file() for path in required)))
    if not all(path.is_file() for path in required):
        failed = [label for label, passed in checks if not passed]
        for label in failed:
            print(f"FAIL Batch F1 Forward+ bridge contract: {label}")
        return False

    bridge_h = required[0].read_text(encoding="utf-8", errors="ignore")
    bridge_cpp = required[1].read_text(encoding="utf-8", errors="ignore")
    patch = patch_path.read_text(encoding="utf-8", errors="ignore")
    readme = readme_path.read_text(encoding="utf-8", errors="ignore")
    doc = doc_path.read_text(encoding="utf-8", errors="ignore")
    types = (rd_dir / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    coordinator_h = (rd_dir / "terrain_3d_render_coordinator.h").read_text(encoding="utf-8", errors="ignore")
    coordinator_cpp = (rd_dir / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    compositor_cpp = (rd_dir / "terrain_3d_rd_compositor_effect.cpp").read_text(encoding="utf-8", errors="ignore")
    register_cpp = (REPO / "src" / "register_types.cpp").read_text(encoding="utf-8", errors="ignore")
    vcxproj = (REPO / "Terrain3D.vcxproj").read_text(encoding="utf-8", errors="ignore")
    vcxproj_filters = (REPO / "Terrain3D.vcxproj.filters").read_text(encoding="utf-8", errors="ignore")

    exact_revision = "a13da4feb8d8aefc283c3763d33a2f170a18d541"
    pass_tokens = (
        "T3D_RD_FORWARD_PASS_DEPTH",
        "T3D_RD_FORWARD_PASS_OPAQUE",
        "T3D_RD_FORWARD_PASS_SHADOW",
        "T3D_RD_FORWARD_PASS_MOTION",
        "T3D_RD_FORWARD_PASS_MATERIAL",
        "T3D_RD_FORWARD_PASS_SDF",
        "T3D_RD_FORWARD_PASS_GI",
        "T3D_RD_FORWARD_PASS_REFLECTION",
    )
    packet_fields = (
        "m_nBridgeAbiVersion", "m_phase", "m_passKind", "m_nativePassMode", "m_nDrawList", "m_nFramebufferFormat",
        "m_nViewCount", "m_nScenePass", "m_renderRegion", "m_renderSize",
        "m_nColorPassFlags", "m_bDepthPrepassActive", "m_bReverseCull", "m_bReflectionProbeRender",
        "m_cameraTransform", "m_previousCameraTransform", "m_szViewProjection",
        "m_szPreviousViewProjection", "m_szEyeOffset", "m_nCameraVisibleLayers",
        "m_hDirectionalLightBuffer", "m_hOmniLightBuffer", "m_hSpotLightBuffer",
        "m_hAreaLightBuffer", "m_hClusterBuffer", "m_nClusterSize", "m_nClusterMaxElements",
        "m_hShadowAtlasTexture", "m_hDirectionalShadowAtlasTexture", "m_hNativeShadowSampler",
        "m_hLightmapBuffer", "m_hLightmapCaptureBuffer", "m_hReflectionProbeBuffer",
        "m_hReflectionAtlasTexture", "m_hRadianceTexture", "m_hDecalBuffer",
        "m_hDecalAtlasTexture", "m_hDecalAtlasSrgbTexture", "m_hAreaLightAtlasTexture",
        "m_hBaseUniformSet", "m_hRenderPassUniformSet",
    )
    engine_methods = (
        "get_abi_version", "register_callback", "unregister_callback", "has_callback",
        "collect_retired_callbacks",
    )

    checks += [
        ("bridge ABI is synchronized on extension side", "s_nBridgeAbiVersion = s_nT3dForwardBridgeAbi" in bridge_h and "s_nRdForwardBridgeAbiVersion = 10u" in types),
        ("engine singleton name fixed", 's_pszEngineSingletonName = "Terrain3DForwardPlusBridge"' in bridge_h and "class Terrain3DForwardPlusBridge" in patch),
        ("engine ABI exposes required lifecycle API", all(token in patch for token in engine_methods) and "ABI_VERSION = 10" in patch),
        ("engine callback slot is atomic", "std::atomic<CallbackSlot *> callback_slot" in patch and "callback_slot.load(std::memory_order_acquire)" in patch),
        ("engine callback reclamation is reader-safe", "callback_readers.fetch_add" in patch and "callback_readers.fetch_sub" in patch and "callback_readers.load" in patch),
        ("engine callback invocation is synchronous", "slot->callable.call(p_packet)" in patch and "call_deferred" not in patch),
        ("engine bridge owns no Terrain3D GPU resource", "Terrain3DRdContext" not in patch and "vertex_buffer_create" not in patch and "index_buffer_create" not in patch),
        ("engine singleton registration and teardown are deterministic", "GDREGISTER_CLASS(Terrain3DForwardPlusBridge)" in patch and "Engine::get_singleton()->add_singleton" in patch and "remove_singleton" in patch and "memdelete(terrain3d_forward_plus_bridge)" in patch),
        ("extension validates all required bridge methods", all(token in bridge_cpp for token in engine_methods)),
        ("extension validates ABI before registration", "abi != s_nBridgeAbiVersion" in bridge_cpp and bridge_cpp.find("abi != s_nBridgeAbiVersion") < bridge_cpp.find('m_pEngineBridge->call( StringName( "register_callback" )')),
        ("extension registration/unregistration exists", "connectEngineBridge" in bridge_cpp and "disconnectEngineBridge" in bridge_cpp and "m_bCallbackRegistered" in bridge_h),
        ("callback retirement is synchronized before extension release", "Terrain3DRdContext::forceSync" in bridge_cpp and "collect_retired_callbacks" in bridge_cpp),
        ("coordinator fanout is fixed and atomic", "std::array<std::atomic<Terrain3DRenderCoordinator *>" in bridge_h and "s_nMaxCoordinators = 64u" in bridge_h),
        ("dynamic packet is parsed once at bridge boundary", "parsePacket( rPacket, context" in bridge_cpp and "renderForwardPass( context )" in bridge_cpp),
        ("typed pass context reserves complete F1 scene ABI", all(token in types for token in packet_fields)),
        ("typed pass enum reserves F2-F6 pass kinds", all(token in types for token in pass_tokens)),
        ("view arrays are bounded", "s_nRdForwardMaxViews = 2u" in types and "value > ( int64_t )( s_nRdForwardMaxViews )" in bridge_cpp),
        ("malformed packets are rejected", "m_nPacketsRejected" in bridge_h and "Forward+ bridge packet missing required field" in bridge_cpp and "return false" in bridge_cpp),
        ("native draw-list identity reaches coordinator", "m_nLastDrawList" in bridge_h and "m_nDrawList" in coordinator_cpp and "renderForwardPass( const t3dRdForwardPassContext_t" in coordinator_h),
        ("coordinator has dedicated Forward+ entry point", "renderForwardPass( const t3dRdForwardPassContext_t &rContext )" in coordinator_h and "Terrain3DRenderCoordinator::renderForwardPass" in coordinator_cpp),
        ("coordinator retains dedicated synchronous Forward+ entry point", "Terrain3DRenderCoordinator::renderForwardPass" in coordinator_cpp and "T3D_RD_FORWARD_PHASE_PREPARE" in coordinator_cpp and "T3D_RD_FORWARD_PHASE_SUBMIT" in coordinator_cpp),
        ("production route requires ready bridge", "isForwardBridgeReady" in coordinator_cpp and "m_bRenderEnabled" in coordinator_cpp),
        ("bridge absence and ABI failures emit a visible engine error", "UtilityFunctions::push_error" in bridge_cpp and "production RenderingDevice rendering is disabled" in bridge_cpp and "ABI mismatch" in bridge_cpp),
        ("missing bridge disables RD production rather than proxy fallback", "bBridgeReady && !m_bFailureLatched" in coordinator_cpp and "setRdPresentation" not in coordinator_cpp and "enforceRdPresentation" not in coordinator_cpp),
        ("compositor lane is explicit debug-only", "TERRAIN3D_RD_DEBUG_COMPOSITOR" in coordinator_cpp and "set_enabled( false )" in compositor_cpp),
        ("adapter lifecycle registered with extension", "Terrain3DRdForwardPlusBridgeAdapter::initializeSingleton" in register_cpp and "Terrain3DRdForwardPlusBridgeAdapter::shutdownSingleton" in register_cpp),
        ("Windows project includes F1 adapter source", "terrain_3d_rd_forward_plus_bridge.cpp" in vcxproj and "terrain_3d_rd_forward_plus_bridge.h" in vcxproj),
        ("Windows project filters include F1 adapter source", "terrain_3d_rd_forward_plus_bridge.cpp" in vcxproj_filters and "terrain_3d_rd_forward_plus_bridge.h" in vcxproj_filters),
        ("engine patch targets exact stable source revision in docs", exact_revision in readme and exact_revision in doc and "4.7.2-stable" in readme),
        ("engine patch base blobs match exact 4.7.2-stable files", all(token in patch for token in ("index 370d3b66ce", "index be97ee2d96", "index de40333785"))),
        ("engine patch is syntactically parseable by git", run_quiet(["git", "apply", "--stat", str(patch_path)], label="batch_f1_patch_parse")),
        ("native render helper submits while Godot draw list remains open", "_terrain3d_forward_plus_submit(draw_list" in patch and patch.find("_terrain3d_forward_plus_submit(draw_list") < patch.find("draw_list_end();", patch.find("_terrain3d_forward_plus_submit(draw_list"))),
        ("engine patch does not begin a Terrain3D-owned draw list", not any(line.startswith("+") and "draw_list_begin(" in line for line in patch.splitlines()) and "_terrain3d_forward_plus_submit" in patch),
        ("depth hook is explicit", "Terrain3DForwardPlusBridge::PASS_DEPTH, p_render_data" in patch),
        ("opaque hook is explicit", "Terrain3DForwardPlusBridge::PASS_OPAQUE, p_render_data" in patch),
        ("motion hook is explicit", "Terrain3DForwardPlusBridge::PASS_MOTION, p_render_data" in patch),
        ("shadow hook is explicit", "Terrain3DForwardPlusBridge::PASS_SHADOW" in patch and "&shadow_pass" in patch),
        ("F6-relevant borrowed resources are present in engine packet", all(token in patch for token in ("lightmap_buffer", "reflection_probe_buffer", "decal_buffer", "radiance_texture"))),
        ("engine patch documents no hidden queue", "synchronous" in readme.lower() and "no hidden" in readme.lower()),
        ("F1 architecture documentation exists", "Forward+ bridge" in doc and "F2" in doc and "RenderingDevice" in doc),
    ]

    failed = [label for label, passed in checks if not passed]
    if failed:
        for label in failed:
            print(f"FAIL Batch F1 Forward+ bridge contract: {label}")
        print(f"Batch F1 Forward+ bridge contract: {len(checks) - len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch F1 Forward+ bridge contract: {len(checks)}/{len(checks)} passed")
    return True

def rd_batch_f2_native_depth_opaque_check() -> bool:
    rd_dir = REPO / "src" / "rendering" / "rd"
    types = (rd_dir / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    bridge_cpp = (rd_dir / "terrain_3d_rd_forward_plus_bridge.cpp").read_text(encoding="utf-8", errors="ignore")
    clip_h = (rd_dir / "terrain_3d_rd_clipmap_renderer.h").read_text(encoding="utf-8", errors="ignore")
    clip_cpp = (rd_dir / "terrain_3d_rd_clipmap_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    inst_h = (rd_dir / "terrain_3d_rd_instance_renderer.h").read_text(encoding="utf-8", errors="ignore")
    inst_cpp = (rd_dir / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    coordinator = (rd_dir / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    shader_lib = (rd_dir / "terrain_3d_rd_shader_library.cpp").read_text(encoding="utf-8", errors="ignore")
    patch = (REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch").read_text(encoding="utf-8", errors="ignore")
    shaders = [
        (REPO / "project" / "addons" / "terrain_3d" / "rd" / name).read_text(encoding="utf-8", errors="ignore")
        for name in ("terrain_clipmap_near.glsl", "terrain_clipmap_middle.glsl", "terrain_clipmap_far.glsl", "terrain_instances.glsl")
    ]
    vcxproj = (REPO / "Terrain3D.vcxproj").read_text(encoding="utf-8", errors="ignore")
    filters = (REPO / "Terrain3D.vcxproj.filters").read_text(encoding="utf-8", errors="ignore")

    def function_body(text: str, signature: str, next_signature: str | None = None) -> str:
        start = text.find(signature)
        if start < 0:
            return ""
        if next_signature:
            end = text.find(next_signature, start + len(signature))
            if end >= 0:
                return text[start:end]
        return text[start:]

    clip_submit = function_body(clip_cpp, "bool Terrain3DRdClipmapRenderer::drawForwardView", "bool Terrain3DRdClipmapRenderer::renderForwardPass")
    inst_submit = function_body(inst_cpp, "bool Terrain3DRdInstanceRenderer::drawForwardView", "bool Terrain3DRdInstanceRenderer::renderForwardPass")
    checks = [
        ("ABI2 prepare/submit phase is typed", "T3D_RD_FORWARD_PHASE_PREPARE" in types and "T3D_RD_FORWARD_PHASE_SUBMIT" in types and "native_pass_mode" in patch),
        ("bridge parses native pass mode and depth-prepass state", 'StringName( "native_pass_mode" )' in bridge_cpp and 'StringName( "depth_prepass_active" )' in bridge_cpp),
        ("engine prepares before opening native draw list", "PHASE_PREPARE" in patch and patch.find("PHASE_PREPARE") < patch.find("draw_list_begin")),
        ("engine submits while native draw list is open", "PHASE_SUBMIT" in patch and "_terrain3d_forward_plus_submit(draw_list" in patch),
        ("engine passes combined current and previous VP", "(correction * scene_data->view_projection[view]) * view_matrix" in patch and "(previous_correction * scene_data->prev_view_projection[view]) * previous_view_matrix" in patch),
        ("terrain has native prepare and submit APIs", "prepareForwardPass" in clip_h and "renderForwardPass" in clip_h and "drawForwardView" in clip_cpp),
        ("instances/HLOD have native prepare and submit APIs", "prepareForwardPass" in inst_h and "renderForwardPass" in inst_h and "drawForwardView" in inst_cpp),
        ("native submit paths never begin/end draw lists", "draw_list_begin" not in clip_submit and "draw_list_end" not in clip_submit and "draw_list_begin" not in inst_submit and "draw_list_end" not in inst_submit),
        ("native submit paths never update buffers or create pipelines", "buffer_update" not in clip_submit and "render_pipeline_create" not in clip_submit and "buffer_update" not in inst_submit and "render_pipeline_create" not in inst_submit),
        ("native terrain pipeline is framebuffer keyed", all(t in clip_h for t in ("m_nFramebufferFormat", "m_samples", "m_nViewCount", "m_bReverseCull", "m_bDepthPrepassActive"))),
        ("native instance pipeline is framebuffer keyed", all(t in inst_h for t in ("m_nFramebufferFormat", "m_samples", "m_nViewCount", "m_bReverseCull", "m_bDepthPrepassActive"))),
        ("native main depth is reversed-Z", clip_cpp.count("COMPARE_OP_GREATER_OR_EQUAL") >= 1 and inst_cpp.count("COMPARE_OP_GREATER_OR_EQUAL") >= 1),
        ("opaque depth-prepass uses equality without depth write", "COMPARE_OP_EQUAL" in clip_cpp and "set_enable_depth_write( !key.m_bDepthPrepassActive )" in clip_cpp and "COMPARE_OP_EQUAL" in inst_cpp),
        ("terrain pipeline variants cover native depth attachments", all(t in clip_cpp for t in ("T3D_RD_FORWARD_SHADER_DEPTH", "T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS", "T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI"))),
        ("instance pipeline variants cover native depth attachments", all(t in inst_cpp for t in ("T3D_RD_FORWARD_SHADER_DEPTH", "T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS", "T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI"))),
        ("shader catalog loads all F2 variants", "depth_normal_roughness_voxel_gi" in (rd_dir / "terrain_3d_rd_shader_catalog.cpp").read_text(encoding="utf-8", errors="ignore") and "loadCatalogShader" in shader_lib),
        ("all F2 shaders declare version variants", all("#[versions]" in sh and "depth_normal_roughness_voxel_gi" in sh for sh in shaders)),
        ("native normal/roughness output is view-space", all("view_matrix" in sh and "mat3(frame_data.view_matrix)" in sh for sh in shaders)),
        ("terrain depth retains hole discard behavior", all("outside_region" in sh and "discard" in sh and "T3D_FORWARD_DEPTH" in sh for sh in shaders[:3])),
        ("instance depth retains alpha-scissor/depth-prepass behavior", "MATERIAL_ALPHA_SCISSOR" in shaders[3] and "INSTANCE_DEPTH_PREPASS_PASS" in shaders[3] and "discard" in shaders[3]),
        ("regular opaque excludes alpha blend", "else if ( alpha_blend )" in inst_cpp and "continue;" in inst_submit),
        ("alpha-blend depth prepass remains represented", "T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS" in inst_cpp and "T3D_RD_INSTANCE_DEPTH_PREPASS_PASS" in inst_cpp),
        ("HLOD remains in shared draw packet and indirect stream", "T3D_RD_INSTANCE_HLOD" in inst_cpp and "m_hIndirectBuffer" in inst_submit and "m_drawPackets" in inst_submit),
        ("coordinator routes depth and opaque through native path", "T3D_RD_FORWARD_PASS_DEPTH" in coordinator and "T3D_RD_FORWARD_PASS_OPAQUE" in coordinator and "prepareForwardPass" in coordinator and "renderForwardPass" in coordinator),
        ("submit failure does not mutate RD ownership while list is open", "Do not destroy or rebuild RD resources" in coordinator),
        ("compositor remains explicit debug lane", "TERRAIN3D_RD_DEBUG_COMPOSITOR" in coordinator and "!m_bCompositorDebugEnabled" in coordinator),
        ("shader catalog is in Windows project", "terrain_3d_rd_shader_catalog.h" in vcxproj and "terrain_3d_rd_shader_catalog.h" in filters),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        for label in failed:
            print(f"FAIL Batch F2 native depth/opaque contract: {label}")
        print(f"Batch F2 native depth/opaque contract: {len(checks)-len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch F2 native depth/opaque contract: {len(checks)}/{len(checks)} passed")
    return True



def rd_batch_f3_native_shadow_check() -> bool:
    rd = REPO / "src" / "rendering" / "rd"
    types = (rd / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    bridge = (rd / "terrain_3d_rd_forward_plus_bridge.cpp").read_text(encoding="utf-8", errors="ignore")
    clip = (rd / "terrain_3d_rd_clipmap_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    inst = (rd / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    coord = (rd / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    patch = (REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch").read_text(encoding="utf-8", errors="ignore")
    ledger = (REPO / "docs" / "rd_contract_ledger.tsv").read_text(encoding="utf-8", errors="ignore")
    checks = [
        ("current ABI retains F3 shadow packet on engine and extension sides", "s_nRdForwardBridgeAbiVersion = 10u" in types and "ABI_VERSION = 10" in patch),
        ("shadow packet carries exact light/cascade identity", all(x in patch for x in ("shadow_light_instance", "shadow_pass_index", "shadow_light_type")) and all(x in bridge for x in ('StringName( "shadow_light_instance" )', 'StringName( "shadow_pass_index" )'))),
        ("shadow packet carries masks/bias/range/atlas metadata", all(x in patch for x in ("shadow_caster_mask", "shadow_light_cull_mask", "shadow_bias", "shadow_normal_bias", "shadow_bias_scale", "shadow_range_begin", "shadow_range", "shadow_atlas_rect", "shadow_atlas_size"))),
        ("shadow VP uses exact light projection and transform", "p_projection * shadow_view" in patch),
        ("coordinator routes native shadow pass", "T3D_RD_FORWARD_PASS_SHADOW" in coord and "m_nShadowCasterMask & rContext.m_nShadowLightCullMask" in coord),
        ("terrain shadow uses native depth shader and supplied list", "T3D_RD_FORWARD_PASS_SHADOW" in clip and "T3D_RD_FORWARD_SHADER_DEPTH" in clip and "draw_list_draw_indirect" in clip),
        ("terrain shadow policy uses near/middle/far caster settings", all(x in clip for x in ("m_bNearShadowEnabled", "m_bMiddleShadowEnabled", "m_bFarShadowEnabled", "m_nNearShadowLodCount", "m_nMiddleShadowLodCount", "m_nFarShadowLodCount"))),
        ("terrain shadow reculls every native shadow pass", "const bool shadow_pass" in clip and "const bool recull = shadow_pass" in clip),
        ("terrain shadow pipeline has directional reverse-cull state", "POLYGON_CULL_FRONT" in clip and "POLYGON_CULL_BACK" in clip and "m_passKind" in clip),
        ("instances have dedicated shadow visibility and indirect buffers", all(x in inst for x in ("m_hShadowVisibleIndexBuffer", "m_hShadowIndirectBuffer", "m_hShadowUniformSet", "uploadShadowVisibility"))),
        ("instance/HLOD shadow submission is grouped indirect", "m_shadowPackets" in inst and "m_shadowVisibleIndices" in inst and "shadow_pass ? m_hShadowIndirectBuffer" in inst),
        ("shadow instance path respects caster layers", "packet.m_nVisibilityLayers & rContext.m_nShadowCasterMask" in inst),
        ("alpha blended non-prepass surfaces are excluded from shadow", "bShadowPass && alpha_blend" in inst and "T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS" in inst),
        ("double-sided shadow material behavior is represented", "T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED" in inst),
        ("selected instance LOD is not recomputed from light-camera distance", "if ( !bShadowPass )" in inst),
        ("RD production contains no native visual/shadow proxy path", "setRdPresentation" not in coord and "enforceRdPresentation" not in coord and 'result["native_shadow_proxy_active"] = false' in coord),
        ("shadow native submit paths do not open a draw list", "PASS_SHADOW" in patch and "_render_list_with_draw_list" in patch),
        ("F3 contract ledger records Section 17 completion", "17\tShadows\tF\tcomplete" in ledger),
        ("engine patch remains syntactically parseable", run_quiet(["git", "apply", "--stat", str(REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch")], label="batch_f3_patch_parse")),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        for label in failed: print(f"FAIL Batch F3 native shadow contract: {label}")
        print(f"Batch F3 native shadow contract: {len(checks)-len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch F3 native shadow contract: {len(checks)}/{len(checks)} passed")
    return True



def rd_batch_f4_native_lighting_check() -> bool:
    rd = REPO / "src" / "rendering" / "rd"
    types = (rd / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    bridge_h = (rd / "terrain_3d_rd_forward_plus_bridge.h").read_text(encoding="utf-8", errors="ignore")
    bridge = (rd / "terrain_3d_rd_forward_plus_bridge.cpp").read_text(encoding="utf-8", errors="ignore")
    lighting_h = (rd / "terrain_3d_rd_forward_lighting.h").read_text(encoding="utf-8", errors="ignore")
    lighting = (rd / "terrain_3d_rd_forward_lighting.cpp").read_text(encoding="utf-8", errors="ignore")
    clip = (rd / "terrain_3d_rd_clipmap_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    inst = (rd / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    shader_lib = (rd / "terrain_3d_rd_shader_library.cpp").read_text(encoding="utf-8", errors="ignore")
    include = (REPO / "project" / "addons" / "terrain_3d" / "rd" / "terrain_forward_lighting_inc.glslinc").read_text(encoding="utf-8", errors="ignore")
    terrain = [(REPO / "project" / "addons" / "terrain_3d" / "rd" / f"terrain_clipmap_{tier}.glsl").read_text(encoding="utf-8", errors="ignore") for tier in ("near", "middle", "far")]
    instance_shader = (REPO / "project" / "addons" / "terrain_3d" / "rd" / "terrain_instances.glsl").read_text(encoding="utf-8", errors="ignore")
    patch = (REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch").read_text(encoding="utf-8", errors="ignore")
    ledger = (REPO / "docs" / "rd_contract_ledger.tsv").read_text(encoding="utf-8", errors="ignore")
    vcx = (REPO / "Terrain3D.vcxproj").read_text(encoding="utf-8", errors="ignore")
    filters = (REPO / "Terrain3D.vcxproj.filters").read_text(encoding="utf-8", errors="ignore")
    exact_cluster = all(token in include for token in (
        "cluster_pos = uvec2(gl_FragCoord.xy) >> cluster_shift",
        "(cluster_width * cluster_pos.y + cluster_pos.x) * stride",
        "item_min = item_min_max & 0xffffu",
        "item_max = item_min_max >> 16u",
        "item_from = min(item_min >> 5u, max_words)",
        "t3d_cluster_range_mask(word, item_min, item_max)",
        "cluster_type_size * type_index",
    ))
    checks = [
        ("current ABI retains F4 native-light packet", "s_nRdForwardBridgeAbiVersion = 10u" in types and "s_nBridgeAbiVersion = s_nT3dForwardBridgeAbi" in bridge_h and "ABI_VERSION = 10" in patch),
        ("bridge exposes exact native cluster metadata", all(x in types for x in ("m_nClusterShift", "m_nClusterWidth", "m_nClusterTypeSize", "m_nClusterMaxElementsDiv32")) and all(x in patch for x in ("cluster_shift", "cluster_width", "cluster_type_size", "cluster_max_elements_div_32"))),
        ("bridge exposes directional count and camera depth range", all(x in types for x in ("m_nDirectionalLightCount", "m_bDirectionalLightSoftShadows", "m_fCameraZNear", "m_fCameraZFar")) and all(x in bridge for x in ("directional_light_count", "directional_light_soft_shadows", "camera_z_near", "camera_z_far"))),
        ("native directional/omni/spot/area/cluster buffers are borrowed", all(x in lighting for x in ("m_hDirectionalLightBuffer", "m_hOmniLightBuffer", "m_hSpotLightBuffer", "m_hAreaLightBuffer", "m_hClusterBuffer")) and "registerResource( rContext.m_h" not in lighting),
        ("lighting descriptors are cached by native RID key", "t3dRdForwardLightKey_t" in lighting_h and "if ( !m_bKeyValid || !( key == m_key ) )" in lighting and "freeSets();" in lighting),
        ("descriptor creation is outside native submit bodies", "uniform_set_create" in lighting and "uniform_set_create" not in clip[clip.find("bool Terrain3DRdClipmapRenderer::drawForwardView"):clip.find("bool Terrain3DRdClipmapRenderer::renderForwardPass")] and "uniform_set_create" not in inst[inst.find("bool Terrain3DRdInstanceRenderer::drawForwardView"):inst.find("bool Terrain3DRdInstanceRenderer::renderForwardPass")]),
        ("terrain and instance/HLOD bind native lighting set 2", "draw_list_bind_uniform_set( draw_list, lighting_set, 2u )" in clip and "draw_list_bind_uniform_set( draw_list, lighting_set, 2u )" in inst),
        ("native color shaders are isolated from diagnostic color", "color_native" in shader_lib and all('color_native = "#define T3D_FORWARD_NATIVE_LIGHTING"' in sh for sh in terrain + [instance_shader])),
        ("shared shader mirrors Godot native light structs", all(x in include for x in ("T3DNativeLightData", "T3DNativeDirectionalLightData", "shadow_matrix1", "shadow_split_offsets", "atlas_rect", "area_width", "area_height", "cone_attenuation", "specular_amount", "mask"))),
        ("Godot 4.7.2 cluster addressing is implemented", exact_cluster),
        ("cluster item words are bounds-clamped", "min(item_min >> 5u, max_words)" in include and "min(item_max == 0u" in include and "cluster_pos.y >= cluster_height" in include),
        ("directional light comes from native buffer", "t3d_directional_lights.data[i]" in include and "light.color * light.energy" in include),
        ("omni lights use native range/attenuation and clustered membership", "t3d_omni_lights.data[light_index]" in include and "t3d_native_attenuation" in include),
        ("spot lights use native cone/range/attenuation", "t3d_spot_lights.data[light_index]" in include and "spot_rim" in include and "light.cone_attenuation" in include),
        ("area lights use native one-sided rectangle/closest-point behavior", all(x in include for x in ("t3d_area_lights.data[light_index]", "dot(light.direction, view_position - light.position) <= 0.0", "closest_point = light.position", "light.inv_radius", "width_length * height_length"))),
        ("directional shadows use native cascades/bias/opacity/sampler", all(x in include for x in ("t3d_directional_shadow", "shadow_split_offsets", "shadow_normal_bias", "shadow_opacity", "t3d_directional_shadow_atlas", "sampler2DShadow"))),
        ("omni and spot shadows use native atlas/bias representation", all(x in include for x in ("t3d_omni_shadow", "flip_offset = light.direction.xy", "local_normal = normalize(mat3(light.shadow_matrix) * view_normal)", "1.0 - abs(dot(local_normal, shadow_dir))", "t3d_spot_shadow", "light.atlas_rect", "light.shadow_matrix"))),
        ("area shadow receiving follows native dual-paraboloid representation", "t3d_area_shadow" in include and "light.cone_attenuation" in include and "light.soft_shadow_size > 0.0" in include),
        ("surface/light masks gate all native contributions", "(light.mask & surface_mask) == 0u" in include and "push_data.visibility_layers" in instance_shader and "m_nRenderLayers" in clip),
        ("terrain keeps ambient/AO/fog/material response", all("frame_data.light_direction_ambient.w * ao" in sh and "apply_fog(lit)" in sh and "material.roughness" in sh and "material.metallic" in sh for sh in terrain)),
        ("instance hardcoded light is diagnostic-only", "#ifdef T3D_FORWARD_NATIVE_LIGHTING" in instance_shader and "debug_light_direction" in instance_shader and "t3d_forward_direct_lighting" in instance_shader),
        ("regular instances and HLOD share the native lighting shader path", "m_drawPackets" in inst and "T3D_RD_INSTANCE_HLOD" in inst and "m_hszForwardShaders[variant]" in inst),
        ("F4 lighting adapter is in Windows project files", "terrain_3d_rd_forward_lighting.cpp" in vcx and "terrain_3d_rd_forward_lighting.h" in vcx and "terrain_3d_rd_forward_lighting.cpp" in filters),
        ("F4 direct-light evidence remains in converged F6 lighting ledger", "18\tLighting\tF\tcomplete" in ledger and "Batch F4 supplies native directional/omni/spot/area lighting" in ledger),
        ("engine patch remains syntactically parseable", run_quiet(["git", "apply", "--stat", str(REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch")], label="batch_f4_patch_parse")),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        for label in failed: print(f"FAIL Batch F4 native lighting contract: {label}")
        print(f"Batch F4 native lighting contract: {len(checks)-len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch F4 native lighting contract: {len(checks)}/{len(checks)} passed")
    return True



def rd_batch_f5_motion_check() -> bool:
    rd = REPO / "src" / "rendering" / "rd"
    types = (rd / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    bridge_h = (rd / "terrain_3d_rd_forward_plus_bridge.h").read_text(encoding="utf-8", errors="ignore")
    bridge = (rd / "terrain_3d_rd_forward_plus_bridge.cpp").read_text(encoding="utf-8", errors="ignore")
    clip = (rd / "terrain_3d_rd_clipmap_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    clip_h = (rd / "terrain_3d_rd_clipmap_renderer.h").read_text(encoding="utf-8", errors="ignore")
    inst = (rd / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    inst_h = (rd / "terrain_3d_rd_instance_renderer.h").read_text(encoding="utf-8", errors="ignore")
    coord = (rd / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    patch = (REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch").read_text(encoding="utf-8", errors="ignore")
    ledger = (REPO / "docs" / "rd_contract_ledger.tsv").read_text(encoding="utf-8", errors="ignore")
    shaders = [(REPO / "project" / "addons" / "terrain_3d" / "rd" / f).read_text(encoding="utf-8", errors="ignore") for f in (
        "terrain_clipmap_near.glsl", "terrain_clipmap_middle.glsl", "terrain_clipmap_far.glsl", "terrain_instances.glsl")]
    terrain = shaders[:3]
    instance = shaders[3]
    exact_motion = all("out_motion = (previous_clip_xy - current_clip_xy) * 0.5" in sh and
                       "current_screen_position.xy / current_screen_position.w - frame_data.motion_jitter.xy" in sh and
                       "previous_screen_position.xy / previous_screen_position.w - frame_data.motion_jitter.zw" in sh
                       for sh in shaders)
    checks = [
        ("current ABI retains F5 temporal packet", "s_nRdForwardBridgeAbiVersion = 10u" in types and "s_nBridgeAbiVersion = s_nT3dForwardBridgeAbi" in bridge_h and "ABI_VERSION = 10" in patch),
        ("bridge packet carries TAA jitter and history validity", all(x in types for x in ("m_taaJitter", "m_previousTaaJitter", "m_bMotionVectorsEnabled")) and all(x in bridge for x in ("taa_jitter", "previous_taa_jitter", "motion_vectors_enabled")) and all(x in patch for x in ('packet["taa_jitter"]', 'packet["previous_taa_jitter"]', 'packet["motion_vectors_enabled"]'))),
        ("engine bridge constructs corrected jittered current and previous VP", "correction.set_depth_correction(scene_data->flip_y)" in patch and "correction.add_jitter_offset(scene_data->taa_jitter)" in patch and "previous_correction.set_depth_correction(true)" in patch and "previous_correction.add_jitter_offset(scene_data->prev_taa_jitter)" in patch),
        ("motion hook submits on native draw list with depth-prepass state", "PASS_MOTION" in patch and "radiance_texture, nullptr, depth_pre_pass" in patch),
        ("coordinator accepts and diagnoses native motion callbacks", "T3D_RD_FORWARD_PASS_MOTION" in coord and "m_nMotionPassCallbacks" in coord and 'result["native_motion_vectors_active"]' in coord),
        ("terrain native motion pass is selectable", "T3D_RD_FORWARD_PASS_MOTION" in clip and "T3D_RD_FORWARD_SHADER_COLOR_MOTION" in clip),
        ("instance/HLOD native motion pass is selectable", "T3D_RD_FORWARD_PASS_MOTION" in inst and "T3D_RD_FORWARD_SHADER_COLOR_MOTION" in inst),
        ("terrain frame UBO carries previous VP and jitter", "m_mPreviousViewProjection" in types and "m_szMotionJitter" in types and "m_szMotionParams" in types and "sizeof( t3dRdFrameUniforms_t ) == 512u" in types),
        ("instance frame UBO carries previous VP and jitter", "m_szPreviousViewProjection" in inst and "m_szMotionJitter" in inst and "sizeof( t3dRdInstanceFrame_t ) == 384u" in inst),
        ("velocity matches Godot previous-UV minus current-UV convention", exact_motion),
        ("terrain motion uses stable world position across clipmap snaps", all("frame_data.previous_view_projection[t3d_view_index()] * vec4(world_position, 1.0)" in sh for sh in terrain) and all(sh.count("previous_origin_x") == 1 and sh.count("previous_scale") == 1 for sh in terrain)),
        ("terrain motion history resets to neutral current clip", all("frame_data.motion_params.x > 0.5" in sh and ": current_clip" in sh for sh in terrain)),
        ("instance shader consumes persistent previous-record stream", "PreviousInstanceRecords" in instance and "previous_records[record_index]" in instance and "instance_world_position" in instance),
        ("previous instance GPU buffer is persistent and bound", "m_hPreviousInstanceBuffer" in inst_h and "T3D.Instance.PreviousRecords" in inst and "binding = 4" in instance),
        ("regular-instance history uses stable cell identity without hot hash", "t3dRdMotionCellsMatch" in inst and "m_nRegionX" in inst and "m_nCellX" in inst and "m_nMeshAssetId" in inst and "unordered_map" not in inst),
        ("HLOD replacement history is revision-safe", "current_hlod" in inst and "m_nMeshSourceKey == rPrevious.m_nMeshSourceKey" in inst and "m_nRevision == rPrevious.m_nRevision" in inst),
        ("new/replacement instance history is neutral without camera-global resets", "reset_history = m_motionHistoryRecords.empty()" in inst and "m_motionPreviousRecords = rScene.m_instances" in inst and "bCameraHistoryValid" not in inst),
        ("stopped instance history advances after one rendered frame", "m_bMotionHistoryNeedsAdvance" in inst and "a stopped object does not keep emitting the same velocity forever" in inst),
        ("motion path writes native velocity attachment rather than private texture", all("layout(location = 2) out vec2 out_motion" in sh for sh in shaders) and "velocity_texture" not in clip and "velocity_texture" not in inst),
        ("motion redraw retains native lighting", "rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION" in clip and "m_forwardLighting.prepare( rContext" in clip and "rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION" in inst and "m_forwardLighting.prepare( rContext" in inst),
        ("motion pipeline honors prepass EQUAL path", "T3D_RD_FORWARD_PASS_MOTION ) && rContext.m_bDepthPrepassActive" in clip and "T3D_RD_FORWARD_PASS_MOTION ) && rContext.m_bDepthPrepassActive" in inst and "COMPARE_OP_EQUAL" in clip and "COMPARE_OP_EQUAL" in inst),
        ("deterministic motion visualization is explicit opt-in", "TERRAIN3D_RD_DEBUG_MOTION" in coord and "setMotionDebugEnabled" in clip_h and "setMotionDebugEnabled" in inst_h and all("motion_params.y > 0.5" in sh for sh in shaders)),
        ("F5 ledger records Section 20 completion", "20\tMotion vectors\tF\tcomplete" in ledger),
        ("engine patch remains syntactically parseable", run_quiet(["git", "apply", "--stat", str(REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch")], label="batch_f5_patch_parse")),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        for label in failed: print(f"FAIL Batch F5 motion contract: {label}")
        print(f"Batch F5 motion contract: {len(checks)-len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch F5 motion contract: {len(checks)}/{len(checks)} passed")
    return True


def rd_batch_f6_scene_integration_check() -> bool:
    rd = REPO / "src" / "rendering" / "rd"
    types = (rd / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    bridge_h = (rd / "terrain_3d_rd_forward_plus_bridge.h").read_text(encoding="utf-8", errors="ignore")
    bridge = (rd / "terrain_3d_rd_forward_plus_bridge.cpp").read_text(encoding="utf-8", errors="ignore")
    lighting_h = (rd / "terrain_3d_rd_forward_lighting.h").read_text(encoding="utf-8", errors="ignore")
    lighting = (rd / "terrain_3d_rd_forward_lighting.cpp").read_text(encoding="utf-8", errors="ignore")
    coord = (rd / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    data_h = (REPO / "src" / "data" / "terrain_3d_data.h").read_text(encoding="utf-8", errors="ignore")
    data = (REPO / "src" / "data" / "terrain_3d_data.cpp").read_text(encoding="utf-8", errors="ignore")
    cache_h = (REPO / "src" / "streaming" / "terrain_3d_gpu_region_cache.h").read_text(encoding="utf-8", errors="ignore")
    cache = (REPO / "src" / "streaming" / "terrain_3d_gpu_region_cache.cpp").read_text(encoding="utf-8", errors="ignore")
    include = (REPO / "project" / "addons" / "terrain_3d" / "rd" / "terrain_forward_lighting_inc.glslinc").read_text(encoding="utf-8", errors="ignore")
    terrain = [(REPO / "project" / "addons" / "terrain_3d" / "rd" / f"terrain_clipmap_{tier}.glsl").read_text(encoding="utf-8", errors="ignore") for tier in ("near", "middle", "far")]
    instance = (REPO / "project" / "addons" / "terrain_3d" / "rd" / "terrain_instances.glsl").read_text(encoding="utf-8", errors="ignore")
    baker = (REPO / "project" / "addons" / "terrain_3d" / "tools" / "region_lightmap_baker.gd").read_text(encoding="utf-8", errors="ignore")
    patch_path = REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch"
    patch = patch_path.read_text(encoding="utf-8", errors="ignore")
    doc = (REPO / "docs" / "rd_batch_scene_integrations.md").read_text(encoding="utf-8", errors="ignore")
    ledger = (REPO / "docs" / "rd_contract_ledger.tsv").read_text(encoding="utf-8", errors="ignore")

    borrowed_packet = (
        "m_hLightmapBuffer", "m_hLightmapCaptureBuffer", "m_hReflectionProbeBuffer",
        "m_hReflectionAtlasTexture", "m_hRadianceTexture", "m_hDecalBuffer",
        "m_hDecalAtlasTexture", "m_hDecalAtlasSrgbTexture",
    )
    native_reflection = (
        "T3DNativeReflectionData", "box_extents", "box_offset", "blend_distance",
        "box_project", "ambient_mode", "exposure_normalization", "local_matrix",
    )
    native_decal = (
        "T3DNativeDecalData", "albedo_mix", "albedo_rect", "normal_rect", "orm_rect",
        "emission_rect", "emission_energy", "upper_fade", "lower_fade", "normal_xform", "normal_fade",
    )
    terrain_integrated = all(
        "terrain_shadow_masks" in sh and "region_shadowmask_mode" in sh and
        "t3d_forward_apply_decals" in sh and "t3d_forward_reflection" in sh
        for sh in terrain
    )
    submit_slices = []
    for source, begin, end in (
        ((rd / "terrain_3d_rd_clipmap_renderer.cpp").read_text(encoding="utf-8", errors="ignore"), "bool Terrain3DRdClipmapRenderer::drawForwardView", "bool Terrain3DRdClipmapRenderer::renderForwardPass"),
        ((rd / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore"), "bool Terrain3DRdInstanceRenderer::drawForwardView", "bool Terrain3DRdInstanceRenderer::renderForwardPass"),
    ):
        i = source.find(begin); j = source.find(end, i + 1)
        submit_slices.append(source[i:j] if i >= 0 and j > i else "")

    checks = [
        ("ABI 10 retains the complete F6 scene-integration packet", "s_nRdForwardBridgeAbiVersion = 10u" in types and "s_nBridgeAbiVersion = s_nT3dForwardBridgeAbi" in bridge_h and "ABI_VERSION = 10" in patch),
        ("bridge packet carries all F6 borrowed scene resources", all(x in types for x in borrowed_packet) and all(x in bridge for x in ("lightmap_buffer", "reflection_probe_buffer", "reflection_atlas_texture", "radiance_texture", "decal_buffer", "decal_atlas_texture", "decal_atlas_srgb_texture", "reflection_atlas_border_size")) and all(x in patch for x in ("lightmap_buffer", "reflection_probe_buffer", "reflection_atlas_texture", "radiance_texture", "decal_buffer", "decal_atlas_texture", "decal_atlas_srgb_texture", "reflection_atlas_border_size"))),
        ("shader mirrors Godot 4.7.2 reflection data layout", all(x in include for x in native_reflection)),
        ("shader mirrors Godot 4.7.2 decal data layout", all(x in include for x in native_decal)),
        ("native cluster element IDs use decal 3 and reflection 4", "T3D_CLUSTER_TYPE_DECAL = 3u" in include and "T3D_CLUSTER_TYPE_REFLECTION = 4u" in include),
        ("decal path respects masks projection fades and SRGB/linear atlases", all(x in include for x in ("t3d_forward_apply_decals", "decal.mask & surface_mask", "uv_local", "upper_fade", "lower_fade", "normal_fade", "t3d_decal_atlas_srgb", "t3d_decal_atlas", "albedo_mix", "orm_rect", "emission_rect"))),
        ("reflection path implements masks bounds blend box projection roughness LOD and native atlas border", all(x in include for x in ("t3d_forward_reflection", "probe.mask & surface_mask", "box_extents", "blend_distance", "box_project", "sqrt(clamp(roughness, 0.0, 1.0)) * 5.0", "t3d_vec3_to_oct_with_border", "depth_params.w", "exposure_normalization", "probe.intensity")) and "m_fReflectionAtlasBorderSize" in types),
        ("terrain near/middle/far consume decals probes and regional shadowmask", terrain_integrated),
        ("regular instances and HLOD share decal/probe shader path", "t3d_forward_apply_decals" in instance and "t3d_forward_reflection" in instance and "T3D_RD_INSTANCE_HLOD" in (rd / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore")),
        ("regional baker extracts a dedicated terrain shadowmask", all(x in baker for x in ("_extract_terrain_shadow_mask", "shadowmask_textures", "Image.FORMAT_L8", "generate_mipmaps", "terrain_shadow_mask_path"))),
        ("regional baker strips temporary Terrain3D bake users", "Terrain3DBakeProxy" in baker and "temporary" in baker.lower() and "baked_user_data" in baker),
        ("F6 audit records no fabricated VoxelGI/SDFGI receiver path", "No Terrain3D project/fork runtime path was found" in doc and "does not claim custom VoxelGI/SDFGI injection" in doc),
        ("full-residency shadowmask refresh updates existing layers in place", "refreshTerrainShadowMasksInternal" in data_h and "m_generatedTerrainShadowMasks.update( shadow_masks[slot], slot )" in data),
        ("bounded-residency shadowmask refresh updates mapped physical layers in place", "refreshTerrainShadowMasks" in cache_h and "m_generatedTerrainShadowMasks.update( pShadowMask, layer )" in cache and "TERRAIN3D_GPU_SLOT_MAPPED" in cache),
        ("coordinator refreshes resident masks on lighting-signature change", "bRegionLightingChanged" in coord and "refreshTerrainShadowMasksInternal()" in coord and "Unable to refresh Terrain3D regional shadowmask residency after lighting change" in coord),
        ("packed region map carries shadowmask mode with physical layer identity", all("region_shadowmask_mode" in sh and ">> 16u" in sh for sh in terrain) and "shadowmask" in coord.lower()),
        ("F6 extends persistent descriptor key rather than allocating per frame", all(x in lighting_h for x in ("m_hReflectionProbeBuffer", "m_hReflectionAtlas", "m_hDecalBuffer", "m_hDecalAtlas")) and "if ( !m_bKeyValid || !( key == m_key ) )" in lighting),
        ("F6 descriptor binds only actually consumed reflection/decal resources", all(f"{n}," in lighting for n in range(9, 15)) and "t3d_lightmaps" not in include and "m_hLightmapBuffer" not in lighting_h and "uniform_set_create" in lighting),
        ("native SUBMIT draw bodies do not allocate/update/create lists", all(body and all(x not in body for x in ("uniform_set_create", "buffer_update", "draw_list_begin", "draw_list_end", "render_pipeline_create")) for body in submit_slices)),
        ("RD scene integrations do not enumerate Decal or ReflectionProbe nodes", "get_nodes_in_group" not in lighting and "get_children" not in lighting and "SceneTree" not in lighting and "Node3D" not in lighting),
        ("F6 lighting remains complete after F7 lifecycle closure", "18\tLighting\tF\tcomplete" in ledger and "Forward+ integration is complete at bridge ABI 10" in ledger and "22\tMultiview and viewport lifecycle\tF\tcomplete" in ledger),
        ("engine patch remains syntactically parseable", run_quiet(["git", "apply", "--stat", str(patch_path)], label="batch_f6_patch_parse")),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        for label in failed:
            print(f"FAIL Batch F6 scene integration contract: {label}")
        print(f"Batch F6 scene integration contract: {len(checks)-len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch F6 scene integration contract: {len(checks)}/{len(checks)} passed")
    return True

def rd_batch_f7_multiview_lifecycle_check() -> bool:
    rd = REPO / "src" / "rendering" / "rd"
    types = (rd / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    bridge_h = (rd / "terrain_3d_rd_forward_plus_bridge.h").read_text(encoding="utf-8", errors="ignore")
    bridge = (rd / "terrain_3d_rd_forward_plus_bridge.cpp").read_text(encoding="utf-8", errors="ignore")
    clip_h = (rd / "terrain_3d_rd_clipmap_renderer.h").read_text(encoding="utf-8", errors="ignore")
    clip = (rd / "terrain_3d_rd_clipmap_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    cull = (rd / "terrain_3d_rd_culling.cpp").read_text(encoding="utf-8", errors="ignore")
    inst_h = (rd / "terrain_3d_rd_instance_renderer.h").read_text(encoding="utf-8", errors="ignore")
    inst = (rd / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    lighting_h = (rd / "terrain_3d_rd_forward_lighting.h").read_text(encoding="utf-8", errors="ignore")
    coord_h = (rd / "terrain_3d_render_coordinator.h").read_text(encoding="utf-8", errors="ignore")
    coord = (rd / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    patch_path = REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch"
    patch = patch_path.read_text(encoding="utf-8", errors="ignore")
    ledger = (REPO / "docs" / "rd_contract_ledger.tsv").read_text(encoding="utf-8", errors="ignore")
    doc = (REPO / "docs" / "rd_batch_multiview_lifecycle.md").read_text(encoding="utf-8", errors="ignore")
    shaders = [(REPO / "project" / "addons" / "terrain_3d" / "rd" / name).read_text(encoding="utf-8", errors="ignore") for name in (
        "terrain_clipmap_near.glsl", "terrain_clipmap_middle.glsl", "terrain_clipmap_far.glsl", "terrain_instances.glsl")]
    submit_slices = []
    for source, begin, end in (
        (clip, "bool Terrain3DRdClipmapRenderer::drawForwardView", "bool Terrain3DRdClipmapRenderer::renderForwardPass"),
        (inst, "bool Terrain3DRdInstanceRenderer::drawForwardView", "bool Terrain3DRdInstanceRenderer::renderForwardPass"),
    ):
        i = source.find(begin); j = source.find(end, i + 1)
        submit_slices.append(source[i:j] if i >= 0 and j > i else "")
    checks = [
        ("ABI 10 synchronized across extension and engine contract", "s_nRdForwardBridgeAbiVersion = 10u" in types and "s_nBridgeAbiVersion = s_nT3dForwardBridgeAbi" in bridge_h and "ABI_VERSION = 10" in patch),
        ("bridge carries stable viewport render-frame and sample identity", all(x in types for x in ("m_nRenderFrame", "m_nViewportKey", "m_nSampleCount")) and all(x in bridge for x in ("render_frame", "viewport_key", "sample_count")) and all(x in patch for x in ('packet["render_frame"]', 'packet["viewport_key"]', 'packet["sample_count"]'))),
        ("bridge bounds views and accepted MSAA sample counts", "s_nRdForwardMaxViews = 2u" in types and "view count is outside" in bridge and "value != 1 && value != 2 && value != 4 && value != 8" in bridge),
        ("all native shaders select per-eye state with gl_ViewIndex", all("GL_EXT_multiview" in sh and "t3d_view_index()" in sh and "view_projection[2]" in sh and "previous_view_projection[2]" in sh for sh in shaders)),
        ("terrain frame ABI carries two current/previous matrices and eye cameras", "m_mViewProjection[s_nRdForwardMaxViews][16]" in types and "m_mPreviousViewProjection[s_nRdForwardMaxViews][16]" in types and "m_szCamera[s_nRdForwardMaxViews][4]" in types and "sizeof( t3dRdFrameUniforms_t ) == 512u" in types),
        ("instance/HLOD frame ABI carries two current/previous matrices and eye cameras", "m_szViewProjection[s_nRdForwardMaxViews][16]" in inst and "m_szPreviousViewProjection[s_nRdForwardMaxViews][16]" in inst and "m_szCamera[s_nRdForwardMaxViews][4]" in inst and "sizeof( t3dRdInstanceFrame_t ) == 384u" in inst),
        ("terrain visibility is unioned across native eyes", "Terrain3DRdCpuCuller::buildMultiview" in cull and "rContext.m_szViewProjection, rContext.m_nViewCount" in clip and "rContext.m_nViewCount == 1u && m_bGpuCullingReady" in clip),
        ("regular instance visibility is unioned across native eyes", "const Projection *const pViewProjections" in inst and "for ( uint32_t view = 0u; view < nViewCount && !visible; ++view )" in inst),
        ("multiview HLOD avoids single-eye compute culling", "nViewCount != 1u || ( packet.m_nFlags & T3D_RD_INSTANCE_HLOD )" in inst and "rContext.m_nViewCount == 1u && !dispatchHlodCull" in inst),
        ("instance transform history advances once per engine render frame", "m_nMotionHistoryRenderFrame" in inst_h and "prepareMotionHistory( *pScene, rContext.m_nRenderFrame )" in inst and "bCameraHistoryValid" not in inst),
        ("fixed-size per-viewport lifecycle table avoids hot map allocation", "s_nForwardViewportSlotCount = 8u" in coord_h and "m_szForwardViewportStates[s_nForwardViewportSlotCount]" in coord_h and "unordered_map" not in coord_h),
        ("resize render-region view-count sample and framebuffer changes invalidate temporal state", all(x in coord for x in ("m_hFramebuffer == rContext.m_hNativeFramebuffer", "m_nFramebufferFormat == rContext.m_nFramebufferFormat", "m_renderSize == rContext.m_renderSize", "m_renderRegion == rContext.m_renderRegion", "m_nViewCount == rContext.m_nViewCount", "m_nSampleCount == rContext.m_nSampleCount", "m_bTemporalHistoryValid = same"))),
        ("all passes in a reconfiguration frame keep temporal history neutral", "m_nReconfiguredRenderFrame" in coord_h and "reconfigured_this_frame" in coord and "same && !reconfigured_this_frame && rContext.m_bMotionVectorsEnabled" in coord and "slot->m_nReconfiguredRenderFrame = rContext.m_nRenderFrame" in coord),
        ("viewport recreation invalidates borrowed descriptor sets outside SUBMIT", "m_bViewportReconfigured" in types and "invalidateBorrowedState" in lighting_h and "rContext.m_bViewportReconfigured" in clip and "rContext.m_bViewportReconfigured" in inst),
        ("pipeline keys include native format pass sample view and depth/cull state", all(x in clip_h for x in ("m_nFramebufferFormat", "m_passKind", "m_samples", "m_nViewCount", "m_bReverseCull", "m_bDepthPrepassActive")) and all(x in inst_h for x in ("m_nFramebufferFormat", "m_passKind", "m_samples", "m_nViewCount", "m_bReverseCull", "m_bDepthPrepassActive"))),
        ("packet sample count is cross-checked against framebuffer format", "sample-count metadata does not match native framebuffer format" in clip and "sample-count metadata does not match native framebuffer format" in inst),
        ("pipeline caches are bounded across resize/MSAA warmups", "m_forwardPipelines.size() >= 64u" in clip and "m_forwardPipelines.size() >= 64u" in inst and "freeForwardPipelines()" in clip and "freeForwardPipelines()" in inst),
        ("PREPARE/SUBMIT identity rejects stale viewport submissions", "m_nForwardPreparedViewportKey != rContext.m_nViewportKey" in coord and "m_nForwardPreparedViewportKey != rContext.m_nViewportKey" in clip and "m_nForwardPreparedViewportKey != rContext.m_nViewportKey" in inst),
        ("viewport/MSAA lifecycle does not rebuild static mesh or material state", "prepareForwardLifecycle" in coord and "m_meshCache" not in coord[coord.find("bool Terrain3DRenderCoordinator::prepareForwardLifecycle"):coord.find("void Terrain3DRenderCoordinator::renderForwardPass")] and "m_material" not in coord[coord.find("bool Terrain3DRenderCoordinator::prepareForwardLifecycle"):coord.find("void Terrain3DRenderCoordinator::renderForwardPass")]),
        ("teardown clears viewport lifecycle state and renderers clear prepared viewport keys", "resetForwardLifecycle();" in coord and "m_nForwardPreparedViewportKey = 0u" in clip and "m_nForwardPreparedViewportKey = 0u" in inst),
        ("native SUBMIT remains bind/draw only", all(body and all(x not in body for x in ("buffer_update", "uniform_set_create", "render_pipeline_create", "draw_list_begin", "draw_list_end")) for body in submit_slices)),
        ("F7 ledger closes Forward+ lifecycle sections", "19\tForward+ integration decision\tF\tcomplete" in ledger and "21\tDepth behavior\tF\tcomplete" in ledger and "22\tMultiview and viewport lifecycle\tF\tcomplete" in ledger),
        ("F7 documentation records stereo lifecycle and no F8 scope", "ABI 7" in doc and "render scale" in doc.lower() and "multiple cameras" in doc.lower() and "F8" in doc),
        ("engine patch remains syntactically parseable", run_quiet(["git", "apply", "--stat", str(patch_path)], label="batch_f7_patch_parse")),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        for label in failed:
            print(f"FAIL Batch F7 multiview/lifecycle contract: {label}")
        print(f"Batch F7 multiview/lifecycle contract: {len(checks)-len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch F7 multiview/lifecycle contract: {len(checks)}/{len(checks)} passed")
    return True



def rd_batch_f8_consolidation_check() -> bool:
    rd = REPO / "src" / "rendering" / "rd"
    coord_h = (rd / "terrain_3d_render_coordinator.h").read_text(encoding="utf-8", errors="ignore")
    coord = (rd / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    bridge = (rd / "terrain_3d_rd_forward_plus_bridge.cpp").read_text(encoding="utf-8", errors="ignore")
    clip = (rd / "terrain_3d_rd_clipmap_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    inst = (rd / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    types = (rd / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    lighting = (rd / "terrain_3d_rd_forward_lighting.cpp").read_text(encoding="utf-8", errors="ignore")
    lighting_inc = (REPO / "project" / "addons" / "terrain_3d" / "rd" / "terrain_forward_lighting_inc.glslinc").read_text(encoding="utf-8", errors="ignore")
    profiler = (REPO / "project" / "addons" / "terrain_3d" / "src" / "runtime_profiler_window.gd").read_text(encoding="utf-8", errors="ignore")
    patch_path = REPO / "engine_patch" / "godot-4.7.2-terrain3d-forward-plus.patch"
    patch = patch_path.read_text(encoding="utf-8", errors="ignore")
    patch_readme = (REPO / "engine_patch" / "README.md").read_text(encoding="utf-8", errors="ignore")
    ledger = (REPO / "docs" / "rd_contract_ledger.tsv").read_text(encoding="utf-8", errors="ignore")
    batches = BATCH_FILE.read_text(encoding="utf-8", errors="ignore")
    doc_path = REPO / "docs" / "rd_batch_consolidation.md"
    doc = doc_path.read_text(encoding="utf-8", errors="ignore") if doc_path.is_file() else ""

    def between(source: str, begin: str, end: str) -> str:
        i = source.find(begin)
        j = source.find(end, i + 1) if i >= 0 else -1
        return source[i:j] if i >= 0 and j > i else ""

    clip_forward = between(clip, "bool Terrain3DRdClipmapRenderer::drawForwardView", "bool Terrain3DRdClipmapRenderer::renderForwardPass")
    inst_forward = between(inst, "bool Terrain3DRdInstanceRenderer::drawForwardView", "bool Terrain3DRdInstanceRenderer::renderForwardPass")
    coord_shutdown = between(coord, "void Terrain3DRenderCoordinator::shutdown", "void Terrain3DRenderCoordinator::update")
    coord_proxy = between(coord, "void Terrain3DRenderCoordinator::enforceRdPresentation", "void Terrain3DRenderCoordinator::attachEffectToActiveCamera")
    clip_pipeline = between(clip, "bool Terrain3DRdClipmapRenderer::ensureForwardPipelines", "bool Terrain3DRdClipmapRenderer::ensureSamplers")
    inst_pipeline = between(inst, "bool Terrain3DRdInstanceRenderer::ensureForwardPipelines", "bool Terrain3DRdInstanceRenderer::ensureOwnedBuffer")
    marker_paths = [rd, REPO / "src" / "lighting", REPO / "engine_patch", REPO / "project" / "addons" / "terrain_3d" / "rd"]
    marker_re = re.compile(r"\b(?:TODO|FIXME|placeholder)\b", re.IGNORECASE)
    marker_hits = []
    for root in marker_paths:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.is_file() and path.suffix.lower() in TEXT_SUFFIXES:
                text = path.read_text(encoding="utf-8", errors="ignore")
                if marker_re.search(text):
                    marker_hits.append(path)

    section_rows = {}
    for line in ledger.splitlines():
        cols = line.split("\t")
        if cols and cols[0] in {"17", "18", "19", "20", "21", "22"}:
            section_rows[cols[0]] = cols
    ledger_complete = all(section_rows.get(str(section), [None, None, None, None])[3] == "complete" for section in range(17, 23))

    prepare_pos = patch.find("PHASE_PREPARE")
    begin_pos = patch.find("draw_list_begin", prepare_pos)
    submit_pos = patch.find("PHASE_SUBMIT", begin_pos)
    end_pos = patch.find("draw_list_end", submit_pos)

    checks = [
        ("F8 convergence documentation exists and records static+compile scope", bool(doc) and "static audit and C++ compilation" in doc and "Runtime, GPU" in doc),
        ("Batch F is marked completed in batch ledger", any(line.startswith("F\t13-15\tcompleted\t17-22\t") for line in batches.splitlines())),
        ("Sections 17-22 remain individually complete", ledger_complete),
        ("engine patch package targets exact Godot 4.7.2 and ABI 10", "4.7.2-stable" in patch_readme and "ABI_VERSION = 10" in patch and "Terrain3DForwardPlusBridge" in patch),
        ("engine bridge exposes native depth opaque shadow and motion pass kinds", all(x in patch for x in ("PASS_DEPTH", "PASS_OPAQUE", "PASS_SHADOW", "PASS_MOTION")) and all(x in types for x in ("T3D_RD_FORWARD_PASS_DEPTH", "T3D_RD_FORWARD_PASS_OPAQUE", "T3D_RD_FORWARD_PASS_SHADOW", "T3D_RD_FORWARD_PASS_MOTION"))),
        ("engine owns draw-list lifetime around synchronous Terrain3D submission", 0 <= prepare_pos < begin_pos < submit_pos < end_pos),
        ("native terrain and instance submit slices never begin/end a draw list", bool(clip_forward) and bool(inst_forward) and all(x not in clip_forward + inst_forward for x in ("draw_list_begin", "draw_list_end"))),
        ("native SUBMIT remains free of persistent resource allocation", all(x not in clip_forward + inst_forward for x in ("buffer_update", "uniform_set_create", "render_pipeline_create"))),
        ("compositor lane remains explicit opt-in debug only", "TERRAIN3D_RD_DEBUG_COMPOSITOR" in coord and "m_bCompositorDebugEnabled" in coord and "m_effect->set_enabled( m_bCompositorDebugEnabled && !m_bFailureLatched )" in coord),
        ("missing bridge disables RD production instead of enabling native geometry", "bBridgeReady && !m_bFailureLatched" in coord and "production RenderingDevice rendering is disabled" in bridge and "setRdPresentation( true" not in coord),
        ("RD native proxy path is removed", "setRdPresentation" not in coord and "enforceRdPresentation" not in coord),
        ("shutdown cannot resurrect native visual terrain", "setRdPresentation" not in coord_shutdown and "enforceRdPresentation" not in coord_shutdown and "m_bRenderEnabled.store( false" in coord_shutdown),
        ("failure state is a latch and obsolete fallback state is gone", "m_bFailureLatched" in coord_h and "renderer_failure_latched" in coord and "m_bFallbackActive" not in coord_h + coord),
        ("diagnostics never advertise an RD native shadow proxy", 'result["native_shadow_proxy_active"] = false' in coord and 'result["native_shadow_proxy_available"] = false' in coord),
        ("diagnostics derive native depth/opaque/shadow/motion participation from callbacks", all(x in coord for x in ("m_nDepthPassCallbacks", "m_nOpaquePassCallbacks", "m_nShadowPassCallbacks", "m_nMotionPassCallbacks", 'result["native_depth_prepass_participation"]', 'result["native_shadow_participation"]', 'result["native_motion_vectors_active"]'))),
        ("diagnostics expose bridge ABI and callback registration", 'result["forward_plus_bridge_abi"]' in coord and 'result["forward_plus_callback_registered"]' in coord),
        ("diagnostics expose current native view/sample/framebuffer identity", all(x in coord_h + coord for x in ("m_nLastForwardViewCount", "m_nLastForwardSampleCount", "m_nLastForwardFramebufferFormat", "current_forward_view_count", "current_forward_sample_count", "current_forward_framebuffer_format"))),
        ("diagnostics expose clustered-light readiness and borrowed-resource generation changes", "forward_light_resources_ready" in lighting and "forward_scene_resource_changes" in lighting and "clustered_light_path_active" in coord and "borrowed_scene_resource_changes" in coord),
        ("profiler no longer claims native renderer restoration or compositor production submission", "Native renderer restored after RD initialization/render failure" not in profiler and "Compositor RD submission enabled" not in profiler and "renderer_failure_latched" in profiler),
        ("native terrain depth pipeline uses reversed-Z", "COMPARE_OP_GREATER_OR_EQUAL" in clip_pipeline and "COMPARE_OP_LESS_OR_EQUAL" not in clip_pipeline),
        ("native instance/HLOD depth pipeline uses reversed-Z", "COMPARE_OP_GREATER_OR_EQUAL" in inst_pipeline and "COMPARE_OP_LESS_OR_EQUAL" not in inst_pipeline),
        ("terrain production path represents depth opaque shadow and motion", all(x in clip for x in ("T3D_RD_FORWARD_PASS_DEPTH", "T3D_RD_FORWARD_PASS_OPAQUE", "T3D_RD_FORWARD_PASS_SHADOW", "T3D_RD_FORWARD_PASS_MOTION"))),
        ("regular instance/HLOD production path represents depth opaque shadow and motion", all(x in inst for x in ("T3D_RD_FORWARD_PASS_DEPTH", "T3D_RD_FORWARD_PASS_OPAQUE", "T3D_RD_FORWARD_PASS_SHADOW", "T3D_RD_FORWARD_PASS_MOTION", "T3D_RD_INSTANCE_HLOD"))),
        ("native clustered-light shader retains omni spot and area resources", all(x in lighting_inc for x in ("T3DOmniLights", "T3DSpotLights", "T3DAreaLights", "T3DClusterBuffer", "t3d_cluster_range"))),
        ("motion state retains per-view previous transforms and native motion pass", "m_mPreviousViewProjection" in types and "T3D_RD_FORWARD_PASS_MOTION" in coord and "native_motion_vectors_active" in coord),
        ("multiview remains bounded and shader-visible", "s_nRdForwardMaxViews = 2u" in types and "m_nViewCount" in coord_h and "GL_EXT_multiview" in (REPO / "project" / "addons" / "terrain_3d" / "rd" / "terrain_clipmap_near.glsl").read_text(encoding="utf-8", errors="ignore")),
        ("pipeline cache keys retain framebuffer/pass/sample/view identity", all(x in (rd / "terrain_3d_rd_clipmap_renderer.h").read_text(encoding="utf-8", errors="ignore") for x in ("m_nFramebufferFormat", "m_passKind", "m_samples", "m_nViewCount")) and all(x in (rd / "terrain_3d_rd_instance_renderer.h").read_text(encoding="utf-8", errors="ignore") for x in ("m_nFramebufferFormat", "m_passKind", "m_samples", "m_nViewCount"))),
        ("Batch F-owned source contains no TODO/FIXME/placeholder residue", not marker_hits),
        ("F8 documentation records removal of temporary fallback/proxy residue", "failure latch" in doc and "native visual terrain" in doc and "compositor diagnostic lane" in doc),
        ("engine patch remains syntactically parseable", run_quiet(["git", "apply", "--stat", str(patch_path)], label="batch_f8_patch_parse")),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        for label in failed:
            print(f"FAIL Batch F8 consolidation contract: {label}")
        if marker_hits:
            for path in marker_hits[:20]:
                print(f"  marker={path.relative_to(REPO)}")
        print(f"Batch F8 consolidation contract: {len(checks)-len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch F8 consolidation contract: {len(checks)}/{len(checks)} passed")
    return True


def rd_abi10_forward_bridge_check(label: str) -> bool:
    rd_dir = REPO / "src" / "rendering" / "rd"
    bridge_h = (rd_dir / "terrain_3d_rd_forward_plus_bridge.h").read_text(encoding="utf-8")
    bridge_cpp = (rd_dir / "terrain_3d_rd_forward_plus_bridge.cpp").read_text(encoding="utf-8")
    types = (rd_dir / "terrain_3d_rd_types.h").read_text(encoding="utf-8")
    coordinator_h = (rd_dir / "terrain_3d_render_coordinator.h").read_text(encoding="utf-8")
    coordinator_cpp = (rd_dir / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8")
    clipmap_h = (rd_dir / "terrain_3d_rd_clipmap_renderer.h").read_text(encoding="utf-8")
    instance_h = (rd_dir / "terrain_3d_rd_instance_renderer.h").read_text(encoding="utf-8")
    lighting_h = (rd_dir / "terrain_3d_rd_forward_lighting.h").read_text(encoding="utf-8")
    lighting_cpp = (rd_dir / "terrain_3d_rd_forward_lighting.cpp").read_text(encoding="utf-8")
    ocean_cpp = (rd_dir / "terrain_3d_rd_ocean_renderer.cpp").read_text(encoding="utf-8")
    sconstruct = (REPO / "SConstruct").read_text(encoding="utf-8")
    csproj = (REPO / "project" / "Terrain3D.csproj").read_text(encoding="utf-8")
    external_readme = (REPO / "engine_patch" / "README.md").read_text(encoding="utf-8")
    api_path = REPO / "thirdparty" / "godot_api" / "extension_api-4.7.2.json"

    required_methods = (
        "get_abi_version", "get_native_capabilities", "register_native_callback",
        "unregister_native_callback", "has_native_callback", "collect_retired_callbacks",
    )
    required_capabilities = (
        "DIRECT_POD", "CAP_DEPTH", "CAP_OPAQUE", "CAP_SHADOW", "CAP_MOTION",
        "CAP_TRANSPARENT", "CLUSTERED_LIGHTING", "CAP_DECALS", "CAP_REFLECTIONS",
        "RESOURCE_GENERATIONS", "SPLIT_SHADOW_PACKET",
    )
    packet_sizes = ("472u", "120u", "48u", "200u")
    checks = [
        ("ABI 10 is synchronized across adapter and typed context", "s_nBridgeAbiVersion = s_nT3dForwardBridgeAbi" in bridge_h and "s_nRdForwardBridgeAbiVersion = 10u" in types),
        ("all four ABI 10 POD sizes are asserted", all(size in bridge_h for size in packet_sizes)),
        ("ABI packets require standard layout and trivial copies", bridge_h.count("is_standard_layout_v") >= 4 and bridge_h.count("is_trivially_copyable_v") >= 4),
        ("full native capability mask is required", all(token in bridge_h for token in required_capabilities) and "s_nT3dForwardBridgeRequiredCapabilities" in bridge_cpp),
        ("all six cold singleton methods are validated", all(method in bridge_cpp for method in required_methods)),
        ("three native callback addresses and exact sizes are registered", all(token in bridge_cpp for token in ("prepareThunk", "prepareShadowThunk", "submitThunk", "sizeof( t3dRdForwardBridgeFramePacket_t )", "sizeof( t3dRdForwardBridgeShadowPacket_t )"))),
        ("Dictionary and Callable are absent from the render callback path", "parsePacket" not in bridge_cpp and "forwardPassCallback" not in bridge_cpp and "Callable m_callback" not in bridge_h),
        ("frame and resource conversion are generation gated", "m_nCachedFrameGeneration" in bridge_cpp and "m_nCachedResourceGeneration" in bridge_cpp),
        ("main and shadow callback contexts are separate", "m_mainCallbackContext" in bridge_h and "m_shadowCallbackContext" in bridge_h and "prepareShadow" in bridge_cpp),
        ("SUBMIT validates only serial/context/draw-list state", "nPreparedSerial != m_nCachedPassSerial" in bridge_cpp and "m_pPreparedContext->m_nDrawList = nDrawList" in bridge_cpp),
        ("hot coordinator dispatch uses one atomic mask and ctz iteration", "m_nCoordinatorMask" in bridge_h and "t3dRdForwardCtz64" in bridge_cpp and "mask &= mask - 1u" in bridge_cpp),
        ("detach synchronizes before clearing coordinator pointers", bridge_cpp.find("m_nCoordinatorMask.fetch_and") < bridge_cpp.find("Terrain3DRdContext::forceSync();", bridge_cpp.find("detachCoordinator")) < bridge_cpp.find("compare_exchange_strong", bridge_cpp.find("detachCoordinator"))),
        ("coordinator lifecycle returns explicit history flags", "const t3dRdForwardPassContext_t &rContext" in coordinator_h and "bool &rHistoryValid, bool &rViewportReconfigured" in coordinator_h and "prepared_context = rContext" not in coordinator_cpp),
        ("clipmap and instance PREPARE accept explicit lifecycle flags", all(token in clipmap_h and token in instance_h for token in ("bTemporalHistoryValid", "bViewportReconfigured"))),
        ("forward lighting caches borrowed-resource and frame generations", "m_nBorrowedResourceGeneration" in lighting_h and "m_nFrameGeneration" in lighting_h and "resources_changed" in lighting_cpp),
        ("ocean keeps PREPARE/SUBMIT separation", "T3D_RD_FORWARD_PHASE_PREPARE" in ocean_cpp and "T3D_RD_FORWARD_PHASE_SUBMIT" in ocean_cpp),
        ("active native build selects exact 4.7.2 API", "extension_api-4.7.2.json" in sconstruct and api_path.is_file()),
        ("managed project selects Godot.NET.Sdk 4.7.2", "Godot.NET.Sdk/4.7.2" in csproj),
        ("legacy embedded engine patch is removed", not any((REPO / "engine_patch").glob("*.patch"))),
        ("external engine contract documents ABI 10 failure behavior", all(token in external_readme for token in ("4.7.2-stable", "ABI version: `10`", "472", "120", "48", "200", "rejects"))),
    ]

    if api_path.is_file():
        try:
            api = json.loads(api_path.read_text(encoding="utf-8"))
            header = api.get("header", {})
            checks.append(("extension API JSON reports exact Godot 4.7.2", header.get("version_major") == 4 and header.get("version_minor") == 7 and header.get("version_patch") == 2))
        except (OSError, json.JSONDecodeError):
            checks.append(("extension API JSON reports exact Godot 4.7.2", False))

    external_source = os.environ.get("RD_GODOT_SOURCE")
    if external_source:
        engine_root = Path(external_source)
        abi_h = engine_root / "servers" / "rendering" / "renderer_rd" / "forward_clustered" / "terrain3d_forward_plus_bridge_abi.h"
        bridge_engine_h = engine_root / "servers" / "rendering" / "renderer_rd" / "forward_clustered" / "terrain3d_forward_plus_bridge.h"
        version_py = engine_root / "version.py"
        engine_contract = "\n".join(path.read_text(encoding="utf-8", errors="ignore") for path in (abi_h, bridge_engine_h, version_py) if path.is_file())
        checks.extend([
            ("external Godot source exposes ABI 10", "T3D_FORWARD_BRIDGE_ABI UINT32_C(10)" in engine_contract),
            ("external Godot source exposes the six singleton methods", all(method in engine_contract for method in required_methods)),
            ("external Godot source targets 4.7.2 stable", all(token in engine_contract for token in ('major = 4', 'minor = 7', 'patch = 2', 'status = "stable"'))),
        ])

    failed = [description for description, passed in checks if not passed]
    if failed:
        for description in failed:
            print(f"FAIL {label} ABI 10 contract: {description}")
        print(f"{label} ABI 10 contract: {len(checks) - len(failed)}/{len(checks)} passed")
        return False
    print(f"OK {label} ABI 10 contract: {len(checks)}/{len(checks)} passed")
    return True


# ABI 10 consolidates the former F1-F8 Dictionary/embedded-patch lanes into
# one native POD contract. Keep the batch entry points so existing workflows
# retain their coverage without retaining obsolete pre-ABI-10 assumptions.
def rd_batch_f1_forward_bridge_check() -> bool:
    return rd_abi10_forward_bridge_check("Batch F1")


def rd_batch_f2_native_depth_opaque_check() -> bool:
    return rd_abi10_forward_bridge_check("Batch F2")


def rd_batch_f3_native_shadow_check() -> bool:
    return rd_abi10_forward_bridge_check("Batch F3")


def rd_batch_f4_native_lighting_check() -> bool:
    return rd_abi10_forward_bridge_check("Batch F4")


def rd_batch_f5_motion_check() -> bool:
    return rd_abi10_forward_bridge_check("Batch F5")


def rd_batch_f6_scene_integration_check() -> bool:
    return rd_abi10_forward_bridge_check("Batch F6")


def rd_batch_f7_multiview_lifecycle_check() -> bool:
    return rd_abi10_forward_bridge_check("Batch F7")


def rd_batch_f8_consolidation_check() -> bool:
    return rd_abi10_forward_bridge_check("Batch F8")


def rd_batch_g_editor_integration_check() -> bool:
    rd = REPO / "src" / "rendering" / "rd"
    coord_h = (rd / "terrain_3d_render_coordinator.h").read_text(encoding="utf-8", errors="ignore")
    coord = (rd / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    inst_h = (rd / "terrain_3d_rd_instance_renderer.h").read_text(encoding="utf-8", errors="ignore")
    inst = (rd / "terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8", errors="ignore")
    assets_h = (REPO / "src" / "assets" / "terrain_3d_assets.h").read_text(encoding="utf-8", errors="ignore")
    assets = (REPO / "src" / "assets" / "terrain_3d_assets.cpp").read_text(encoding="utf-8", errors="ignore")
    terrain_h = (REPO / "src" / "terrain_3d.h").read_text(encoding="utf-8", errors="ignore")
    integration = (REPO / "src" / "rendering" / "terrain_3d_render_integration.cpp").read_text(encoding="utf-8", errors="ignore")
    bindings = (REPO / "src" / "core" / "terrain_3d_bindings.cpp").read_text(encoding="utf-8", errors="ignore")
    types = (rd / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    near = (REPO / "project" / "addons" / "terrain_3d" / "rd" / "terrain_clipmap_near.glsl").read_text(encoding="utf-8", errors="ignore")
    profiler = (REPO / "project" / "addons" / "terrain_3d" / "src" / "runtime_profiler_window.gd").read_text(encoding="utf-8", errors="ignore")
    metadata = (REPO / "project" / "addons" / "terrain_3d" / "setting_metadata.json").read_text(encoding="utf-8", errors="ignore")
    region_workbench = (REPO / "project" / "addons" / "terrain_3d" / "tools" / "region_workbench.gd").read_text(encoding="utf-8", errors="ignore")
    sln = (REPO / "Terrain3D.sln").read_text(encoding="utf-8-sig", errors="ignore")
    managed_sln = (REPO / "project" / "Terrain3D.sln").read_text(encoding="utf-8-sig", errors="ignore")
    csproj = (REPO / "project" / "Terrain3D.csproj").read_text(encoding="utf-8", errors="ignore")
    sconstruct = (REPO / "SConstruct").read_text(encoding="utf-8", errors="ignore")
    gdextension = (REPO / "project" / "addons" / "terrain_3d" / "terrain.gdextension").read_text(encoding="utf-8", errors="ignore")
    generator = (REPO / "tools" / "generate_msvc_project.py").read_text(encoding="utf-8", errors="ignore")
    project_godot = (REPO / "project" / "project.godot").read_text(encoding="utf-8", errors="ignore")
    ledger = (REPO / "docs" / "rd_contract_ledger.tsv").read_text(encoding="utf-8", errors="ignore")
    batches = BATCH_FILE.read_text(encoding="utf-8", errors="ignore")
    g_doc_path = REPO / "docs" / "rd_batch_g_editor_integration.md"
    g_doc = g_doc_path.read_text(encoding="utf-8", errors="ignore") if g_doc_path.is_file() else ""

    stale_tokens = (
        "T3D_RENDER_BACKEND", "T3D_RD_NATIVE_PROXY", "render_backend", "rd_native_proxy_mode",
        "getRenderBackend", "getRdNativeProxyMode", "setRenderBackend", "setRdNativeProxyMode",
    )
    production_text = "\n".join(
        path.read_text(encoding="utf-8", errors="ignore")
        for root in (REPO / "src", REPO / "project" / "addons" / "terrain_3d")
        for path in root.rglob("*")
        if path.is_file() and path.suffix.lower() in {".cpp", ".h", ".gd", ".cs", ".json"}
    )
    if git_checkout_available():
        ref_paths = set(capture(["git", "ls-tree", "-r", "--name-only", REFERENCE_TAG]).splitlines())
        current_paths = set(capture(["git", "ls-files"]).splitlines())
        ref_cs = {x for x in ref_paths if x.endswith(".cs")}
        current_cs = {x for x in current_paths if x.endswith(".cs")}
        ref_gd = {x for x in ref_paths if x.endswith(".gd")}
        current_gd = {x for x in current_paths if x.endswith(".gd")}
    else:
        current_cs = {p.relative_to(REPO).as_posix() for p in REPO.rglob("*.cs") if ".rd_tools" not in p.parts}
        current_gd = {p.relative_to(REPO).as_posix() for p in REPO.rglob("*.gd") if ".rd_tools" not in p.parts}
        ref_cs = current_cs
        ref_gd = current_gd
        print("SKIP Batch G reference file parity baseline: source archive has no frozen Git tag")

    rs_preview_tokens = (
        "scenario_create", "viewport_create", "camera_create", "directional_light_create",
        "instance_create2", "viewport_get_texture", "texture_2d_get",
    )
    checks = [
        ("Batch G documentation exists", bool(g_doc) and "RenderingDevice editor and preview convergence" in g_doc),
        ("Batch G is marked completed in batch ledger", any(line.startswith("G\t16-17\tcompleted\t23-24,27-29,37-38\t") for line in batches.splitlines())),
        ("Sections 23,24,27,28,29 have implementation evidence", all(f"{section}\t" in ledger and f"{section}\t" in ledger for section in (23, 24, 27, 28, 29)) and all(any(line.startswith(f"{section}\t") and "\tcomplete\t" in line for line in ledger.splitlines()) for section in (23, 24, 27, 28, 29))),
        ("editor overlay state is published through RD frame snapshots", "buildEditorOverlayState" in coord and "ensureEditorBrushTexture" in coord and "t3dRdEditorOverlayConfig_t" in types),
        ("brush and reticle overlays are consumed by the terrain RD shader", "EditorOverlayBuffer" in near and "editor_brush_texture" in near and "apply_editor_decals" in near and "editor_reticle" in near),
        ("region/vertex/instancer/debug/navigation overlays remain in RD shader", all(x in near for x in ("T3D_DEBUG_REGION_GRID", "T3D_DEBUG_VERTEX_GRID", "T3D_DEBUG_INSTANCER_GRID", "T3D_DEBUG_CONTOURS", "T3D_DEBUG_NAVIGATION", "debug_visualization"))),
        ("RD presentation proxy machinery is absent", "enforceRdPresentation" not in production_text and "setRdPresentation" not in production_text),
        ("stale backend/proxy API symbols are removed from native and scripting surfaces", all(token not in production_text for token in stale_tokens)),
        ("runtime profiler reports the single RD Forward+ production route", "RenderingDevice / Forward+" in profiler and "Rendering backend" not in profiler and "Native proxy mode" not in profiler),
        ("setting metadata and region workbench no longer expose stale backend/proxy properties", all(token not in metadata + region_workbench for token in ("Terrain3D.render_backend", "Terrain3D.rd_native_proxy_mode"))),
        ("asset thumbnail path delegates to Terrain3D RD preview", "createRdMeshPreview" in assets and "ImageTexture::create_from_image" in assets),
        ("asset thumbnail code owns no RenderingServer preview objects", all(token not in assets + assets_h for token in rs_preview_tokens)),
        ("RD preview owns explicit color/depth textures and framebuffer", "ensurePreviewTargets" in inst and "TEXTURE_USAGE_COLOR_ATTACHMENT_BIT" in inst and "TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT" in inst and "ensureViewState" in inst),
        ("RD preview reuses the instance mesh/material cache and includes disabled editor assets", "m_meshCache.upload( pMeshes )" in inst and "uploadMaterials()" in inst and "getMeshHandle( nAssetId, 0 )" in inst and "m_nPreviewMeshGeneration, &error, true" in coord),
        ("RD preview supplies camera/projection state and performs image readback", "create_orthogonal_aspect" in inst and "texture_get_data( m_hPreviewColorTexture" in inst and "Image::create_from_data" in inst and "flip_y" in inst),
        ("RD preview resources are deterministically freed", "freePreviewTargets" in inst_h and "freePreviewTargets();" in inst and "m_previewRenderer.shutdown()" in coord),
        ("preview crosses to render thread only through the permitted synchronization gateway", "Terrain3DRdContext::enqueueOnRenderThread" in coord and "Terrain3DRdContext::forceSync" in coord and "renderPreviewOnRenderThread" in coord_h and all(token not in coord[coord.find("Ref<Image> Terrain3DRenderCoordinator::createMeshPreview"):coord.find("void Terrain3DRenderCoordinator::render(")] for token in rs_preview_tokens)),
        ("Terrain3D internal preview routing is not a stale public backend API", "createRdMeshPreview" in terrain_h and "Terrain3D::createRdMeshPreview" in integration and "createRdMeshPreview" not in bindings),
        ("C# reference file parity is preserved", ref_cs == current_cs and len(current_cs) > 0),
        ("GDScript reference file parity is preserved", ref_gd == current_gd and len(current_gd) > 0),
        ("root solution includes the managed Terrain3D project", "Terrain3D.Managed" in sln and "project\\Terrain3D.csproj" in sln and "Terrain3D.csproj" in generator),
        ("managed project targets exact Godot 4.7.2 SDK", 'Godot.NET.Sdk/4.7.2' in csproj and "Terrain3D.csproj" in managed_sln),
        ("native build uses exact Godot 4.7.2 extension API", "extension_api-4.7.2.json" in sconstruct and 'compatibility_minimum = "4.7"' in gdextension),
        ("native build source list is recursive and Windows/Linux library metadata exists", 'glob("src/**/*.cpp", recursive=True)' in sconstruct and "windows.debug.x86_64" in gdextension and "linux.debug.x86_64" in gdextension),
        ("editor plugin remains registered in project metadata", '[editor_plugins]' in project_godot and 'res://addons/terrain_3d/plugin.cfg' in project_godot),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        for label in failed:
            print(f"FAIL Batch G editor/build integration contract: {label}")
        print(f"Batch G editor/build integration contract: {len(checks)-len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch G editor/build integration contract: {len(checks)}/{len(checks)} passed")
    return True


def rd_batch_h_final_check() -> bool:
    rd = REPO / "src" / "rendering" / "rd"
    context_h = (rd / "terrain_3d_rd_context.h").read_text(encoding="utf-8", errors="ignore")
    context = (rd / "terrain_3d_rd_context.cpp").read_text(encoding="utf-8", errors="ignore")
    coord = (rd / "terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8", errors="ignore")
    types = (rd / "terrain_3d_rd_types.h").read_text(encoding="utf-8", errors="ignore")
    ocean_h_path = rd / "terrain_3d_rd_ocean_renderer.h"
    ocean_cpp_path = rd / "terrain_3d_rd_ocean_renderer.cpp"
    ocean_shader_path = REPO / "project" / "addons" / "terrain_3d" / "rd" / "terrain_ocean.glsl"
    ocean_h = ocean_h_path.read_text(encoding="utf-8", errors="ignore") if ocean_h_path.is_file() else ""
    ocean_cpp = ocean_cpp_path.read_text(encoding="utf-8", errors="ignore") if ocean_cpp_path.is_file() else ""
    ocean_shader = ocean_shader_path.read_text(encoding="utf-8", errors="ignore") if ocean_shader_path.is_file() else ""
    mesher = (REPO / "src" / "rendering" / "terrain_3d_mesher.cpp").read_text(encoding="utf-8", errors="ignore")
    material = (REPO / "src" / "rendering" / "terrain_3d_material.cpp").read_text(encoding="utf-8", errors="ignore")
    collision = (REPO / "src" / "physics" / "terrain_3d_collision.cpp").read_text(encoding="utf-8", errors="ignore")
    collision_builder = (REPO / "src" / "physics" / "terrain_3d_collision_surface_builder.cpp").read_text(encoding="utf-8", errors="ignore")
    data = (REPO / "src" / "data" / "terrain_3d_data.cpp").read_text(encoding="utf-8", errors="ignore")
    bake = (REPO / "src" / "data" / "terrain_3d_bake_integration.cpp").read_text(encoding="utf-8", errors="ignore")
    unit = (REPO / "src" / "core" / "unit_testing.cpp").read_text(encoding="utf-8", errors="ignore")
    bridge_h = (rd / "terrain_3d_rd_forward_plus_bridge.h").read_text(encoding="utf-8", errors="ignore")
    external_engine = (REPO / "engine_patch" / "README.md").read_text(encoding="utf-8", errors="ignore")
    ledger = (REPO / "docs" / "rd_contract_ledger.tsv").read_text(encoding="utf-8", errors="ignore")
    batches = BATCH_FILE.read_text(encoding="utf-8", errors="ignore")
    h_doc_path = REPO / "docs" / "rd_batch_h_final_validation.md"
    h_doc = h_doc_path.read_text(encoding="utf-8", errors="ignore") if h_doc_path.is_file() else ""
    parity_path = WORK_DIR / "final_parity_matrix.tsv"
    parity_rows = read_tsv(parity_path) if parity_path.is_file() else []
    evidence_path = WORK_DIR / "final_validation_evidence.tsv"
    allowlist = read_final_allowlist()
    scan = scan_symbols()

    required_diag = (
        "gpu_frame_id", "snapshot_generation", "bytes_uploaded_frame", "buffer_updates_frame",
        "pipeline_creates", "uniform_set_creates", "terrain_patch_count", "visible_terrain_patches",
        "static_instance_count", "visible_instances", "hlod_count", "visible_hlods",
        "indirect_command_count", "compute_dispatch_count", "steady_state_allocation_violations",
        "last_successful_pass", "failure_state",
    )
    h_sections = (25, 26, 31, 32, 33, 34, 35, 39, 40, 41, 42, 43, 44, 45, 46, 47)
    rs_files = scan["RenderingServer"]["files"]
    assert isinstance(rs_files, dict)
    legacy_zero = all(int(scan[name]["count"]) == 0 for name in (
        "mesh_create", "instance_create", "instance_set_base", "material_set_param", "Hybrid", "TODO", "FIXME",
    ))
    checks = [
        ("Batch H documentation exists", bool(h_doc) and "Final clean-break convergence" in h_doc),
        ("Batch H is marked completed", any(line.startswith("H\t18-19\tcompleted\t25-26,31-35,39-47\t") for line in batches.splitlines())),
        ("all Batch H contract sections have completion evidence", all(any(line.startswith(f"{section}\t") and "\tcomplete\t" in line for line in ledger.splitlines()) for section in h_sections)),
        ("final validation evidence ledger exists", evidence_path.is_file() and len(read_tsv(evidence_path)) >= 9),
        ("final parity matrix covers every frozen reference row", len(parity_rows) == 155 and all(row.get("rd_status") in {"Implemented", "Preserved"} for row in parity_rows)),
        ("final symbol allowlist is explicit and reasoned", len(allowlist) >= 10 and all(limit > 0 and bool(reason) for limit, reason in allowlist.values())),
        ("RenderingServer is confined to the sole RD gateway", set(rs_files) == {"src/rendering/rd/terrain_3d_rd_context.cpp"}),
        ("runtime legacy RS creation/material/fallback symbols are zero", legacy_zero),
        ("RD context exposes global/local device and render-thread gateway", all(x in context_h for x in ("getGlobalDevice", "createLocalDevice", "isOnRenderThread", "enqueueOnRenderThread", "forceSync")) and "#include <godot_cpp/classes/rendering_server.hpp>" in context),
        ("direct RenderingServer include exists only in RD context", set(rs_files) == {"src/rendering/rd/terrain_3d_rd_context.cpp"}),
        ("ocean has dedicated RD renderer and raw shader", bool(ocean_h) and bool(ocean_cpp) and bool(ocean_shader) and "Terrain3DRdOceanRenderer" in ocean_h and "renderForwardPass" in ocean_cpp),
        ("Forward+ ABI 10 includes split shadow and transparent submission", "s_nBridgeAbiVersion = s_nT3dForwardBridgeAbi" in bridge_h and "SPLIT_SHADOW_PACKET" in bridge_h and "T3D_RD_FORWARD_PASS_TRANSPARENT" in types),
        ("legacy mesher contains no RS or MultiMesh renderer", "RenderingServer" not in mesher and "MultiMesh" not in mesher and "mesh_create" not in mesher and "instance_create" not in mesher),
        ("native Terrain3D material publication is removed", "material_create" not in material and "material_set_param" not in material and "m_hMaterial" not in material),
        ("material defaults use authoring reflection without RS", "Ref<ShaderMaterial> near_defaults" in material and "get_shader_parameter" in material and "RenderingServer" not in material),
        ("collision remains on the shared collision surface builder", "m_collisionSurfaceBuilder.build" in collision and "Terrain3DCollisionSurfaceBuilder::build" in collision_builder),
        ("runtime baking reuses collision and navigation data paths", "Terrain3DCollisionSurfaceBuilder collision_builder" in data and "encodeNavigationMask" in data and "getNavigationMaskView" in bake),
        ("navigation paint/get bindings remain present", "set_control_navigation" in data and "get_control_navigation" in data),
        ("triple-buffer immutable publication contract remains", "s_nRdFrameSnapshotSlotCount = 3u" in types and "m_nPublishedSnapshotIndex" in coord and "m_nPublishedSnapshotGeneration" in coord),
        ("CPU reference culling and indirect validation remain", "Terrain3DRdCpuCuller::build" in unit and "Terrain3DRdCpuCuller::validate" in unit and "isCommandRangeValid" in unit),
        ("RD context failure ownership regression is tested", "FAILURE_UNTRACKED_RESOURCE" in unit and "untracked_free_attempts" in unit and "rejects untracked RIDs" in unit),
        ("diagnostic schema covers H memory/performance/failure counters", all(f'result["{key}"]' in context for key in required_diag)),
        ("steady-state allocation instrumentation remains explicit", "markWarmupComplete" in context and "m_nSteadyStateAllocationViolations" in context),
        ("no production compositor fallback is restored", "TERRAIN3D_RD_DEBUG_COMPOSITOR" in coord and "m_bCompositorDebugEnabled" in coord and "setRdPresentation" not in coord),
        ("external Godot 4.7.2 ABI 10 dependency is explicit", "4.7.2-stable" in external_engine and "ABI version: `10`" in external_engine and "Terrain3DForwardPlusBridge" in external_engine),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        for label in failed:
            print(f"FAIL Batch H final contract: {label}")
        print(f"Batch H final contract: {len(checks)-len(failed)}/{len(checks)} passed")
        return False
    print(f"OK Batch H final contract: {len(checks)}/{len(checks)} passed")
    return True

def iter_scan_files() -> list[Path]:
    files: list[Path] = []
    for root in SCAN_ROOTS:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.is_file() and path.suffix.lower() in TEXT_SUFFIXES:
                files.append(path)
    return sorted(files)


def scan_symbols() -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {name: {"count": 0, "files": {}} for name in SYMBOLS}
    for path in iter_scan_files():
        text = path.read_text(encoding="utf-8", errors="ignore")
        rel = path.relative_to(REPO).as_posix()
        for name, pattern in SYMBOLS.items():
            count = len(pattern.findall(text))
            if count:
                result[name]["count"] = int(result[name]["count"]) + count
                files = result[name]["files"]
                assert isinstance(files, dict)
                files[rel] = count
    return result


def read_symbol_baseline() -> dict[str, int]:
    if not SYMBOL_BASELINE_FILE.exists():
        return {}
    values: dict[str, int] = {}
    with SYMBOL_BASELINE_FILE.open("r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle, delimiter="\t"):
            values[row["symbol"]] = int(row["count"])
    return values


def write_symbol_baseline(scan: dict[str, dict[str, object]]) -> None:
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    with SYMBOL_BASELINE_FILE.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("symbol\tcount\tfiles\n")
        for name in SYMBOLS:
            files = scan[name]["files"]
            assert isinstance(files, dict)
            handle.write(f"{name}\t{scan[name]['count']}\t{len(files)}\n")


def read_final_allowlist() -> dict[tuple[str, str], tuple[int, str]]:
    allowed: dict[tuple[str, str], tuple[int, str]] = {}
    if not FINAL_ALLOWLIST_FILE.exists():
        return allowed
    with FINAL_ALLOWLIST_FILE.open("r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle, delimiter="\t"):
            symbol = row.get("symbol", "").strip()
            path = row.get("path", "").strip()
            if not symbol or not path:
                continue
            allowed[(symbol, path)] = (int(row["max_count"]), row.get("reason", "").strip())
    return allowed


def audit_symbols(*, details: bool = False, strict_growth: bool = True, final: bool = False) -> bool:
    scan = scan_symbols()
    baseline = read_symbol_baseline()
    allowed = read_final_allowlist() if final else {}
    ok = True
    print("symbol\tcurrent\tbaseline\tdelta\tfiles")
    for name in SYMBOLS:
        current = int(scan[name]["count"])
        base = baseline.get(name, current)
        delta = current - base
        files = scan[name]["files"]
        assert isinstance(files, dict)
        print(f"{name}\t{current}\t{base}\t{delta:+d}\t{len(files)}")
        if strict_growth and baseline and delta > 0:
            ok = False
        if final:
            for path, count in files.items():
                limit_reason = allowed.get((name, path))
                if limit_reason is None or count > limit_reason[0] or not limit_reason[1]:
                    ok = False
                    print(f"  UNJUSTIFIED {name} {count} {path}")
        elif details and files:
            for path, count in sorted(files.items(), key=lambda item: (-item[1], item[0])):
                print(f"  {count:4d}  {path}")
    if final and not ok:
        print(f"FAIL final legacy audit; justify only genuine gateway/reference occurrences in {FINAL_ALLOWLIST_FILE.relative_to(REPO)}")
    elif not ok:
        print("FAIL legacy symbol counts increased relative to the baseline")
    return ok


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def next_batch() -> dict[str, str] | None:
    for row in read_tsv(BATCH_FILE):
        if row["state"].lower() not in {"complete", "completed"}:
            return row
    return None


def print_start() -> int:
    row = next_batch()
    print(f"branch={current_branch()}")
    print(f"commit={current_commit()}")
    if row is None:
        print("batch=none")
        print("action=run tools\\rd.cmd final")
        return 0
    print(f"batch={row['batch']} sections={row['contract_sections']} state={row['state']}")
    print(f"goal={row['goal']}")
    print(f"paths={row['primary_paths']}")
    print(f"gate={row['completion_gate']}")
    print(f"context=tools\\rd.cmd context {row['batch']}")
    print(f"contract=tools\\rd.cmd contract {row['batch']}  (only if needed)")
    return 0


def print_next() -> int:
    row = next_batch()
    if row is None:
        print("No incomplete batch remains. Run final validation before claiming completion.")
        return 0
    for key in ("batch", "responses", "state", "contract_sections", "goal", "primary_paths", "completion_gate"):
        print(f"{key}={row[key]}")
    return 0


def print_context(batch_name: str) -> int:
    batch_name = batch_name.upper()
    batches = {row["batch"].upper(): row for row in read_tsv(BATCH_FILE)}
    row = batches.get(batch_name)
    if row is None:
        print(f"Unknown batch: {batch_name}", file=sys.stderr)
        return 2
    print("[batch]")
    for key in ("batch", "responses", "state", "contract_sections", "goal", "completion_gate"):
        print(f"{key}={row[key]}")
    print("\n[source map]")
    for source in read_tsv(SOURCE_MAP_FILE):
        names = {part.strip().upper() for part in source["batches"].split(",")}
        if batch_name in names:
            print(f"{source['subsystem']}\t{source['role']}\t{source['primary_files']}")
    return 0


def parse_section_spec(spec: str) -> set[int]:
    sections: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            start_text, end_text = part.split("-", 1)
            sections.update(range(int(start_text), int(end_text) + 1))
        else:
            sections.add(int(part))
    return sections


def print_contract_for_batch(batch_name: str) -> int:
    batch_name = batch_name.upper()
    batches = {row["batch"].upper(): row for row in read_tsv(BATCH_FILE)}
    row = batches.get(batch_name)
    if row is None:
        print(f"Unknown batch: {batch_name}", file=sys.stderr)
        return 2
    wanted = parse_section_spec(row["contract_sections"])
    lines = CONTRACT_FILE.read_text(encoding="utf-8").splitlines()
    heading = re.compile(r"^#{1,6}\s+(\d+)\.\s+")
    current: int | None = None
    emitted = False
    for line in lines:
        match = heading.match(line)
        if match:
            current = int(match.group(1))
        if current in wanted:
            print(line)
            emitted = True
    return 0 if emitted else 1


def executable_candidates_from_dir(root: Path) -> list[Path]:
    if not root.is_dir():
        return []
    candidates = list(root.glob("*Godot*4.7.2*.exe")) + list(root.glob("godot*.exe")) + list(root.glob("Godot*.exe"))
    candidates = sorted(set(candidates), key=lambda path: ("_console" not in path.stem.lower(), len(path.name), path.name.lower()))
    return candidates


def discover_godot() -> str | None:
    explicit = os.environ.get("RD_GODOT") or os.environ.get("GODOT_BIN")
    roots = [os.environ.get("RD_GODOT_ROOT")]
    candidates: list[Path] = []
    if explicit:
        path = Path(explicit)
        if path.is_file():
            candidates.append(path)
        elif path.is_dir():
            candidates.extend(executable_candidates_from_dir(path))
    if os.name == "nt":
        if roots[0]:
            candidates.extend(executable_candidates_from_dir(Path(roots[0])))
        candidates.extend(executable_candidates_from_dir(DEFAULT_WINDOWS_GODOT_ROOT))
    for name in ("godot-mono", "godot4", "godot"):
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    for base in (REPO, REPO.parent, Path("/mnt/data")):
        if base.is_dir():
            candidates.extend(base.glob("**/Godot_v4.7.2-stable_linux.x86_64"))
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return None


def discover_scons() -> str | None:
    explicit = os.environ.get("SCONS")
    candidates = [
        explicit,
        shutil.which("scons"),
        str(TOOLS_DIR / "venv" / "Scripts" / "scons.exe"),
        str(TOOLS_DIR / "venv" / "bin" / "scons"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def discover_vsdevcmd() -> str | None:
    if os.name != "nt":
        return None
    explicit = os.environ.get("VSDEVCMD")
    if explicit and Path(explicit).is_file():
        return explicit
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.is_file():
        try:
            install = subprocess.check_output(
                [str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                text=True,
                stderr=subprocess.DEVNULL,
            ).strip()
            if install:
                candidate = Path(install) / "Common7" / "Tools" / "VsDevCmd.bat"
                if candidate.is_file():
                    return str(candidate)
        except subprocess.CalledProcessError:
            pass
    return None


def bootstrap() -> int:
    TOOLS_DIR.mkdir(parents=True, exist_ok=True)
    scons = discover_scons()
    if scons is None:
        venv = TOOLS_DIR / "venv"
        print("setup=scons-local-venv")
        try:
            subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)
            pip = venv / ("Scripts/pip.exe" if os.name == "nt" else "bin/pip")
            subprocess.run([str(pip), "install", "--disable-pip-version-check", "-q", "scons"], check=True)
        except (subprocess.CalledProcessError, OSError) as exc:
            print(f"WARN scons install failed: {exc}")
        scons = discover_scons()
    godot = discover_godot()
    print(f"scons={scons or 'MISSING'}")
    print(f"godot={godot or 'MISSING'}")
    print(f"dotnet={shutil.which('dotnet') or 'MISSING'}")
    if os.name == "nt":
        print(f"msvc={'PATH' if shutil.which('cl') else (discover_vsdevcmd() or 'MISSING')}")
    if godot:
        try:
            version = capture([godot, "--version"])
            print(f"godot_version={version}")
            if not version.startswith("4.7.2"):
                print("FAIL Godot is not 4.7.2")
                return 1
        except (subprocess.CalledProcessError, OSError) as exc:
            print(f"FAIL Godot version probe: {exc}")
            return 1
    return 0 if scons and godot else 1


def build_extension(target: str = "editor") -> bool:
    scons = discover_scons()
    if scons is None:
        print("FAIL SCons missing; run tools\\rd.cmd bootstrap")
        return False
    platform = "windows" if os.name == "nt" else "linux"
    jobs = max(1, min(os.cpu_count() or 1, 8))
    args = [scons, f"platform={platform}", f"target={target}", "arch=x86_64", f"-j{jobs}"]
    env = os.environ.copy()
    env.setdefault("SCONS_CACHE", str(REPO / ".scons-cache"))
    if os.name == "nt" and shutil.which("cl") is None:
        vsdev = discover_vsdevcmd()
        if vsdev:
            command_line = subprocess.list2cmdline(args)
            # cmd.exe does not use the MSVC argv parser. Passing the command as
            # a list makes subprocess.list2cmdline() escape the embedded quotes
            # as \"; cmd treats those backslashes literally and cannot find the
            # developer-environment batch file. A raw command line preserves the
            # quoting expected by cmd while keeping the SCons arguments quoted.
            args = f'cmd.exe /d /c call "{vsdev}" -arch=x64 -host_arch=x64 >nul && {command_line}'
    return run_quiet(args, label=f"build_{target}", env=env, timeout=1800)


def godot_command_base() -> tuple[str | None, list[str]]:
    godot = discover_godot()
    if godot is None:
        return None, []
    return godot, [godot, "--path", str(REPO / "project")]


def godot_log_has_hard_errors(label: str) -> bool:
    path = log_path(label)
    if not path.is_file():
        return True
    text = path.read_text(encoding="utf-8", errors="ignore")
    hard_markers = (
        "SCRIPT ERROR:",
        "GDExtension dynamic library not found",
        "Error loading extension:",
        "Can't open dynamic library",
    )
    return any(marker in text for marker in hard_markers)


def godot_headless_import() -> bool:
    godot, base = godot_command_base()
    if godot is None:
        print("FAIL Godot 4.7.2 missing; set RD_GODOT_ROOT or RD_GODOT")
        return False
    command = base + ["--headless", "--render-thread", "safe", "--import", "--log-file", str(log_path("godot_import_engine"))]
    if not run_quiet(command, label="godot_import", timeout=300):
        return False
    if godot_log_has_hard_errors("godot_import"):
        print(f"FAIL godot_import: engine reported script/GDExtension load errors; log={log_path('godot_import').relative_to(REPO)}")
        return False
    print("OK Godot import log contains no script/GDExtension load errors")
    return True


def godot_editor_headless() -> bool:
    godot, base = godot_command_base()
    if godot is None:
        print("FAIL Godot 4.7.2 missing")
        return False
    command = base + ["--headless", "--render-thread", "safe", "--editor", "--quit-after", "5", "--log-file", str(log_path("godot_editor_engine"))]
    env = os.environ.copy()
    env["TERRAIN3D_RUN_RD_CPU_TESTS"] = "1"
    if not run_quiet(command, label="godot_editor_headless", env=env, timeout=300):
        return False
    if godot_log_has_hard_errors("godot_editor_headless"):
        print(f"FAIL godot_editor_headless: engine reported script/GDExtension load errors; log={log_path('godot_editor_headless').relative_to(REPO)}")
        return False
    print("OK Godot editor log contains no script/GDExtension load errors")
    return True


def godot_build_solutions() -> bool:
    project = REPO / "project" / "Terrain3D.csproj"
    if not project.exists():
        print("OK C# project absent; skipped")
        return True
    dotnet = shutil.which("dotnet")
    if dotnet is None:
        print("FAIL dotnet missing; install the .NET 8 SDK required by Godot.NET.Sdk/4.7.2")
        return False
    command = [dotnet, "build", str(project), "--nologo", "--verbosity", "minimal"]
    return run_quiet(command, label="dotnet_build", timeout=600)

def godot_recovery_smoke() -> bool:
    godot, base = godot_command_base()
    if godot is None:
        print("FAIL Godot 4.7.2 missing")
        return False
    command = base + ["--headless", "--recovery-mode", "--render-thread", "safe", "--editor", "--quit-after", "3", "--log-file", str(log_path("godot_recovery_engine"))]
    return run_quiet(command, label="godot_recovery", timeout=180)


def godot_gpu_smoke(driver: str) -> bool:
    driver = driver.lower()
    if driver not in {"vulkan", "d3d12"}:
        print("FAIL driver must be vulkan or d3d12")
        return False
    if os.name != "nt" and driver == "d3d12":
        print("FAIL d3d12 smoke is Windows-only")
        return False
    godot, base = godot_command_base()
    if godot is None:
        print("FAIL Godot 4.7.2 missing")
        return False
    command = base + [
        "--rendering-method", "forward_plus",
        "--rendering-driver", driver,
        "--render-thread", "safe",
        "--audio-driver", "Dummy",
        "--single-window",
        "--xr-mode", "off",
        "--windowed",
        "--resolution", "640x360",
        "--max-fps", "60",
        "--quit-after", "120",
        "--log-file", str(log_path(f"godot_gpu_{driver}_engine")),
    ]
    env = os.environ.copy()
    env["TERRAIN3D_RD_COMPARE_CULL"] = "1"
    ok = run_quiet(command, label=f"godot_gpu_{driver}", env=env, timeout=180)
    if not ok:
        return False
    validation_text = "\n".join(
        path.read_text(encoding="utf-8", errors="ignore")
        for path in (log_path(f"godot_gpu_{driver}"), log_path(f"godot_gpu_{driver}_engine"))
        if path.is_file()
    )
    marker = "[RD validation] GPU/CPU culling comparison passed"
    instance_marker = "[RD validation] Instance GPU/CPU culling parity passed"
    instance_production_marker = "[RD validation] Instance GPU cull production submission passed"
    instance_arena_marker = "[RD validation] Instance shared mesh arena submission passed"
    if marker not in validation_text:
        print(f"FAIL gpu_{driver}: RD GPU/CPU culling comparison did not execute")
        return False
    if instance_marker not in validation_text:
        print(f"FAIL gpu_{driver}: RD instance GPU/CPU culling parity did not execute")
        return False
    if instance_production_marker not in validation_text:
        print(f"FAIL gpu_{driver}: RD instance GPU cull production submission did not execute")
        return False
    if instance_arena_marker not in validation_text:
        print(f"FAIL gpu_{driver}: RD instance shared mesh arena submission did not execute")
        return False
    print(f"OK gpu_{driver}: RD GPU/CPU culling comparison + instance parity + production indirect + shared mesh arena")
    return True


def diagnose_godot() -> int:
    godot = discover_godot()
    print(f"godot={godot or 'MISSING'}")
    if godot is None:
        return 1
    try:
        print(f"version={capture([godot, '--version'])}")
    except (subprocess.CalledProcessError, OSError) as exc:
        print(f"FAIL version: {exc}")
        return 1
    results = [godot_recovery_smoke(), godot_headless_import()]
    if os.name == "nt":
        results.append(godot_gpu_smoke("vulkan"))
        results.append(godot_gpu_smoke("d3d12"))
    print(f"logs={LOG_DIR}")
    return 0 if all(results) else 1


def engine_source(bootstrap_source: bool = False) -> int:
    explicit = os.environ.get("RD_GODOT_SOURCE")
    path = Path(explicit) if explicit else DEFAULT_ENGINE_SOURCE
    if not path.exists() and bootstrap_source:
        TOOLS_DIR.mkdir(parents=True, exist_ok=True)
        ok = run_quiet(
            ["git", "clone", "--depth", "1", "--branch", "4.7.2-stable", "https://github.com/godotengine/godot.git", str(path)],
            label="godot_source_clone",
            cwd=REPO,
            timeout=1800,
        )
        if not ok:
            return 1
    if not path.exists():
        print(f"missing={path}")
        print("action=tools\\rd.cmd engine-source --bootstrap   (Batch F only)")
        return 1
    try:
        commit = capture(["git", "rev-parse", "HEAD"], cwd=path)
        tag = capture(["git", "describe", "--tags", "--exact-match"], cwd=path)
    except subprocess.CalledProcessError as exc:
        print(f"FAIL invalid Godot source checkout: {exc}")
        return 1
    print(f"godot_source={path}")
    print(f"tag={tag}")
    print(f"commit={commit}")
    if tag != "4.7.2-stable":
        print("FAIL Godot source is not exact 4.7.2-stable")
        return 1
    return 0



def prepare_fixture(verify: bool = False) -> int:
    if not FIXTURE_ARCHIVE.is_file():
        print(f"FAIL missing fixture archive={FIXTURE_ARCHIVE}")
        return 1
    if not FIXTURE_DIR.exists():
        FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
        try:
            with zipfile.ZipFile(FIXTURE_ARCHIVE, "r") as archive:
                archive.extractall(FIXTURE_DIR)
        except (OSError, zipfile.BadZipFile) as exc:
            print(f"FAIL fixture extract: {exc}")
            return 1
    files = sorted(path for path in FIXTURE_DIR.iterdir() if path.is_file())
    if len(files) != 354:
        print(f"FAIL fixture files={len(files)} expected=354")
        return 1
    if verify:
        expected: dict[str, tuple[int, str]] = {}
        lines = FIXTURE_MANIFEST.read_text(encoding="utf-8").splitlines()
        for line in lines:
            if not line or line.startswith("#") or line.startswith("bytes\t"):
                continue
            size_text, digest, name = line.split("\t", 2)
            expected[name] = (int(size_text), digest)
        if len(expected) != 354:
            print(f"FAIL fixture manifest entries={len(expected)} expected=354")
            return 1
        for path in files:
            item = expected.get(path.name)
            if item is None or path.stat().st_size != item[0]:
                print(f"FAIL fixture metadata={path.name}")
                return 1
            hasher = hashlib.sha256()
            with path.open("rb") as handle:
                while True:
                    chunk = handle.read(1024 * 1024)
                    if not chunk:
                        break
                    hasher.update(chunk)
            digest = hasher.hexdigest()
            if digest != item[1]:
                print(f"FAIL fixture sha256={path.name}")
                return 1
        print("OK validation fixture SHA-256 manifest")
    print(f"fixture={FIXTURE_DIR}")
    print("files=354 bytes=311464723")
    return 0


def git_fsck() -> bool:
    if not git_checkout_available():
        print("SKIP git fsck: clean source archive has no .git metadata")
        return True
    return run_quiet(["git", "fsck", "--full", "--strict"], label="git_fsck", timeout=300)


def source_archive_acceptance_check() -> bool:
    scripts = (
        "verify_async_preview_hardening_r01.py",
        "verify_editor_vt_transaction_r02.py",
        "verify_vt_authoring_invalidation_r03.py",
        "verify_repo_truth_r04.py",
        "verify_live_editor_vt_republish_p03.py",
        "verify_preview_resource_profile_p05.py",
        "verify_async_thumbnail_readback_p07.py",
        "verify_editor_vt_preview_ivt40.py",
        "verify_mesh_preview_failure_recovery.py",
        "verify_single_asset_preview.py",
        "verify_instance_vt_lifecycle_ivt00.py",
        "verify_vt_single_owner_p16.py",
        "verify_vt_tail_fallback_p18.py",
        "verify_vt_cooker_p19.py",
        "verify_terrain_micro_vt_p26.py",
        "verify_terrain_vt_package_composition.py",
        "verify_terrain_vt_runtime_consumer_recovery.py",
        "verify_instancer_global_vt_bind_p28.py",
        "verify_instance_vt_pipeline_recovery.py",
        "verify_instance_vt_remap_ivt30.py",
        "verify_instance_texturing_recovery.py",
        "verify_runtime_profile_shader_lifecycle.py",
        "verify_godot_shader_include_contract.py",
        "verify_shader_truth.py",
        "verify_wg_c_terrain_instance_shaders.py",
    )
    ok = True
    for script in scripts:
        path = REPO / "tools" / script
        ok = run_quiet([sys.executable, str(path)], label=f"archive_{path.stem}", timeout=120) and ok
    return ok


def validate_fast(static_only: bool = False) -> int:
    if not git_checkout_available():
        checks = [build_contract_check(), source_archive_acceptance_check()]
        if not static_only:
            checks.append(build_extension("editor"))
        return 0 if all(checks) else 1
    checks = [branch_check(), reference_check(), diff_check(), build_contract_check(), rd_shader_abi_check(), rd_batch_c_material_check(), rd_batch_d_instance_check(), rd_batch_e_hlod_check(), rd_batch_f1_forward_bridge_check(), rd_batch_f2_native_depth_opaque_check(), rd_batch_f3_native_shadow_check(), rd_batch_f4_native_lighting_check(), rd_batch_f5_motion_check(), rd_batch_f6_scene_integration_check(), rd_batch_f7_multiview_lifecycle_check(), rd_batch_f8_consolidation_check(), rd_batch_g_editor_integration_check(), rd_batch_h_final_check(), audit_symbols(strict_growth=True)]
    if not static_only:
        checks.append(build_extension("editor"))
    return 0 if all(checks) else 1


def validate_full() -> int:
    checks = [
        branch_check(),
        reference_check(),
        diff_check(),
        build_contract_check(),
        rd_shader_abi_check(),
        rd_batch_c_material_check(),
        rd_batch_d_instance_check(),
        rd_batch_e_hlod_check(),
        rd_batch_f1_forward_bridge_check(),
        rd_batch_f2_native_depth_opaque_check(),
        rd_batch_f3_native_shadow_check(),
        rd_batch_f4_native_lighting_check(),
        rd_batch_f5_motion_check(),
        rd_batch_f6_scene_integration_check(),
        rd_batch_f7_multiview_lifecycle_check(),
        rd_batch_f8_consolidation_check(),
        rd_batch_g_editor_integration_check(),
        rd_batch_h_final_check(),
        audit_symbols(strict_growth=True),
        build_extension("editor"),
        godot_headless_import(),
        godot_editor_headless(),
        godot_build_solutions(),
        git_fsck(),
    ]
    return 0 if all(checks) else 1


def validate_final() -> int:
    checks = [
        branch_check(),
        reference_check(),
        diff_check(),
        build_contract_check(),
        rd_shader_abi_check(),
        rd_batch_c_material_check(),
        rd_batch_d_instance_check(),
        rd_batch_e_hlod_check(),
        rd_batch_f1_forward_bridge_check(),
        rd_batch_f2_native_depth_opaque_check(),
        rd_batch_f3_native_shadow_check(),
        rd_batch_f4_native_lighting_check(),
        rd_batch_f5_motion_check(),
        rd_batch_f6_scene_integration_check(),
        rd_batch_f7_multiview_lifecycle_check(),
        rd_batch_f8_consolidation_check(),
        rd_batch_g_editor_integration_check(),
        rd_batch_h_final_check(),
        audit_symbols(strict_growth=True, final=True),
        build_extension("editor"),
        build_extension("template_release"),
        godot_headless_import(),
        godot_editor_headless(),
        godot_build_solutions(),
    ]
    if os.name == "nt":
        checks += [godot_gpu_smoke("vulkan"), godot_gpu_smoke("d3d12")]
    checks.append(git_fsck())
    return 0 if all(checks) else 1


def summary() -> int:
    status = capture(["git", "status", "--short"]) if git_checkout_available() else ""
    row = next_batch()
    print(f"branch={current_branch()}")
    print(f"commit={current_commit()}")
    print(f"worktree={'source-archive' if not git_checkout_available() else ('clean' if not status else 'dirty')}")
    if row:
        print(f"next_batch={row['batch']} state={row['state']} sections={row['contract_sections']}")
    else:
        print("next_batch=none")
    return 0


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("start", help="Print the minimum context needed to start/resume work")
    sub.add_parser("next", help="Print the next incomplete batch")
    context = sub.add_parser("context", help="Print compact source map for one batch")
    context.add_argument("batch")
    contract = sub.add_parser("contract", help="Print only contract sections assigned to one batch")
    contract.add_argument("batch")
    audit = sub.add_parser("audit", help="Print legacy/forbidden symbol counts")
    audit.add_argument("--details", action="store_true")
    audit.add_argument("--final", action="store_true")
    baseline = sub.add_parser("baseline", help="Write current symbol counts as non-growth baseline")
    baseline.add_argument("--force", action="store_true")
    sub.add_parser("bootstrap", help="Set up local SCons and locate Windows Godot/MSVC")
    build = sub.add_parser("build", help="Incrementally build the native extension")
    build.add_argument("--target", choices=["editor", "template_debug", "template_release"], default="editor")
    fast = sub.add_parser("fast", help="Static checks plus incremental editor build")
    fast.add_argument("--static-only", action="store_true")
    sub.add_parser("import", help="Headless project import using conservative automation flags")
    sub.add_parser("editor-smoke", help="Headless editor/plugin smoke")
    sub.add_parser("csharp", help="Build managed solution through Godot Mono")
    gpu = sub.add_parser("gpu-smoke", help="Short Forward+ RenderingDevice runtime smoke")
    gpu.add_argument("driver", choices=["vulkan", "d3d12"])
    sub.add_parser("diagnose-godot", help="Isolate startup/import/Vulkan/D3D12 failure lanes")
    engine = sub.add_parser("engine-source", help="Locate or shallow-clone exact external Godot 4.7.2 source for Batch F")
    engine.add_argument("--bootstrap", action="store_true")
    fixture = sub.add_parser("fixture", help="Extract the supplied 176-region validation fixture")
    fixture.add_argument("--verify", action="store_true")
    sub.add_parser("full", help="Batch completion gate without routine GPU launch")
    sub.add_parser("final", help="Strict final static/build/runtime/Git gate")
    sub.add_parser("summary", help="Print terse repository state")
    return parser


def main() -> int:
    args = make_parser().parse_args()
    if args.command == "start":
        return print_start()
    if args.command == "next":
        return print_next()
    if args.command == "context":
        return print_context(args.batch)
    if args.command == "contract":
        return print_contract_for_batch(args.batch)
    if args.command == "audit":
        return 0 if audit_symbols(details=args.details, strict_growth=False, final=args.final) else 1
    if args.command == "baseline":
        if SYMBOL_BASELINE_FILE.exists() and not args.force:
            print(f"Baseline exists: {SYMBOL_BASELINE_FILE.relative_to(REPO)}; use --force to replace", file=sys.stderr)
            return 2
        scan = scan_symbols()
        write_symbol_baseline(scan)
        print(f"Wrote {SYMBOL_BASELINE_FILE.relative_to(REPO)}")
        return 0
    if args.command == "bootstrap":
        return bootstrap()
    if args.command == "build":
        return 0 if build_extension(args.target) else 1
    if args.command == "fast":
        return validate_fast(static_only=args.static_only)
    if args.command == "import":
        return 0 if godot_headless_import() else 1
    if args.command == "editor-smoke":
        return 0 if godot_editor_headless() else 1
    if args.command == "csharp":
        return 0 if godot_build_solutions() else 1
    if args.command == "gpu-smoke":
        return 0 if godot_gpu_smoke(args.driver) else 1
    if args.command == "diagnose-godot":
        return diagnose_godot()
    if args.command == "engine-source":
        return engine_source(args.bootstrap)
    if args.command == "fixture":
        return prepare_fixture(args.verify)
    if args.command == "full":
        return validate_full()
    if args.command == "final":
        return validate_final()
    if args.command == "summary":
        return summary()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
