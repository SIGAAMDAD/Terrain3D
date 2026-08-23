#!/usr/bin/env python3
"""Low-noise Terrain3D RenderingDevice workflow driver for Codex/Windows."""

from __future__ import annotations

import argparse
import csv
import hashlib
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
DEFAULT_WINDOWS_GODOT_ROOT = Path(r"D:\Godot_v4.7.1-stable_mono_win64")
DEFAULT_ENGINE_SOURCE = TOOLS_DIR / "godot-4.7.1-src"
FIXTURE_ARCHIVE = REPO / "validation" / "terrain3d_00_02.zip"
FIXTURE_DIR = TOOLS_DIR / "validation_data" / "terrain3d_00_02"
FIXTURE_MANIFEST = REPO / "docs" / "rd_reference" / "terrain3d_00_02_manifest.tsv"

SCAN_ROOTS = [
    REPO / "src",
    REPO / "project" / "addons" / "terrain_3d",
    REPO / "engine_patch",
]
TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl",
    ".gd", ".cs", ".json", ".glsl", ".gdshader", ".gdshaderinc",
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


def current_branch() -> str:
    return capture(["git", "rev-parse", "--abbrev-ref", "HEAD"])


def current_commit() -> str:
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
    branch = current_branch()
    if branch != EXPECTED_BRANCH:
        print(f"FAIL branch={branch}; expected={EXPECTED_BRANCH}")
        return False
    print(f"OK branch={branch}")
    return True


def reference_check() -> bool:
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
        ("material layer ABI size", 'sizeof( t3dRdMaterialLayer_t ) == 64u' in types),
        ("material layer integral texture index", 'uint32_t m_nPhysicalLayer' in types),
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
        ("runtime texture object retained for RD publication", 'm_runtimeMaterialParams[strName] = rValue;' in material_cpp and 'const Ref<Texture> texture = rValue;' in material_cpp and 'native_value = texture->get_rid();' in material_cpp),
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
    candidates = list(root.glob("*Godot*4.7.1*.exe")) + list(root.glob("Godot*.exe"))
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
            candidates.extend(base.glob("**/Godot_v4.7.1-stable_linux.x86_64"))
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
            if not version.startswith("4.7.1"):
                print("FAIL Godot is not 4.7.1")
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


def godot_headless_import() -> bool:
    godot, base = godot_command_base()
    if godot is None:
        print("FAIL Godot 4.7.1 missing; set RD_GODOT_ROOT or RD_GODOT")
        return False
    command = base + ["--headless", "--render-thread", "safe", "--import", "--log-file", str(log_path("godot_import_engine"))]
    return run_quiet(command, label="godot_import", timeout=300)


def godot_editor_headless() -> bool:
    godot, base = godot_command_base()
    if godot is None:
        print("FAIL Godot 4.7.1 missing")
        return False
    command = base + ["--headless", "--render-thread", "safe", "--editor", "--quit-after", "5", "--log-file", str(log_path("godot_editor_engine"))]
    env = os.environ.copy()
    env["TERRAIN3D_RUN_RD_CPU_TESTS"] = "1"
    return run_quiet(command, label="godot_editor_headless", env=env, timeout=300)


def godot_build_solutions() -> bool:
    project = REPO / "project" / "Terrain3D.csproj"
    if not project.exists():
        print("OK C# project absent; skipped")
        return True
    dotnet = shutil.which("dotnet")
    if dotnet is None:
        print("FAIL dotnet missing; install the .NET 8 SDK required by Godot.NET.Sdk/4.7.1")
        return False
    command = [dotnet, "build", str(project), "--nologo", "--verbosity", "minimal"]
    return run_quiet(command, label="dotnet_build", timeout=600)

def godot_recovery_smoke() -> bool:
    godot, base = godot_command_base()
    if godot is None:
        print("FAIL Godot 4.7.1 missing")
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
        print("FAIL Godot 4.7.1 missing")
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
    if marker not in validation_text:
        print(f"FAIL gpu_{driver}: RD GPU/CPU culling comparison did not execute")
        return False
    print(f"OK gpu_{driver}: RD GPU/CPU culling comparison")
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
            ["git", "clone", "--depth", "1", "--branch", "4.7.1-stable", "https://github.com/godotengine/godot.git", str(path)],
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
    if tag != "4.7.1-stable" or not commit.startswith("a13da4f"):
        print("FAIL Godot source is not exact 4.7.1-stable")
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
    return run_quiet(["git", "fsck", "--full", "--strict"], label="git_fsck", timeout=300)


def validate_fast(static_only: bool = False) -> int:
    checks = [branch_check(), reference_check(), diff_check(), build_contract_check(), rd_shader_abi_check(), rd_batch_c_material_check(), audit_symbols(strict_growth=True)]
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
    status = capture(["git", "status", "--short"])
    row = next_batch()
    print(f"branch={current_branch()}")
    print(f"commit={current_commit()}")
    print(f"worktree={'clean' if not status else 'dirty'}")
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
    engine = sub.add_parser("engine-source", help="Locate or shallow-clone exact Godot 4.7.1 source for Batch F")
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
