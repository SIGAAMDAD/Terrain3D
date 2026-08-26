#!/usr/bin/env python3
"""Generate deterministic repository and public-API inventories for the RD branch."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def run_git(repo: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True)


def git_manifest(repo: Path, ref: str) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    paths = run_git(repo, "ls-tree", "-r", "--name-only", ref).splitlines()
    for path in paths:
        mode_type_hash = run_git(repo, "ls-tree", ref, "--", path).rstrip("\n")
        parts = mode_type_hash.split(None, 3)
        blob_hash = parts[2] if len(parts) >= 3 else ""
        size = int(run_git(repo, "cat-file", "-s", f"{ref}:{path}").strip())
        rows.append({"path": path, "bytes": size, "git_blob": blob_hash})
    return rows


def write_manifest(path: Path, ref: str, rows: list[dict[str, object]]) -> None:
    total = sum(int(row["bytes"]) for row in rows)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(f"# Git file manifest: {ref}\n")
        handle.write(f"# files={len(rows)} bytes={total}\n")
        handle.write("bytes\tgit_blob\tpath\n")
        for row in rows:
            handle.write(f"{row['bytes']}\t{row['git_blob']}\t{row['path']}\n")



def write_worktree_manifest(repo: Path, path: Path) -> None:
    listed = run_git(repo, "ls-files", "-co", "--exclude-standard").splitlines()
    rows: list[tuple[str, int, str]] = []
    out_resolved = path.resolve()
    for rel in sorted(set(listed)):
        source = (repo / rel).resolve()
        if source == out_resolved or not source.is_file():
            continue
        data = source.read_bytes()
        rows.append((rel, len(data), hashlib.sha256(data).hexdigest()))
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("# experiment/rendering-device worktree manifest; self file excluded by definition\n")
        handle.write(f"# files={len(rows)} bytes={sum(row[1] for row in rows)}\n")
        handle.write("bytes\tsha256\tpath\n")
        for rel, size, digest in rows:
            handle.write(f"{size}\t{digest}\t{rel}\n")

def parse_registered_classes(register_cpp: str) -> list[str]:
    return sorted(set(re.findall(r"ClassDB::register_class<([A-Za-z0-9_]+)>\(\)", register_cpp)))


def parse_class_bases(src_root: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    pattern = re.compile(r"class\s+([A-Za-z0-9_]+)\s*:\s*public\s+([A-Za-z0-9_:<>]+)")
    for header in src_root.rglob("*.h"):
        text = header.read_text(encoding="utf-8", errors="ignore")
        for match in pattern.finditer(text):
            result[match.group(1)] = match.group(2)
    return result


def extract_bind_blocks(text: str) -> list[tuple[str, str]]:
    starts = list(re.finditer(r"void\s+([A-Za-z0-9_]+)::_bind_methods\s*\(\s*void\s*\)\s*\{", text))
    blocks: list[tuple[str, str]] = []
    for start in starts:
        depth = 1
        index = start.end()
        while index < len(text) and depth:
            char = text[index]
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
            index += 1
        blocks.append((start.group(1), text[start.end(): index - 1]))
    return blocks


def parse_api(src_root: Path) -> dict[str, dict[str, object]]:
    api: dict[str, dict[str, object]] = {}
    for cpp in src_root.rglob("*.cpp"):
        text = cpp.read_text(encoding="utf-8", errors="ignore")
        for class_name, block in extract_bind_blocks(text):
            entry = api.setdefault(class_name, {
                "methods": set(), "properties": set(), "signals": set(), "enum_constants": set(), "groups": set(), "sources": set()
            })
            entry["sources"].add(cpp.relative_to(src_root.parent).as_posix())
            for method in re.findall(r"D_METHOD\(\s*\"([^\"]+)\"", block):
                entry["methods"].add(method)
            for prop in re.findall(r"PropertyInfo\([^\n]*?\"([^\"]+)\"", block):
                entry["properties"].add(prop)
            for signal in re.findall(r"ADD_SIGNAL\s*\(\s*MethodInfo\(\s*\"([^\"]+)\"", block):
                entry["signals"].add(signal)
            for constant in re.findall(r"BIND_ENUM_CONSTANT\(\s*([A-Za-z0-9_]+)\s*\)", block):
                entry["enum_constants"].add(constant)
            for group in re.findall(r"ADD_GROUP\(\s*\"([^\"]+)\"", block):
                entry["groups"].add(group)
    serializable: dict[str, dict[str, object]] = {}
    for class_name, entry in api.items():
        serializable[class_name] = {key: sorted(value) if isinstance(value, set) else value for key, value in entry.items()}
    return serializable


def setting_inventory(repo: Path) -> dict[str, object]:
    path = repo / "project/addons/terrain_3d/setting_metadata.json"
    if not path.exists():
        return {"path": None, "keys": []}
    data = json.loads(path.read_text(encoding="utf-8"))
    keys: list[str] = []
    if isinstance(data, dict):
        properties = data.get("properties")
        if isinstance(properties, dict):
            for key, value in properties.items():
                if isinstance(value, dict) and "key" in value:
                    keys.append(str(value["key"]))
                elif isinstance(key, str):
                    keys.append(key)
        else:
            for key, value in data.items():
                if isinstance(value, dict) and "key" in value:
                    keys.append(str(value["key"]))
                elif isinstance(key, str) and "." in key:
                    keys.append(key)
    return {"path": path.relative_to(repo).as_posix(), "keys": sorted(set(keys))}


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_api_markdown(path: Path, registered: list[str], bases: dict[str, str], api: dict[str, dict[str, object]], settings: dict[str, object]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("# RenderingDevice Reference Public API Inventory\n\n")
        handle.write("Generated from the frozen pre-clean-break source. This file is a mechanical inventory, not a statement that every API remains desirable in the final clean-break branch.\n\n")
        handle.write(f"Registered native classes: **{len(registered)}**\n\n")
        for class_name in registered:
            entry = api.get(class_name, {})
            base = bases.get(class_name, "unknown")
            handle.write(f"## {class_name}\n\n")
            handle.write(f"Base: `{base}`\n\n")
            for label, key in (("Methods", "methods"), ("Properties", "properties"), ("Signals", "signals"), ("Enum constants", "enum_constants"), ("Inspector groups", "groups")):
                values = list(entry.get(key, []))
                handle.write(f"### {label} ({len(values)})\n\n")
                if values:
                    for value in values:
                        handle.write(f"- `{value}`\n")
                else:
                    handle.write("- None mechanically detected.\n")
                handle.write("\n")
        keys = list(settings.get("keys", []))
        handle.write("# Editor/settings metadata inventory\n\n")
        handle.write(f"Metadata keys detected: **{len(keys)}**\n\n")
        for key in keys:
            handle.write(f"- `{key}`\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=".")
    parser.add_argument("--reference", default="rd-alpha-pre-cleanbreak-reference")
    parser.add_argument("--start-ref", default="rd-alpha-pre-cleanbreak-reference")
    args = parser.parse_args()
    repo = Path(args.repo).resolve()
    out = repo / "docs/rd_reference"
    out.mkdir(parents=True, exist_ok=True)

    reference_rows = git_manifest(repo, args.reference)
    branch_rows = git_manifest(repo, args.start_ref)
    write_manifest(out / "reference_file_manifest.tsv", args.reference, reference_rows)
    write_manifest(out / "implementation_start_file_manifest.tsv", args.start_ref, branch_rows)

    src_root = repo / "src"
    register_cpp = (src_root / "register_types.cpp").read_text(encoding="utf-8", errors="ignore")
    registered = parse_registered_classes(register_cpp)
    bases = parse_class_bases(src_root)
    api = parse_api(src_root)
    settings = setting_inventory(repo)

    inventory = {
        "reference": args.reference,
        "branch": run_git(repo, "rev-parse", "--abbrev-ref", "HEAD").strip(),
        "registered_classes": registered,
        "class_bases": bases,
        "api": api,
        "settings": settings,
    }
    (out / "public_api_inventory.json").write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_api_markdown(out / "public_api_inventory.md", registered, bases, api, settings)

    tracked_contract = repo / "docs/rendering_device_completion_contract.md"
    if tracked_contract.exists():
        (out / "contract_sha256.txt").write_text(file_sha256(tracked_contract) + "  docs/rendering_device_completion_contract.md\n", encoding="utf-8")
    write_worktree_manifest(repo, out / "branch_worktree_manifest.tsv")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
