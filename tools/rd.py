#!/usr/bin/env python3
"""Compact Terrain3D RenderingDevice workflow driver for Codex/CI-style loops."""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
EXPECTED_BRANCH = "experiment/rendering-device"
WORK_DIR = REPO / "docs" / "rd_work"
BATCH_FILE = REPO / "docs" / "rd_codex_batches.tsv"
SOURCE_MAP_FILE = REPO / "docs" / "rd_source_map.tsv"
CONTRACT_FILE = REPO / "docs" / "rendering_device_completion_contract.md"
SYMBOL_BASELINE_FILE = WORK_DIR / "symbol_baseline.tsv"

SCAN_ROOTS = [REPO / "src", REPO / "project" / "addons" / "terrain_3d"]
TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl",
    ".gd", ".cs", ".json", ".glsl", ".gdshader", ".gdshaderinc",
    ".tscn", ".tres", ".cfg", ".gdextension",
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


def run(command: list[str], *, cwd: Path = REPO, check: bool = True, env: dict[str, str] | None = None, timeout: int | None = None) -> subprocess.CompletedProcess[str]:
    print("+ " + " ".join(command))
    return subprocess.run(command, cwd=str(cwd), text=True, check=check, env=env, timeout=timeout)


def capture(command: list[str], *, cwd: Path = REPO) -> str:
    return subprocess.check_output(command, cwd=str(cwd), text=True).strip()


def current_branch() -> str:
    return capture(["git", "rev-parse", "--abbrev-ref", "HEAD"])


def current_commit() -> str:
    return capture(["git", "rev-parse", "HEAD"])


def branch_check() -> bool:
    branch = current_branch()
    if branch != EXPECTED_BRANCH:
        print(f"FAIL branch={branch}; expected={EXPECTED_BRANCH}")
        return False
    print(f"OK branch={branch}")
    return True


def diff_check() -> bool:
    result = subprocess.run(["git", "diff", "--check"], cwd=str(REPO), text=True)
    if result.returncode != 0:
        print("FAIL git diff --check")
        return False
    print("OK git diff --check")
    return True


def iter_scan_files() -> list[Path]:
    files: list[Path] = []
    for root in SCAN_ROOTS:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            files.append(path)
    return sorted(files)


def scan_symbols() -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {
        name: {"count": 0, "files": {}} for name in SYMBOLS
    }
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


def audit_symbols(*, details: bool = False, strict_growth: bool = True) -> bool:
    scan = scan_symbols()
    baseline = read_symbol_baseline()
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
        if details and files:
            for path, count in sorted(files.items(), key=lambda item: (-item[1], item[0])):
                print(f"  {count:4d}  {path}")
    if not ok:
        print("FAIL forbidden/legacy symbol counts increased relative to the Codex-work baseline")
    return ok


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def next_batch() -> dict[str, str] | None:
    for row in read_tsv(BATCH_FILE):
        if row["state"] not in {"complete", "completed"}:
            return row
    return None


def print_next() -> int:
    row = next_batch()
    if row is None:
        print("No incomplete Codex batch remains. Run the final contract audit before claiming completion.")
        return 0
    print(f"batch={row['batch']}")
    print(f"responses={row['responses']}")
    print(f"state={row['state']}")
    print(f"contract_sections={row['contract_sections']}")
    print(f"goal={row['goal']}")
    print(f"primary_paths={row['primary_paths']}")
    print(f"gate={row['completion_gate']}")
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
    print("\n[current legacy-symbol summary]")
    audit_symbols(details=False, strict_growth=False)
    return 0


def parse_section_spec(spec: str) -> set[int]:
    sections: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            start_text, end_text = part.split("-", 1)
            start = int(start_text)
            end = int(end_text)
            sections.update(range(start, end + 1))
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
    if not emitted:
        print(f"No contract sections found for batch {batch_name}", file=sys.stderr)
        return 1
    return 0


