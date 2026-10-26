#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
tests="$root/src/diagnostics/tests/composed_surface"
python3 "$tests/test_cs18_world_activation.py"
python3 "$tests/test_cs19_optin_policy.py"
if [ "${T3D_CS19_REQUIRE_SPIRV:-0}" = 1 ]; then
    python3 "$tests/test_cs19_shader_variants.py" --require-compiler
else
    python3 "$tests/test_cs19_shader_variants.py"
fi
