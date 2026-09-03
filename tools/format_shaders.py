#!/usr/bin/env python3
"""Format Terrain3D RD GLSL sources and embedded fallback shader strings."""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

from format_cpp import ClangdClient

ROOT = Path(__file__).resolve().parents[1]
SHADER_ROOT = ROOT / "project" / "addons" / "terrain_3d" / "rd"
EMBEDDED_HEADERS = (
    ROOT / "src" / "rendering" / "rd" / "render_world" / "terrain_3d_render_world_forward_lighting_shader_inc.h",
    ROOT / "src" / "rendering" / "rd" / "render_world" / "terrain_3d_render_world_raster_shaders.h",
    ROOT / "src" / "rendering" / "rd" / "render_world" / "terrain_3d_render_world_vt_shaders.h",
)
RAW_STRING_RE = re.compile(r'R"(?P<delimiter>[A-Za-z0-9_]*)\((?P<body>.*?)\)(?P=delimiter)"', re.DOTALL)


def shader_files() -> list[Path]:
    return sorted(path for path in SHADER_ROOT.glob("*.glsl") if path.is_file())


def protect_version_manifest(text: str) -> tuple[str, dict[str, str]]:
    protected_lines: dict[str, str] = {}
    masked_lines: list[str] = []
    in_versions = False
    for line_number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if stripped == "#[versions]":
            in_versions = True
        elif in_versions and stripped.startswith("#[") and stripped != "#[versions]":
            in_versions = False
        if in_versions:
            token = f"//__T3D_FORMAT_PROTECTED_{line_number:06d}__"
            protected_lines[token] = line
            masked_lines.append(token)
        else:
            masked_lines.append(line)
    masked = "\n".join(masked_lines) + ("\n" if text.endswith("\n") else "")
    return masked, protected_lines


def restore_version_manifest(text: str, protected_lines: dict[str, str]) -> str:
    restored = [protected_lines.get(line.strip(), line) for line in text.splitlines()]
    return "\n".join(restored) + ("\n" if text.endswith("\n") else "")


def format_with_clang_format(executable: str, text: str, assume_filename: Path) -> str:
    result = subprocess.run(
        [executable, "--style=file", f"--assume-filename={assume_filename}"],
        input=text.encode("utf-8"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", errors="replace"))
    return result.stdout.decode("utf-8")


def format_text(
    text: str,
    virtual_path: Path,
    clang_format: str | None,
    clangd_client: ClangdClient | None,
) -> str:
    if clang_format:
        return format_with_clang_format(clang_format, text, virtual_path)
    assert clangd_client is not None
    return clangd_client.formatted_text(virtual_path.resolve(), text)


def format_shader_source(
    path: Path,
    clang_format: str | None,
    clangd_client: ClangdClient | None,
) -> str:
    original = path.read_text(encoding="utf-8")
    masked, protected = protect_version_manifest(original)
    formatted = format_text(masked, path, clang_format, clangd_client)
    return restore_version_manifest(formatted, protected)


def format_embedded_header(
    path: Path,
    clang_format: str | None,
    clangd_client: ClangdClient | None,
) -> str:
    original = path.read_text(encoding="utf-8")
    raw_index = 0

    def replace(match: re.Match[str]) -> str:
        nonlocal raw_index
        raw_index += 1
        delimiter = match.group("delimiter")
        body = match.group("body")
        leading_newline = body.startswith("\n")
        trailing_newline = body.endswith("\n")
        core = body[1:] if leading_newline else body
        if trailing_newline and core.endswith("\n"):
            core = core[:-1]
        if core:
            virtual_path = SHADER_ROOT / f"__embedded_shader_{raw_index:02d}.glsl"
            formatted = format_text(core + "\n", virtual_path, clang_format, clangd_client).rstrip("\n")
        else:
            formatted = core
        rebuilt = ("\n" if leading_newline else "") + formatted + ("\n" if trailing_newline else "")
        return f'R"{delimiter}({rebuilt}){delimiter}"'

    return RAW_STRING_RE.sub(replace, original)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="Report formatting differences without modifying files")
    args = parser.parse_args()

    clang_format = shutil.which("clang-format")
    clangd = None if clang_format else shutil.which("clangd")
    if not clang_format and not clangd:
        print("error: neither clang-format nor clangd is available", file=sys.stderr)
        return 2

    client = ClangdClient(clangd, SHADER_ROOT) if clangd else None
    failed: list[Path] = []
    try:
        files = shader_files()
        for index, path in enumerate(files, start=1):
            original = path.read_text(encoding="utf-8")
            formatted = format_shader_source(path, clang_format, client)
            if formatted != original:
                if args.check:
                    failed.append(path)
                else:
                    path.write_text(formatted, encoding="utf-8", newline="\n")
            if index % 10 == 0 or index == len(files):
                print(f"Formatted shaders {index}/{len(files)}", file=sys.stderr)

        for path in EMBEDDED_HEADERS:
            original = path.read_text(encoding="utf-8")
            formatted = format_embedded_header(path, clang_format, client)
            if formatted != original:
                if args.check:
                    failed.append(path)
                else:
                    path.write_text(formatted, encoding="utf-8", newline="\n")
    finally:
        if client:
            client.close()

    if failed:
        print("Formatting differs in:")
        for path in failed:
            print(path.relative_to(ROOT))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
