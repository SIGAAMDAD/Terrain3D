# Terrain3D Instancer P0 — RD ArrayMesh Surface Binding Fix

**Date:** 2026-09-09  
**Scope:** direct-RD static mesh source publication used by Terrain3D instancing

## Failure

The RD mesh-source builder called `Mesh::_surface_get_primitive_type()`, `_surface_get_array_len()`, and `_surface_get_array_index_len()` on engine-provided meshes. In the generated godot-cpp `Mesh` base class those virtual stubs return zero; they are extension override hooks, not the engine-bound `ArrayMesh` accessors.

Because `Mesh::PRIMITIVE_TRIANGLES` is `3`, every real triangle surface was rejected before vertex/index publication. The observed runtime result was therefore internally consistent: instance records published successfully, while mesh publication reported zero vertex bytes, zero LODs, zero surfaces, and zero draw packets.

## Patch

- `terrain_3d_rd_mesh_cache.cpp` now includes `ArrayMesh` explicitly.
- RD mesh publication requires an `ArrayMesh` and uses:
  - `ArrayMesh::surface_get_primitive_type()`
  - `ArrayMesh::surface_get_array_len()`
  - `ArrayMesh::surface_get_array_index_len()`
- Source fingerprinting uses the same engine-bound `ArrayMesh` accessors and hashes whether the source is an `ArrayMesh`.
- A non-empty source which produces zero usable triangle surfaces now fails publication with an explicit error instead of silently succeeding with an empty mesh source.
- The native build ID is now `instancer-rd-arraymesh-surface-binding-p0-2026-09-09`.

## Non-goals

This patch does not alter instance transforms, culling, indirect addressing, Forward+ submission, active-storage ownership, or runtime memory budgeting. It isolates the immediate P0 geometry-publication failure.

## Verification

Run:

```text
python tools/verify_instancer_rd_arraymesh_surface_binding.py
python tools/verify_instancer_authoritative_store_publication.py
python tools/verify_instancer_rd_port_integrated.py
python tools/verify_instancer_direct_surface_bringup.py
python tools/verify_instancer_minimal_render_path.py
```

A runtime verification should show non-zero `published_vertex_bytes`, `published_lods`, and `published_surfaces` for the 18 records that currently survive asset filtering.
