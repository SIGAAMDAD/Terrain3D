#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "Terrain3D EASTL runtime container gate: FAIL: $1" >&2; exit 1; }

for old in \
  "$ROOT/core/terrain_3d_array.h" \
  "$ROOT/core/terrain_3d_fixed_buffer.h" \
  "$ROOT/core/terrain_3d_flat_hash.h" \
  "$ROOT/core/terrain_3d_slab_array.h" \
  "$ROOT/core/terrain_3d_containers.h"; do
  [[ ! -e "$old" ]] || fail "obsolete custom container layer remains: ${old#$ROOT/}"
done

if grep -RInE --include='*.h' --include='*.cpp' \
  'Terrain3D(Array|FixedBuffer|FlatHash(Map|Set)?|HighWaterSlab|SlabArray|TransformArray|SharedVector|FixedVector|FixedArray|FixedBitSet|SharedVectorMap|SharedVectorSet)' "$ROOT"; then
  fail "custom container type or alias remains in source"
fi

if grep -RInE --include='*.h' --include='*.cpp' '^\s*using\s+[^=]+\s*=\s*eastl::(vector|fixed_vector|array|bitset|vector_map|vector_set)<' "$ROOT"; then
  fail "container type alias still hides direct EASTL spelling"
fi

[[ -f "$ROOT/core/terrain_3d_eastl_helpers.h" ]] || fail "direct EASTL helper header missing"
if grep -qE '^\s*using\s+Terrain3D.*=' "$ROOT/core/terrain_3d_eastl_helpers.h"; then
  fail "Terrain3D container alias reintroduced in EASTL helper header"
fi

# Runtime/mixed dynamic storage must spell EASTL directly with the Terrain3D allocator.
# The offline HLOD builder is intentionally allowed to use EASTL's configured default allocator.
while IFS= read -r hit; do
  file="${hit%%:*}"
  rel="${file#$ROOT/}"
  case "$rel" in
    hlod/terrain_3d_hlod_builder.cpp|hlod/terrain_3d_hlod_builder.h) ;;
    *) fail "runtime eastl::vector lacks explicit Terrain3DEastlAllocator: ${hit#$ROOT/}" ;;
  esac
done < <(grep -RIn --include='*.h' --include='*.cpp' 'eastl::vector<' "$ROOT" | grep -v 'Terrain3DEastlAllocator' || true)

if grep -RIn --include='*.h' --include='*.cpp' 'eastl::vector_map<' "$ROOT" | grep -v 'Terrain3DEastlAllocator'; then
  fail "eastl::vector_map without Terrain3DEastlAllocator remains"
fi
if grep -RIn --include='*.h' --include='*.cpp' 'eastl::vector_set<' "$ROOT" | grep -v 'Terrain3DEastlAllocator'; then
  fail "eastl::vector_set without Terrain3DEastlAllocator remains"
fi

# Representative runtime ownership declarations must remain direct EASTL.
grep -q 'eastl::vector<t3dPackedTransform_t, Terrain3DEastlAllocator> m_transforms' "$ROOT/instances/terrain_3d_instance_store.h" || fail "instance transforms are not direct EASTL vector-owned"
grep -q 'eastl::vector<t3dInstanceCell_t, Terrain3DEastlAllocator> m_cells' "$ROOT/instances/terrain_3d_instance_store.h" || fail "instance cells are not direct EASTL vector-owned"
grep -q 'eastl::vector_map<t3dRenderCellKey_t, size_t, t3dRenderCellKeyLess_t, Terrain3DEastlAllocator> m_iRenderCellIndex' "$ROOT/instances/terrain_3d_instancer.h" || fail "render-cell index is not direct EASTL vector_map"
grep -q 'eastl::vector_set<t3dCellUpdateKey_t, t3dCellUpdateKeyLess_t, Terrain3DEastlAllocator> m_streamingUpdates' "$ROOT/instances/terrain_3d_instancer.h" || fail "streaming set is not direct EASTL vector_set"
grep -q 'eastl::vector<t3dHlodRuntimeEntryRecord_t, Terrain3DEastlAllocator> m_entries' "$ROOT/hlod/terrain_3d_hlod_runtime_codec.h" || fail "HLOD runtime entries are not direct EASTL vectors"
grep -q 'eastl::vector<t3dRdInstanceRecord_t, Terrain3DEastlAllocator> m_instances' "$ROOT/rendering/rd/terrain_3d_rd_instance_renderer.h" || fail "RD scene instances are not direct EASTL vectors"
grep -q 'eastl::vector<t3dRdMeshSourceSurface_t, Terrain3DEastlAllocator> m_surfaces' "$ROOT/rendering/rd/terrain_3d_rd_mesh_cache.h" || fail "RD mesh surfaces are not direct EASTL vectors"
grep -q 'eastl::vector_map<t3dRdPacketKey_t, uint32_t, t3dRdPacketKeyLess_t, Terrain3DEastlAllocator> m_packetIndex' "$ROOT/rendering/rd/terrain_3d_rd_instance_renderer.h" || fail "RD packet index is not direct EASTL vector_map"

grep -q 'm_pInstancer->serviceColdCapacity();' "$ROOT/terrain_3d.cpp" || fail "instancer cold capacity service missing"
grep -q 'm_pRenderCoordinator->serviceColdCapacity();' "$ROOT/terrain_3d.cpp" || fail "render coordinator cold capacity service missing"

echo "Terrain3D EASTL runtime container gate: PASS"
