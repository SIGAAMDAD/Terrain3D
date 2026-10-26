# Wave E (CS09): dirty-page invalidation, GPU retirement safety, material sampling correctness

This is a follow-on integration-hardening wave after the planned CS00-CS08 program.
It is **not** shipping activation. The canonical CS02 material evaluator and CS05
engine-side descriptor/fence wiring still need completion.

## Source changes

- `rendering/rd/vt/runtime/terrain_3d_composed_surface_cache.*`: targeted
  `beginInvalidationHot(page)` for editor updates, token-checked
  `retirementPendingHot`, `cancelRetirementHot` for a rejected GPU transition,
  cancellation of pre-submit page loads and queued old-revision demand. An edit
  can invalidate a resident page even without cache pressure. It does not
  allow early slot reuse.
- `rendering/rd/vt/runtime/terrain_3d_composed_surface_cache.cpp`:
  `t3d_cs_find_page_location` locates the old sidecar's logical page even when
  the new authoring page has a different composition revision. Ordinary
  `t3d_cs_find_page` still requires an exact revision match.
- `authoring/vt/integration/terrain_3d_composed_surface_resident_bridge.*`:
  fixed-capacity bridge translating a set of dirty cooked pages to retirement
  tickets for resident old pages. Two passes reject insufficient capacity or
  uploading/retiring pages before any residency mutation. The accepted pass
  cancels stale queued/reading/ready work, and returns resident retirements.
  The GPU owner must process those tickets at a safe publication boundary and
  confirm physical use has ended before slot reuse. The editor must not
  demand the old sidecar revision during the transition.
- `rendering/rd/vt/gpu/terrain_3d_composed_surface_gpu.cpp`: validates that
  an exact retirement token is still owned by the cache before modifying GPU
  page mappings. A delayed retirement token cannot retire a reused slot.
- `project/addons/terrain_3d/rd/terrain_composed_surface_sampling.glslinc`:
  takes **caller-computed** derivatives rather than evaluating derivatives
  inside residency-dependent control flow. The sampler now returns ORM alpha
  (`ao_affect`), consistent with the BC7 surface format.
- `terrain_clipmap_fragment_inc.glslinc`: computes composed texture gradients
  before any control holes, misses or tier branches. Color and DNR pass the
  same gradients; color forwards composed AO influence to lighting.
- `diagnostics/tests/composed_surface`: extends CPU serialization/residency
  integration cases and shader/static contract assertions.

## Verification

Tests performed with `clang++ -std=c++17 -Wall -Wextra -Werror`:

- CS01 runtime and editor contract: pass.
- CS03/CS04/CS09 full sidecar, bounded residency, old/new revision bridge,
  uploading rejection, queued request discard and retirement token handling:
  pass, including AddressSanitizer and UndefinedBehaviorSanitizer.
- CS05 GPU hash table standalone: pass.
- CS07 edit journal and recook bridge: pass.
- CS08 CPU retirement gates/acceptance parser: pass.
- CS02 10,000 material math cases: pass.
- Shader include and 294-variant static topology checks: pass.

## Integration/activation limits

The supplied archive lacks the extension root SCons source set and generated
Godot C++ headers; glslang, spirv-val and Godot/Vulkan are unavailable. The
new editor-only `.cpp` must be registered in the consuming editor build.
There is no fully implemented source material producer matching the canonical
raster evaluation. The composed GPU owner is still not connected to shared
VT transfer/fence/descriptor ownership. No Vulkan SPIR-V compilation, visual
parity, GPU timing or production enablement has been performed. Preserve the
legacy middle-composite path and default-disabled composed specialization.