def discover_scons() -> str | None:
    explicit = os.environ.get("SCONS")
    candidates = [
        explicit,
        shutil.which("scons"),
        str(REPO / ".rd_tools" / "venv" / "bin" / "scons"),
        str(REPO / ".rd_tools" / "venv" / "Scripts" / "scons.exe"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def discover_godot() -> str | None:
    explicit = os.environ.get("RD_GODOT") or os.environ.get("GODOT_BIN")
    candidates: list[str | None] = [explicit]
    names = [
        "Godot_v4.7.1-stable_linux.x86_64",
        "Godot_v4.7.1-stable_linux.x86_64.exe",
        "godot4",
        "godot",
    ]
    for base in (REPO, REPO.parent, Path("/mnt/data")):
        for name in names[:2]:
            candidates.append(str(base / name))
    data_root = Path("/mnt/data")
    if data_root.is_dir():
        for name in names[:2]:
            candidates.extend(str(path) for path in data_root.glob(f"*/{name}"))
    candidates.extend([shutil.which("godot4"), shutil.which("godot")])
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def bootstrap() -> int:
    tools_dir = REPO / ".rd_tools"
    venv = tools_dir / "venv"
    scons = discover_scons()
    if scons is None:
        print("SCons not found; creating local .rd_tools/venv and installing scons")
        tools_dir.mkdir(parents=True, exist_ok=True)
        try:
            run([sys.executable, "-m", "venv", str(venv)])
            pip = venv / ("Scripts/pip.exe" if os.name == "nt" else "bin/pip")
            run([str(pip), "install", "--disable-pip-version-check", "scons"])
        except (subprocess.CalledProcessError, OSError) as exc:
            print(f"WARN automatic SCons install failed: {exc}")
            print("Install SCons externally or set SCONS=/absolute/path/to/scons")
        scons = discover_scons()
    print(f"scons={scons or 'missing'}")
    godot = discover_godot()
    print(f"godot={godot or 'missing; set RD_GODOT=/absolute/path/to/Godot_v4.7.1-stable_linux.x86_64'}")
    if godot:
        version = capture([godot, "--version"])
        print(f"godot_version={version}")
        if not version.startswith("4.7.1"):
            print("FAIL discovered Godot is not 4.7.1")
            return 1
    return 0 if scons else 1


def build_extension(extra_args: list[str] | None = None) -> bool:
    scons = discover_scons()
    if scons is None:
        print("FAIL SCons missing; run: python3 tools/rd.py bootstrap")
        return False
    platform = "windows" if os.name == "nt" else "linux"
    jobs = max(1, min(os.cpu_count() or 1, 12))
    command = [
        scons,
        f"platform={platform}",
        "target=editor",
        "arch=x86_64",
        f"-j{jobs}",
    ]
    if extra_args:
        command.extend(extra_args)
    env = os.environ.copy()
    env.setdefault("SCONS_CACHE", str(REPO / ".scons-cache"))
    result = subprocess.run(command, cwd=str(REPO), env=env, text=True)
    if result.returncode != 0:
        print("FAIL native extension build")
        return False
    print("OK native extension build")
    return True


def godot_smoke() -> bool:
    godot = discover_godot()
    if godot is None:
        print("FAIL Godot 4.7.1 missing; set RD_GODOT=/absolute/path/to/Godot_v4.7.1-stable_linux.x86_64")
        return False
    try:
        version = capture([godot, "--version"])
    except (subprocess.CalledProcessError, OSError) as exc:
        print(f"FAIL unable to execute Godot: {exc}")
        return False
    if not version.startswith("4.7.1"):
        print(f"FAIL Godot version={version}; expected 4.7.1")
        return False
    print(f"OK Godot version={version}")
    command = [godot, "--headless", "--editor", "--path", str(REPO / "project"), "--quit-after", "2"]
    try:
        result = subprocess.run(command, cwd=str(REPO), text=True, timeout=180)
    except subprocess.TimeoutExpired:
        print("FAIL Godot headless smoke test timed out")
        return False
    if result.returncode != 0:
        print("FAIL Godot headless editor smoke test")
        return False
    print("OK Godot headless editor smoke test")
    return True


def git_fsck() -> bool:
    result = subprocess.run(["git", "fsck", "--full", "--strict"], cwd=str(REPO), text=True)
    if result.returncode != 0:
        print("FAIL git fsck --full --strict")
        return False
    print("OK git fsck --full --strict")
    return True


def validate_fast(static_only: bool = False) -> int:
    checks = [branch_check(), diff_check(), audit_symbols(details=False, strict_growth=True)]
    if not static_only:
        checks.append(build_extension())
    return 0 if all(checks) else 1


def validate_full() -> int:
    checks = [
        branch_check(),
        diff_check(),
        audit_symbols(details=False, strict_growth=True),
        build_extension(),
        godot_smoke(),
        git_fsck(),
    ]
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
    audit_symbols(details=False, strict_growth=False)
    return 0


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("next", help="Print the next incomplete Codex batch")
    context = sub.add_parser("context", help="Print compact context/source map for one batch")
    context.add_argument("batch")
    contract = sub.add_parser("contract", help="Print only the completion-contract sections assigned to one batch")
    contract.add_argument("batch")
    audit = sub.add_parser("audit", help="Print legacy/forbidden symbol counts")
    audit.add_argument("--details", action="store_true")
    baseline = sub.add_parser("baseline", help="Write the current symbol counts as the non-growth baseline")
    baseline.add_argument("--force", action="store_true")
    sub.add_parser("bootstrap", help="Create local SCons environment and locate Godot 4.7.1")
    sub.add_parser("build", help="Incrementally build the native editor extension")
    fast = sub.add_parser("fast", help="Branch/diff/audit plus incremental native build")
    fast.add_argument("--static-only", action="store_true")
    sub.add_parser("smoke", help="Run Godot 4.7.1 headless editor smoke test")
    sub.add_parser("full", help="Batch completion gate: fast build + Godot smoke + git fsck")
    sub.add_parser("summary", help="Print terse repository/batch/symbol state")
    return parser


def main() -> int:
    args = make_parser().parse_args()
    if args.command == "next":
        return print_next()
    if args.command == "context":
        return print_context(args.batch)
    if args.command == "contract":
        return print_contract_for_batch(args.batch)
    if args.command == "audit":
        return 0 if audit_symbols(details=args.details, strict_growth=False) else 1
    if args.command == "baseline":
        if SYMBOL_BASELINE_FILE.exists() and not args.force:
            print(f"Baseline already exists: {SYMBOL_BASELINE_FILE.relative_to(REPO)}; use --force to replace it", file=sys.stderr)
            return 2
        scan = scan_symbols()
        write_symbol_baseline(scan)
        print(f"Wrote {SYMBOL_BASELINE_FILE.relative_to(REPO)}")
        audit_symbols(details=False, strict_growth=True)
        return 0
    if args.command == "bootstrap":
        return bootstrap()
    if args.command == "build":
        return 0 if build_extension() else 1
    if args.command == "fast":
        return validate_fast(static_only=args.static_only)
    if args.command == "smoke":
        return 0 if godot_smoke() else 1
    if args.command == "full":
        return validate_full()
    if args.command == "summary":
        return summary()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
