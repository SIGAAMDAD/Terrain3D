# Terrain3D RenderingDevice Reference Feature Matrix

This matrix freezes the functional/rendering state at `rd-alpha-pre-cleanbreak-reference` (`5d9e8b1`) before the clean-break conversion proceeds. It is intentionally strict: **Partial** means some RD implementation exists but the original feature is still incomplete, bridged, proxied, or dependent on RenderingServer.

## Status legend

- **RD** — explicitly owned/submitted by the current RD alpha for the listed scope.
- **Partial** — meaningful RD implementation exists, but parity or ownership is incomplete.
- **Native** — functionality exists only through the current RenderingServer/native path.
- **Missing** — required clean-break implementation is absent.
- **Preserved** — non-rendering subsystem remains present and has not yet been changed by the RD rewrite.

## Core terrain submission

| Feature | Frozen reference implementation | Current clean-break status | Required destination |
|---|---|---:|---|
| Global RenderingDevice access | Acquired for RD alpha through Godot rendering boundary | Partial | Single minimal gateway owned/coordinated by `Terrain3DRdContext` |
| Canonical LOD0 topology | `Terrain3DRdTopology` | RD | Retain and test |
| Canonical outer-ring topology | `Terrain3DRdTopology` | RD | Retain and test |
| 8-byte packed clipmap vertex | `t3dRdClipmapVertex_t`, static assert | RD | Retain and add packing/checksum tests |
| U16/U32 topology indices | RD topology supports width selection | RD | Add explicit boundary/invalid-index tests |
| Persistent terrain VB/IB | `Terrain3DRdClipmapRenderer` | RD | Move lifetime accounting under unified context |
| Patch descriptor buffer | `t3dRdPatch_t` + RD storage buffer | Partial | Add bounds/previous state/dirty-range semantics |
| Clipmap snapping/stitching | Ported into raw RD terrain shaders | Partial | Side-by-side parity validation |
| Near terrain raster pipeline | `terrain_clipmap_near.glsl` | RD | Complete material parity |
| Middle terrain raster pipeline | `terrain_clipmap_middle.glsl` | RD | Complete cache/material parity |
| Far terrain raster pipeline | `terrain_clipmap_far.glsl` | RD | Complete cache/material parity |
| Compute frustum culling | `terrain_cull.glsl` | Partial | CPU reference comparison and bounds validation |
| Indexed indirect terrain draws | Persistent indirect buffers in RD clipmap renderer | RD | Validate command offsets/strides and remove production CPU draw fallback |
| CPU indirect fallback | Existing fallback path | Partial | Keep as debug/reference only |
| Per-view framebuffer state | RD clipmap renderer | Partial | Full resize/MSAA/render-scale/multiview lifecycle |
| Native Forward+ render-list insertion | None | Missing | Godot engine-side submission bridge |

## Frame-state and ownership

| Feature | Frozen reference implementation | Current clean-break status | Required destination |
|---|---|---:|---|
| Central RD context | `Terrain3DRdContext` tracks device and aggregate resource bytes | Partial | Own/coordinate all Terrain3D RD resource classes |
| Deterministic named RD resources | Some resources named/accounted | Partial | All Terrain3D-owned RD objects |
| Triple-buffer immutable snapshots | Single `t3dRdFrameSnapshot_t` guarded by `std::mutex` | Missing | Three slots + atomic published index/generation |
| Mutex-free ordinary render consumption | Snapshot mutex in normal callback | Missing | Atomic immutable publication |
| Texture/material generation tracking | Material generation field exists | Partial | Explicit texture/cache/material generations and stale-snapshot checks |
| Device failure state | Fallback-oriented alpha state | Partial | Visible diagnostic failure with no RS renderer resurrection |
| Deterministic RD reconstruction | Incomplete | Missing | Full teardown/rebuild path |

## Terrain-owned texture/cache resources

