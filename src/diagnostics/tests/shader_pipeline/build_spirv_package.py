#!/usr/bin/env python3
"""Compile a declared set of RDShaderFile versions into a content-addressed SPIR-V ZIP.

Offline build artifact; this does not replace RDShaderFile or Godot driver pipeline caches.
Requires glslangValidator. An explicit optimizer is optional; never use glslang -O.
Only compiler and SPIR-V validation are performed, not GPU/runtime tests.
"""

import argparse
import ast
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

STAGES = {"vertex": "vert", "fragment": "frag", "compute": "comp"}
ZIP_TIME = (1980, 1, 1, 0, 0, 0)


def parse_shader(root):
    text = root.read_text(encoding="utf-8")
    sections = {}
    matches = list(re.finditer(r"^#\[(\w+)\]\s*$", text, re.M))
    for i, match in enumerate(matches):
        sections[match.group(1)] = text[match.end():(matches[i + 1].start() if i + 1 < len(matches) else len(text))]
    versions = {}
    for name, literal in re.findall(r'^(\w+)\s*=\s*("(?:[^"\\]|\\.)*")\s*;', sections.get("versions", ""), re.M):
        if name in versions:
            raise ValueError(f"duplicate version {name} in {root}")
        versions[name] = ast.literal_eval(literal)
    if not versions:
        versions["default"] = ""
    return sections, versions


def stage_text(text, defines):
    source = "#extension GL_GOOGLE_include_directive : require\n" + defines + "\n"
    if "#VERSION_DEFINES" in text:
        return text.replace("#VERSION_DEFINES", source, 1).lstrip("\n")
    if "#version" in text:
        return re.sub(r"(^\s*#version[^\n]*\n)", lambda m: m.group(1) + source, text, count=1, flags=re.M).lstrip("\n")
    raise ValueError("shader stage lacks #version and #VERSION_DEFINES")


def command(args):
    process = subprocess.run(args, capture_output=True, text=True, timeout=180)
    if process.returncode:
        raise RuntimeError("command failed ({}):\n{}\n{}".format(" ".join(map(str, args)), process.stdout[-5000:], process.stderr[-5000:]))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def zip_member(archive, path, content):
    info = zipfile.ZipInfo(path, ZIP_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    archive.writestr(info, content, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-root", required=True, type=Path, help="Directory containing RDShaderFile roots and includes")
    parser.add_argument("--selection", required=True, type=Path, help="JSON manifest of explicitly accepted shader root/version pairs")
    parser.add_argument("--glslang", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--spv-tool", type=Path, help="Optional optimizer+validator executable used for the measured non-inlining pipeline")
    parser.add_argument("--optimization", choices=("none", "conservative", "balanced", "intensive"), default="none")
    parser.add_argument("--spirv-opt", type=Path, help="Optional Khronos spirv-opt executable for stripping debug metadata")
    parser.add_argument("--strip-debug", action="store_true")
    args = parser.parse_args()
    if args.optimization != "none" and args.spv_tool is None:
        parser.error("--optimization requires --spv-tool")
    if args.strip_debug and args.spirv_opt is None:
        parser.error("--strip-debug requires --spirv-opt")
    root = args.shader_root.resolve()
    selected = json.loads(args.selection.read_text(encoding="utf-8"))
    if selected.get("schema") != "terrain3d-spirv-selection-v1" or not isinstance(selected.get("variants"), list):
        raise ValueError("selection requires schema terrain3d-spirv-selection-v1 and a variants array")
    entries = []
    for v in selected["variants"]:
        filename, version = v["shader"], v["version"]
        if not isinstance(filename, str) or not isinstance(version, str) or not filename.endswith(".glsl"):
            raise ValueError("invalid selection record")
        candidate = (root / filename).resolve()
        if root not in candidate.parents or not candidate.is_file():
            raise ValueError(f"shader source unavailable or outside shader root: {filename}")
        entries.append((candidate.relative_to(root).as_posix(), version, candidate))
    if not entries or len(set((a, b) for a, b, _ in entries)) != len(entries):
        raise ValueError("selection must be nonempty and have no duplicate root/version pairs")
    records, blobs = [], {}
    with tempfile.TemporaryDirectory(prefix="t3d_spirv_build_") as td:
        temp = Path(td)
        for index, (filename, version, shader) in enumerate(sorted(entries)):
            sections, versions = parse_shader(shader)
            if version not in versions:
                raise ValueError(f"version {version!r} missing from {filename}")
            for stage, gl_stage in STAGES.items():
                if stage not in sections:
                    continue
                source = temp / f"variant_{index}_{stage}.{gl_stage}"
                raw = temp / f"raw_{index}_{stage}.spv"
                source.write_text(stage_text(sections[stage], versions[version]), encoding="utf-8")
                command([str(args.glslang), "-V", "--target-env", "vulkan1.2", "-Od", "--spirv-val", f"-I{root}", "-S", gl_stage, "-o", str(raw), str(source)])
                result = raw
                if args.optimization != "none":
                    optimized = temp / f"optimized_{index}_{stage}.spv"
                    command([str(args.spv_tool), "optimize", str(result), str(optimized), args.optimization])
                    command([str(args.spv_tool), "validate", str(optimized)])
                    result = optimized
                if args.strip_debug:
                    stripped = temp / f"stripped_{index}_{stage}.spv"
                    command([str(args.spirv_opt), "--strip-debug", "--compact-ids", "-o", str(stripped), str(result)])
                    if args.spv_tool is not None:
                        command([str(args.spv_tool), "validate", str(stripped)])
                    result = stripped
                data = result.read_bytes()
                hash_value = sha(data)
                blobs.setdefault(hash_value, data)
                records.append({"shader": filename, "version": version, "stage": stage, "sha256": hash_value,
                                "module": f"modules/{hash_value}.spv", "bytes": len(data),
                                "root_source_sha256": sha(shader.read_bytes())})
    metadata = {"schema": "terrain3d-spirv-package-v1", "target": "vulkan1.2",
                "optimization": args.optimization, "stripped": args.strip_debug,
                "stages": len(records), "unique_modules": len(blobs),
                "total_logical_bytes": sum(r["bytes"] for r in records),
                "unique_module_bytes": sum(len(data) for data in blobs.values()), "modules": records}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w") as archive:
        for hash_value, data in sorted(blobs.items()):
            zip_member(archive, f"modules/{hash_value}.spv", data)
        zip_member(archive, "manifest.json", (json.dumps(metadata, sort_keys=True, indent=2) + "\n").encode())
    print("stages={} unique={} logical_bytes={} stored_bytes={} zip_bytes={}".format(
          len(records), len(blobs), metadata["total_logical_bytes"], metadata["unique_module_bytes"], args.output.stat().st_size))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
