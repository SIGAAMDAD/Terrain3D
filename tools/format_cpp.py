#!/usr/bin/env python3
"""Format Terrain3D-authored C/C++ using the repository .clang-format.

Prefers clang-format when installed. Falls back to clangd's LSP formatting
implementation so the same style can be applied in toolchains that ship clangd
without the standalone clang-format executable.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (ROOT / "src",)
SUFFIXES = {".cpp", ".h", ".hpp", ".c", ".cc"}


def source_files() -> list[Path]:
    files: list[Path] = []
    for source_root in SOURCE_ROOTS:
        if not source_root.exists():
            continue
        for path in source_root.rglob("*"):
            if path.is_file() and path.suffix.lower() in SUFFIXES and "gen" not in path.relative_to(source_root).parts:
                files.append(path)
    return sorted(files)


def format_with_clang_format(executable: str, files: list[Path], check: bool) -> int:
    if check:
        failed: list[Path] = []
        for path in files:
            result = subprocess.run(
                [executable, "--style=file", str(path)],
                cwd=ROOT,
                check=False,
                stdout=subprocess.PIPE,
            )
            if result.returncode != 0 or result.stdout != path.read_bytes():
                failed.append(path)
        if failed:
            print("Formatting differs in:")
            for path in failed:
                print(path.relative_to(ROOT))
            return 1
        return 0

    result = subprocess.run(
        [executable, "--style=file", "-i", *[str(path) for path in files]],
        cwd=ROOT,
        check=False,
    )
    return result.returncode


class ClangdClient:
    def __init__(self, executable: str, root: Path = ROOT) -> None:
        self.root = root.resolve()
        self.proc = subprocess.Popen(
            [executable, "--log=error"],
            cwd=self.root,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.next_id = 1
        self._request(
            "initialize",
            {
                "processId": os.getpid(),
                "rootUri": self.root.as_uri(),
                "capabilities": {},
            },
        )
        self._notify("initialized", {})

    def _write(self, payload: dict) -> None:
        assert self.proc.stdin is not None
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.proc.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii"))
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def _read(self) -> dict:
        assert self.proc.stdout is not None
        headers: dict[str, str] = {}
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("clangd exited while waiting for a response")
            if line in (b"\r\n", b"\n"):
                break
            key, value = line.decode("ascii").split(":", 1)
            headers[key.lower()] = value.strip()
        length = int(headers["content-length"])
        return json.loads(self.proc.stdout.read(length))

    def _request(self, method: str, params: dict) -> dict:
        request_id = self.next_id
        self.next_id += 1
        self._write({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        while True:
            response = self._read()
            if response.get("id") == request_id:
                if "error" in response:
                    raise RuntimeError(f"clangd {method} failed: {response['error']}")
                return response

    def _notify(self, method: str, params: dict) -> None:
        self._write({"jsonrpc": "2.0", "method": method, "params": params})

    @staticmethod
    def _position_to_offset(text: str, position: dict) -> int:
        target_line = position["line"]
        target_utf16 = position["character"]
        lines = text.splitlines(keepends=True)
        base = sum(len(line) for line in lines[:target_line])
        if target_line >= len(lines):
            return len(text)
        line = lines[target_line]
        units = 0
        index = 0
        while index < len(line) and units < target_utf16:
            units += 2 if ord(line[index]) > 0xFFFF else 1
            index += 1
        return base + index

    def formatted_text(self, path: Path, text: str) -> str:
        uri = path.as_uri()
        self._notify(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": uri,
                    "languageId": "cpp",
                    "version": 1,
                    "text": text,
                }
            },
        )
        response = self._request(
            "textDocument/formatting",
            {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 4, "insertSpaces": False},
            },
        )
        edits = response.get("result") or []
        for edit in sorted(
            edits,
            key=lambda item: self._position_to_offset(text, item["range"]["start"]),
            reverse=True,
        ):
            start = self._position_to_offset(text, edit["range"]["start"])
            end = self._position_to_offset(text, edit["range"]["end"])
            text = text[:start] + edit["newText"] + text[end:]
        self._notify("textDocument/didClose", {"textDocument": {"uri": uri}})
        return text

    def close(self) -> None:
        try:
            self._request("shutdown", {})
            self._notify("exit", {})
        finally:
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.terminate()


def format_with_clangd(executable: str, files: list[Path], check: bool) -> int:
    client = ClangdClient(executable)
    failed: list[Path] = []
    try:
        for index, path in enumerate(files, start=1):
            original = path.read_text(encoding="utf-8")
            formatted = client.formatted_text(path.resolve(), original)
            if formatted != original:
                if check:
                    failed.append(path)
                else:
                    path.write_text(formatted, encoding="utf-8", newline="\n")
            if index % 25 == 0 or index == len(files):
                print(f"Formatted {index}/{len(files)}", file=sys.stderr)
    finally:
        client.close()
    if failed:
        print("Formatting differs in:")
        for path in failed:
            print(path.relative_to(ROOT))
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="Report files that differ without modifying them")
    args = parser.parse_args()
    files = source_files()
    clang_format = shutil.which("clang-format")
    if clang_format:
        return format_with_clang_format(clang_format, files, args.check)
    clangd = shutil.which("clangd")
    if not clangd:
        print("error: neither clang-format nor clangd is available", file=sys.stderr)
        return 2
    return format_with_clangd(clangd, files, args.check)


if __name__ == "__main__":
    raise SystemExit(main())