| Feature | Frozen reference implementation | Current clean-break status | Required destination |
|---|---|---:|---|
| Height texture arrays | Created by `Terrain3DGeneratedTexture` through RenderingServer, then bridged to RD | Partial | Direct RD texture arrays |
| Control texture arrays | RenderingServer-owned/bridged | Partial | Direct RD ownership |
| Color texture arrays | RenderingServer-owned/bridged | Partial | Direct RD ownership |
| Region map | RD storage data exists, source lifecycle remains tied to existing system | Partial | Explicit RD-owned publication/generation |
| Material albedo arrays | Existing Terrain3D texture resources bridged into RD | Partial | Direct RD ownership for Terrain3D-generated arrays |
| Material normal/surface arrays | Existing resources bridged into RD | Partial | Direct RD ownership where Terrain3D-owned |
| Middle height cache | Existing fork cache/native texture path | Native | Direct RD cache texture |
| Far color cache | Existing fork far-surface cache/native texture path | Native | Direct RD cache texture |
| Far normal cache | Existing fork far-surface cache/native texture path | Native | Direct RD cache texture |
| Far surface cache | Existing fork far-surface cache/native texture path | Native | Direct RD cache texture |
| Displacement output | RD compute exists but exposure/visible texture path still crosses RenderingServer | Partial | Same visible global RD resource, no RS exposure copy/bridge |
| Dirty-region texture updates | Existing generated texture/cache logic | Partial | Explicit RD dirty-layer/subregion updates |
| Texture byte accounting | Aggregate context counters | Partial | Per-category physical byte accounting |

## Terrain material/shader parity

The raw RD shaders already implement a useful subset: control/color/material texture sampling, normal/PBR fields, displacement, simple directional/ambient lighting, and fog. The production shader include tree remains the fuller reference implementation.

| Feature | Frozen reference implementation | Current clean-break status |
|---|---|---:|
| Layer/control lookup | Raw RD shaders | Partial |
| Albedo texture sampling | Raw RD shaders | Partial |
| Color map | Raw RD shaders | Partial |
| Normal mapping | Raw RD shaders | Partial |
| Roughness | Raw RD shaders / RD material layer | Partial |
| Metallic | Raw RD shaders / RD material layer | Partial |
| AO | Raw RD shaders / RD material layer | Partial |
| Cavity | Production include path only | Missing |
| Displacement | Raw RD vertex shaders + current displacement bridge | Partial |
| Texture scale | RD material layer | Partial |
| Texture rotation/detiling rotation | RD material layer/raw shaders | Partial |
| UV/projection behavior | Simplified RD implementation | Partial |
| Texture blending | Simplified control/material path | Partial |
| Dual scaling | Production shader include tree only | Missing |
| Macro variation | Production shader include tree only | Missing |
| World noise | Production shader include tree only | Missing |
| Single/near sampling mode | Simplified RD path | Partial |
| Adaptive sampling | Production include tree only | Missing |
| Weighted sampling | Production include tree only | Missing |
| Distance-adaptive sampling | Production include tree only | Missing |
| Middle cached material behavior | Native production shader system | Native |
| Far cached material behavior | Native production shader system | Native |
| Background/holes | Native production shader system | Native |
| Editor paint/debug material modes | Native ShaderMaterial publication | Native |
| Region/debug overlays | Native ShaderMaterial/editor path | Native |
| Simplified Blinn-Phong path | Production include tree (`blinn_phong.gdshaderinc`) | Missing in raw RD shaders |
| Fog | Raw RD shaders receive environment fog state | Partial |
| Production lighting parameter parity | Native material system | Native |
| Fixed material ABI | `t3dRdMaterialLayer_t` has size assert | Partial |
| Offset assertions / complete GLSL ABI proof | Not comprehensive | Missing |
| Stable material-table indices | Alpha material table exists | Partial |
| Persistent material descriptors | RD alpha has persistent sets for current scope | Partial |

