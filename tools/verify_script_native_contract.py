#!/usr/bin/env python3
from pathlib import Path
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CONSTANTS = ROOT / "src" / "Terrain3DConstants.h"
EDITOR_PLUGIN = ROOT / "project" / "addons" / "terrain_3d" / "src" / "editor_plugin.gd"
PROFILER = ROOT / "project" / "addons" / "terrain_3d" / "src" / "runtime_profiler_window.gd"
METADATA = ROOT / "project" / "addons" / "terrain_3d" / "setting_metadata.json"


def require_match(text: str, pattern: str, label: str) -> str:
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing {label}")
    return match.group(1)


def main() -> int:
    constants = CONSTANTS.read_text(encoding="utf-8")
    plugin = EDITOR_PLUGIN.read_text(encoding="utf-8")
    profiler = PROFILER.read_text(encoding="utf-8")
    metadata = json.loads(METADATA.read_text(encoding="utf-8"))
    props = metadata["properties"]

    native_revision = require_match(
        constants,
        r"#define\s+s_nNativeApiRevision\s*\(\s*([0-9]+)LL\s*\)",
        "native API revision",
    )
    native_build = require_match(
        constants,
        r'#define\s+s_pszNativeBuildId\s*\(\s*"([^"]+)"\s*\)',
        "native build ID",
    )
    script_revision = require_match(
        plugin,
        r"^const\s+EXPECTED_NATIVE_API_REVISION:\s*int\s*=\s*([0-9]+)\s*$",
        "GDScript expected API revision",
    )
    script_build = require_match(
        plugin,
        r'^const\s+EXPECTED_NATIVE_BUILD_ID:\s*String\s*=\s*"([^"]+)"\s*$',
        "GDScript expected build ID",
    )

    assert native_revision == script_revision, (
        f"native/script API revision mismatch: native={native_revision} script={script_revision}"
    )
    assert native_build == script_build, (
        f"native/script build ID mismatch: native={native_build!r} script={script_build!r}"
    )

    assert "Clamp detail to capacity" not in profiler
    assert "_clamp_detail_range" not in profiler
    assert "Fit detail residency to capacity" in profiler
    assert "_fit_detail_range_to_capacity" in profiler

    sync_tip = props["Terrain3D.runtime_sync_detail_lods"]["tooltip"]
    valid_tip = props["Terrain3D.gpu_region_detail_range_valid"]["tooltip"]
    max_tip = props["Terrain3D.gpu_region_max_supported_detail_range_m"]["tooltip"]
    cooked_tip = props["Terrain3D.gpu_region_cooked_pages_enabled"]["tooltip"]

    assert "does not rewrite Near/Middle/Far" in sync_tip
    assert "residency-health diagnostic only" in valid_tip
    assert "does not alter Near/Middle/Far render tiers" in max_tip
    assert "Shipping/runtime treats prepared cooked pages as authoritative" in cooked_tip
    assert "Editor Authoring may fall back to live authored maps" in cooked_tip

    print("PASS: native/GDScript API contract and capacity semantics are synchronized")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
