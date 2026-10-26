#!/usr/bin/env python3
"""P10 guard: one main-device descriptor owner and one published VT acquire API.

Run: python3 src/diagnostics/tests/check_global_descriptor_ownership.py
Optional: --src /path/to/src
"""

import argparse
from pathlib import Path
import re
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", type=Path, default=Path(__file__).resolve().parents[2])
    src = parser.parse_args().src.resolve()
    rd = Path("rendering/rd")
    desc = rd / "infrastructure/descriptors"
    cache = desc / "terrain_3d_rd_global_descriptor_cache.cpp"
    wrapper = desc / "terrain_3d_rd_uniform_set.cpp"
    header = desc / "terrain_3d_rd_uniform_set.h"
    old_cache = desc / "terrain_3d_rd_descriptor_cache.h"
    failures = []

    def fail(path: Path, reason: str) -> None:
        failures.append(f"{path}: {reason}")

    if (src / old_cache).exists():
        fail(old_cache, "obsolete owning descriptor cache still exists")
    if not (src / cache).is_file() or not (src / wrapper).is_file():
        fail(desc, "global cache or uniform-set wrapper is missing")
    else:
        global_cache = (src / cache).read_text(encoding="utf-8")
        uniform_wrapper = (src / wrapper).read_text(encoding="utf-8")
        uniform_header = (src / header).read_text(encoding="utf-8")
        if global_cache.count(".createTracked(") != 1:
            fail(cache, "expected exactly one tracked global uniform-set creation")
        if "friend class Terrain3DRdGlobalDescriptorCache;" not in uniform_header:
            fail(header, "tracked-set creation is not restricted by friendship")
        before_public = uniform_header.split("public:", 1)[0]
        if "bool createTracked(" not in before_public or "void releaseTracked(" not in before_public:
            fail(header, "tracked-set creation/release must be private")
        if len(re.findall(r"\buniform_set_create\s*\(", uniform_wrapper)) != 2:
            fail(wrapper, "expected one global and one local RenderingDevice creation path")
        if not re.search(r"\bbool\s+createLocal\s*\(", uniform_header):
            fail(header, "local-device descriptor path must remain available")

    legal_uniform_type_files = {
        cache, desc / "terrain_3d_rd_global_descriptor_cache.h",
        wrapper, header,
        rd / "infrastructure/buffers/terrain_3d_local_compute_buffer_service.h",
        rd / "infrastructure/buffers/terrain_3d_local_compute_buffer_service.cpp",
    }
    deprecated_accessors = re.compile(
        r"\b(?:get_shared_vt_bindings|snapshot_shared_vt_bindings_hot|"
        r"getSharedVtBindingsHot|snapshotSharedVtBindingsHot|bindVirtualTexture)\b"
    )
    source_count = 0
    for path in sorted(src.rglob("*")):
        if path.suffix not in (".h", ".cpp"):
            continue
        relative = path.relative_to(src)
        body = path.read_text(encoding="utf-8", errors="replace")
        source_count += 1
        if re.search(r"\bTerrain3DRdDescriptorCache\b|terrain_3d_rd_descriptor_cache\.h", body):
            fail(relative, "legacy physical descriptor cache used")
        if deprecated_accessors.search(body):
            fail(relative, "legacy borrowed VT owner/binding accessor used")
        if re.search(r"\buniform_set_create\s*\(", body) and relative != wrapper:
            fail(relative, "direct RenderingDevice uniform-set creation")
        if re.search(r"\bTerrain3DRdUniformSet\b", body) and relative not in legal_uniform_type_files:
            fail(relative, "uniform-set ownership outside global cache/local compute")
        if relative not in (cache, wrapper) and re.search(
                r"\b(?:createTracked|releaseTracked)\s*\([^;{}]*?GlobalDescriptor\.UniformSet", body,
                re.DOTALL):
            fail(relative, "physical descriptor ownership bypass")

    if failures:
        for message in failures:
            print(f"FAIL: {message}", file=sys.stderr)
        print(f"FAILED: {len(failures)} ownership violation(s) in {source_count} source files", file=sys.stderr)
        return 1
    print(f"PASS: centralized descriptor ownership and VT acquisition ({source_count} source files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
