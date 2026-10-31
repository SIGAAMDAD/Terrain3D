#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../../.." && pwd)
out="${TMPDIR:-/tmp}/t3d_st02_tail_layout_$$"
trap 'rm -f "$out"' EXIT HUP INT TERM
python3 "$root/src/diagnostics/tests/superterrain/residency/test_compact_roots.py"
clang++ -std=c++17 -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=address,undefined -I "$root/src" \
    "$root/src/diagnostics/tests/superterrain/residency/test_tail_layout.cpp" \
    -o "$out"
"$out"