## Regular instancing

| Feature | Frozen reference implementation | Current clean-break status | Required destination |
|---|---|---:|---|
| Imported mesh ingestion to RD geometry | Uses native mesh RIDs | Missing | `Terrain3DRdMeshCache` |
| Static vertex arena | None | Missing | Large persistent RD arena |
| Static index arena | None | Missing | Large persistent RD arena |
| Mesh/surface/LOD handles | Native Mesh/asset references | Missing | Explicit generation-safe handles |
| Instance transform storage | CPU instance stores + MultiMesh upload | Native | Packed RD instance storage |
| Instance color/custom data | MultiMesh buffers | Native | RD instance record |
| Region/cell residency | Existing Terrain3D logical caches | Preserved | Reuse CPU residency, replace GPU publication |
| Coarse instance culling | Existing native visibility/streaming behavior | Native | RD cell/region culling |
| Per-instance frustum/distance culling | Native renderer/MultiMesh behavior | Native | Explicit compute/CPU reference path |
| Instance LOD selection | Existing Terrain3D instancer/native mesh handling | Native | RD LOD handle selection |
| Material grouping | Native material/mesh submission | Native | RD material key grouping |
| Mesh grouping | MultiMesh/native instance submission | Native | RD indirect grouping |
| Instance indexed indirect draws | None | Missing | RD command generation/submission |
| Instance shadows | Native RenderingServer instances | Native | RD/Forward+ bridge shadow participation |
| Runtime streaming updates | Existing MultiMesh publication | Native | Dirty RD instance ranges |
| Editor instance updates | Existing MultiMesh publication | Native | Dirty RD instance ranges |
| MultiMesh-free runtime renderer | `Terrain3DInstancer` creates/populates MultiMesh and MMI RIDs | Missing | No runtime MultiMesh renderer |
| Public MultiMesh import/helper APIs | `add_multimesh`, `build_multimesh_buffer`, etc. | Reference API | Preserve authoring semantics only where required; remove runtime renderer dependency |

## HLOD

| Feature | Frozen reference implementation | Current clean-break status |
|---|---|---:|
| HLOD package/manifest resources | Existing CPU/resource system | Preserved |
| HLOD residency rules | Existing streamer | Preserved |
| HLOD RD mesh handles | Native Mesh RID | Missing |
| HLOD descriptor/state buffer | None | Missing |
| HLOD culling | Native instance visibility/rules | Native |
| HLOD indirect draws | None | Missing |
| HLOD shadows | RenderingServer shadow setting | Native |
| HLOD unload/reload | Existing native instance lifecycle | Native |
| RenderingServer-free HLOD | `instance_create`/`instance_set_base` in streamer | Missing |

## Lighting, shadows, depth, temporal features

| Feature | Frozen reference implementation | Current clean-break status |
|---|---|---:|
| Directional sun shading | Simple explicit RD light vector/color | Partial |
| Ambient/environment | Simple explicit ambient extraction | Partial |
| Fog | Explicit RD fog inputs | Partial |
| Local/clustered lights | Native Forward+ only | Missing |
| Terrain directional shadows | Native shadow-only/full proxy | Native |
| Instance shadows | Native instances | Native |
| HLOD shadows | Native instances | Native |
| Vegetation alpha-scissor shadows | Native material/shadow path | Native |
| Terrain self-shadowing | Native proxy/Forward+ | Native |
| Shadow texture ownership | Godot native renderer | Native |
| Native depth prepass | Cannot be entered from current `PRE_OPAQUE` compositor path | Missing |
| Direct RD terrain depth write | Current RD pass writes depth in callback | Partial |
| Instance/HLOD depth | Native renderer | Native |
| Reversed-Z compatibility | Current RD path intended for Godot 4.x depth conventions | Partial |
| MSAA integration | Framebuffer/pipeline state exists but full lifecycle parity unproven | Partial |
| Internal render scaling | Not fully validated | Partial |
| Motion vectors | None for direct RD geometry | Missing |
| Previous patch state | None | Missing |
| Previous instance/HLOD transforms for velocity | None | Missing |
| TAA/XeSS/DLSS/FSR temporal compatibility | Native proxy/full-native path only | Missing for clean-break direct path |
| GI | Native/full proxy only | Missing for direct clean-break path |
| Decals | Native/full proxy only | Missing for direct clean-break path |
| Reflection probes | Native/full proxy only | Missing for direct clean-break path |
| Lightmap/native baked-light interaction | Existing fork/native path | Native |

