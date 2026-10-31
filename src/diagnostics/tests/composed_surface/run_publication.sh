#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
unit="$root/src/diagnostics/tests/composed_surface"
clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
    -fsanitize=address,undefined -I "$root/src" \
    "$unit/test_publication_atomicity.cpp" -o /tmp/t3d_comp_surf_publication_test
/tmp/t3d_comp_surf_publication_test
python3 "$unit/test_pipeline_activation_contract.py"
