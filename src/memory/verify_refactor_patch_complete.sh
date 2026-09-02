#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D bounded-container completion gate: FAIL: $1" >&2; exit 1; }

# Preserve the original Patch 1-11 declaration/mechanics chain.
bash "$ROOT/memory/verify_refactor_patch_11.sh" >/dev/null

# The expanded source packet now includes the HLOD/RD runtime files that were
# absent from the original scoped snapshot. Their runtime ownership contract is
# validated by the dedicated gate rather than by a global raw-array allowlist:
# RD/ABI/GPU/indexed tables are intentionally raw under the Patch 8 contract.
bash "$ROOT/memory/verify_runtime_container_ownership.sh" >/dev/null

if grep -RInE --include='*.h' --include='*.cpp' 'std::(vector|deque|list|map|unordered_map|unique_ptr|shared_ptr)' "$ROOT"; then
    fail "STL ownership container introduced"
fi

# Direct EASTL spelling is now the repository policy. Runtime allocator routing
# is validated by the dedicated ownership gate above; no Terrain3D container
# aliases or EASTL container typedefs may be reintroduced.
if grep -RInE --include='*.h' --include='*.cpp' \
    'Terrain3D(SharedVector|FixedVector|FixedArray|FixedBitSet|SharedVectorMap|SharedVectorSet)' "$ROOT"; then
    fail "Terrain3D container alias remains"
fi
if grep -RInE --include='*.h' --include='*.cpp' \
    '^\s*using\s+[^=]+\s*=\s*eastl::(vector|fixed_vector|array|bitset|vector_map|vector_set)<' "$ROOT"; then
    fail "EASTL container type alias remains"
fi

echo "Terrain3D bounded-container completion gate: PASS"