## Viewport, multiview and lifecycle

| Feature | Frozen reference implementation | Current clean-break status |
|---|---|---:|
| Editor viewport RD rendering | Compositor effect participates | Partial |
| Runtime viewport RD rendering | Compositor effect participates | Partial |
| Multiple views | Per-view state exists | Partial |
| Multiview/stereo correctness | Alpha scaffolding only | Partial |
| Resize | Some framebuffer recreation support | Partial |
| Render-scale changes | Incomplete validation | Partial |
| MSAA changes | Incomplete validation | Partial |
| Multiple cameras | Not comprehensively validated | Partial |
| Resource invalidation/stale RID recovery | Incomplete | Missing |

## Editor rendering and previews

| Feature | Frozen reference implementation | Current clean-break status |
|---|---|---:|
| Paint visualization | Native material parameters/includes | Native |
| Brush overlays | Native material/editor path | Native |
| Region highlighting/grid | Native material/editor path | Native |
| Selection/debug overlays | Native material/editor path | Native |
| Terrain wire/debug modes | Native material system | Native |
| Texture/material feedback | Native ShaderMaterial publication | Native |
| Terrain-owned gizmo rendering | Existing editor/native path | Native |
| Mesh thumbnails/previews | RenderingServer/native mesh path | Native |
| Dedicated RD preview framebuffer | None | Missing |
| Preview readback/resource lifecycle | None | Missing |

## Non-rendering systems frozen for regression parity

These systems are present in the reference and must remain functionally intact while rendering ownership changes:

| System | Reference status |
|---|---:|
| Height collision generation/residency | Preserved |
| Terrain sampling | Preserved |
| Region repository/persistence | Preserved |
| Runtime page store | Preserved |
| Region residency | Preserved |
| Active-height residency | Preserved |
| CPU region cache | Preserved |
| GPU region cache logical system | Preserved; visible texture ownership still requires RD conversion |
| Instance region cache | Preserved |
| Middle height cache logical system | Preserved; GPU texture ownership requires RD conversion |
| Editing/map edit service | Preserved |
| Undo/redo/editor authoring | Preserved |
| Import/export | Preserved |
| Erosion/landform tooling | Preserved |
| HLOD authoring/baking | Preserved |
| Lightmap streamer/baking tools | Preserved |
| Node streaming | Preserved |

## Public clean-break migration hazards

The following are intentionally present in the frozen API inventory and must be resolved explicitly rather than accidentally retained:

- `Terrain3D.render_backend`
- RD native proxy mode APIs/settings
- RenderingServer / Direct / Hybrid backend enums
- native shadow proxy presentation controls
- diagnostics exposing fallback/proxy state
- `Terrain3DInstancer` MultiMesh helper/import methods where their semantics can be retained without retaining a MultiMesh runtime renderer
- shadow properties typed directly as `RenderingServer::ShadowCastingSetting`
- editor metadata/tooltips describing fallback/hybrid/native proxy behavior

## Completion interpretation

The frozen reference proves that the present branch is a **terrain RD alpha plus native compatibility renderer**, not the clean-break target. Terrain raster topology/culling/indirect submission is the strongest completed slice. Texture ownership, material parity, regular instancing, HLOD, shadows, Forward+ integration, editor rendering, and temporal/native feature participation remain later implementation blocks.
