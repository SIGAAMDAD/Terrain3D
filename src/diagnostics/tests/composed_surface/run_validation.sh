#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
tests="$root/src/diagnostics/tests/composed_surface"
python3 "$tests/test_world_activation.py"
python3 "$tests/test_optin_policy.py"
if [ "${T3D_COMP_SURF_REQUIRE_SPIRV:-0}" = 1 ]; then
    python3 "$tests/test_shader_variants.py" --require-compiler
else
    python3 "$tests/test_shader_variants.py"
fi
