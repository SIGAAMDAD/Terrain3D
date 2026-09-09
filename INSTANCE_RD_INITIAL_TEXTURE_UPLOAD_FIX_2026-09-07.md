# Terrain3D instance RD initial-texture upload fix - 2026-09-07

## Symptom

Terrain3D instance placement remained authoritative: instance counts increased and placement data persisted, but no native RenderingDevice instance geometry appeared. RD mesh preview also failed. Earlier logs included invalid texture creation symptoms such as an impossible mip count and invalid downstream RIDs.

## Root cause

The remaining universal blocker was in cold RenderingDevice texture creation, before alpha-scissor draw submission.

Several textures were created with non-empty initial byte arrays while their `RDTextureFormat::usage_bits` contained only `TEXTURE_USAGE_SAMPLING_BIT`. Godot's RenderingDevice validates initial texture data through its update-capable upload path. A texture supplied with initial data therefore requires `TEXTURE_USAGE_CAN_UPDATE_BIT` at creation time.

The instance renderer always requires fallback albedo, normal, and ORM textures while it builds material descriptor sets. Even a mesh with a valid albedo texture still needs fallback bindings for absent material slots and for the default material pair. If those 1x1 fallback textures fail to create, `ensureFallbackMaterialTextures()` fails, `rebuildMaterialBindings()` fails, and both editor preview and runtime instance scene preparation terminate before a draw can be submitted.

The previous alpha-scissor patch also overrode imported material texture layouts to sampling-only. That assumption was incorrect for `texture_create(..., initial_data)`: the resource can remain immutable after publication, but the one cold initialization upload still needs CAN_UPDATE.

## Changes

### Instance material fallbacks

`src/rendering/rd/terrain_3d_rd_instance_renderer.cpp`

- 1x1 fallback albedo/normal/ORM textures now use:
  - `TEXTURE_USAGE_SAMPLING_BIT`
  - `TEXTURE_USAGE_CAN_UPDATE_BIT`
- fallback texture creation failures now record an explicit RD resource failure and renderer error instead of collapsing into a generic material upload failure.

### Imported instance material textures

`src/rendering/rd/terrain_3d_rd_mesh_cache.cpp`

- imported albedo/normal/ORM texture layouts now retain `TEXTURE_USAGE_CAN_UPDATE_BIT` for their initial upload;
- they remain logically immutable after publication;
- optional material texture failure still falls back instead of evicting otherwise valid mesh geometry.

### Shared RD fallback resources

The same invalid initial-data usage existed in other direct-RD fallback creation paths and was corrected at the same boundary:

- `src/rendering/rd/terrain_3d_rd_forward_lighting.cpp`
  - fallback color texture
  - fallback color-array texture
  - fallback radiance texture
  - fallback radiance-array texture
- `src/rendering/rd/terrain_3d_rd_clipmap_renderer.cpp`
  - fallback displacement texture
  - fallback texture array

The empty-data fallback shadow texture is unchanged because it does not provide initial payload data.

### Texture-manager contract gate

`src/rendering/rd/terrain_3d_rd_texture_manager.cpp`

The texture manager now rejects a non-empty initial upload when `TEXTURE_USAGE_CAN_UPDATE_BIT` is absent and emits a direct diagnostic. This prevents a future material path from silently recreating the same invalid RD contract.

### Regression coverage

Added `tools/verify_rd_initial_texture_usage.py` and updated the alpha-scissor verifier so that the old sampling-only imported-material rule cannot be reintroduced.

No GDScript API or `.gd` source was modified.

## Why this matches the observed failure

The instance count is upstream logical state. Material descriptor construction is downstream GPU publication. A failure in fallback texture creation leaves the authoritative placement count intact while preventing any instance material descriptor set from becoming drawable. That produces the observed split: placement/count works, geometry and RD thumbnails do not.

This failure is also independent of GPU culling and indirect drawing. The editor preview path uses CPU-built visibility, yet it still requires the same material fallbacks. Preview failure and runtime failure therefore converge on the same resource-creation boundary.

## Verification

- 34/34 `tools/verify_*.py` gates pass.
- `clang++ -std=c++17 -fsyntax-only` passes for:
  - `terrain_3d_rd_instance_renderer.cpp`
  - `terrain_3d_rd_mesh_cache.cpp`
  - `terrain_3d_rd_texture_manager.cpp`
  - `terrain_3d_rd_forward_lighting.cpp`
  - `terrain_3d_rd_clipmap_renderer.cpp`
- The only compiler diagnostic is the pre-existing generated godot-cpp `INT64_MIN` literal warning.
- Repository scan finds no remaining sampling-only texture usage on direct Terrain3D RD texture creation paths that supply non-empty initialization data.

## Runtime status

No Godot executable is available in this environment, so the Godot 4.7.2 editor/runtime path was not smoke-tested here. Rebuild the GDExtension and test both an instance thumbnail and an already-authored instance without repainting. If another failure remains, the new renderer/resource diagnostics should expose the next boundary instead of failing through the fallback texture setup.
