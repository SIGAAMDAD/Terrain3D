# Terrain3D RenderingDevice Branch — Completion Contract

Target branch:

`experiment/rendering-device`

Target engine:

`Godot 4.7.1`

Target outcome:

A complete clean-break RenderingDevice implementation of the current Terrain3D fork. The branch must retain the functional feature set of the original fork while eliminating Terrain3D's RenderingServer-based rendering architecture.

**The branch is not complete until every mandatory item below is implemented and verified.**

---

## 1. Establish the exact reference feature set

- [ ] Preserve an untouched reference to the current original/main fork.
- [ ] Generate a complete file manifest of the original fork.
- [ ] Generate a complete file manifest of `experiment/rendering-device`.
- [ ] Restore every accidentally omitted repository file.
- [ ] Restore missing:
  - [ ] `.cs` files
  - [ ] `.csproj` files
  - [ ] `.sln` / Visual Studio project files where part of the repo
  - [ ] GDScript files
  - [ ] editor plugin files
  - [ ] project settings
  - [ ] addon metadata
  - [ ] icons/resources
  - [ ] shader includes
  - [ ] tests
  - [ ] documentation
  - [ ] build scripts
  - [ ] SCons files
  - [ ] GDExtension configuration
  - [ ] import metadata that must be retained
- [ ] Diff the public classes, properties, methods, signals, resources and editor controls against the original branch.
- [ ] Build a feature matrix mapping every original rendering feature to its RD implementation.
- [ ] Do not remove a feature merely because its existing implementation depends on RenderingServer.

---

# 2. Branch and repository hygiene

- [ ] Work only on `experiment/rendering-device`.
- [ ] Keep the original main fork untouched.
- [ ] Configure Git identity:
  - `Noah Van Til`
  - `nyvantil@gmail.com`
- [ ] No OpenAI-authored commits.
- [ ] Remove abandoned intermediate RD branches.
- [ ] Remove generated caches/build junk from version control.
- [ ] Remove machine-specific paths.
- [ ] Keep required dependency/build files reproducible from a fresh clone.
- [ ] Maintain a clean worktree before final packaging.
- [ ] Run `git diff --check`.
- [ ] Run `git fsck --full --strict`.

---

# 3. RenderingServer elimination policy

Terrain3D rendering must not use RenderingServer for scene submission.

Remove Terrain3D use of RenderingServer for:

- [ ] mesh creation
- [ ] mesh surface creation
- [ ] MultiMesh creation
- [ ] MultiMesh population
- [ ] render instances
- [ ] terrain geometry
- [ ] clutter instances
- [ ] HLOD instances
- [ ] terrain materials
- [ ] shadow proxies
- [ ] Hybrid rendering
- [ ] compatibility terrain rendering
- [ ] generated terrain textures
- [ ] generated cache textures
- [ ] terrain GPU caches
- [ ] editor terrain geometry
- [ ] debug terrain geometry
- [ ] asset preview geometry
- [ ] viewport-based Terrain3D thumbnail rendering where replaceable by RD
- [ ] material parameter submission
- [ ] terrain texture bridging

The only permitted RenderingServer dependency is the minimal Godot GDExtension gateway required to obtain the engine's global RenderingDevice if Godot exposes no direct equivalent.

That gateway:

- [ ] may obtain the global `RenderingDevice`
- [ ] must not create Terrain3D geometry
- [ ] must not create Terrain3D instances
- [ ] must not own Terrain3D textures
- [ ] must not publish Terrain3D materials
- [ ] must not serve as a hidden fallback renderer

Final static audit:

- [ ] enumerate every `RenderingServer` symbol in the repository
- [ ] manually justify every remaining occurrence
- [ ] zero unexplained RenderingServer rendering calls

---

# 4. Unified RD ownership

Implement a single explicit GPU ownership layer.

`Terrain3DRdContext` must own or coordinate:

- [ ] global RenderingDevice
- [ ] samplers
- [ ] shaders
- [ ] raster pipelines
- [ ] compute pipelines
- [ ] vertex formats
- [ ] vertex buffers
- [ ] index buffers
- [ ] storage buffers
- [ ] uniform buffers
- [ ] uniform sets
- [ ] indirect buffers
- [ ] culling buffers
- [ ] shadow resources
- [ ] framebuffer-dependent resources
- [ ] generated terrain textures
- [ ] terrain cache textures
- [ ] material tables
- [ ] mesh arenas
- [ ] diagnostics

Rules:

- [ ] deterministic ownership
- [ ] deterministic destruction
- [ ] no double frees
- [ ] no hidden ownership in clipmap batches
- [ ] no per-frame GPU object creation in normal traversal
- [ ] named resources via `set_resource_name()`

---

# 5. Threading and frame-state model

Replace cross-thread mutable renderer state with immutable publication.

- [ ] triple-buffer render snapshots
- [ ] atomic published snapshot index/generation
- [ ] no render-thread mutex on ordinary frame consumption
- [ ] main/streaming thread builds next snapshot
- [ ] render thread reads only a completed snapshot
- [ ] GPU resource creation/destruction occurs on the appropriate render thread
- [ ] no mutable `Ref<>` ownership crossing unsafe boundaries
- [ ] generation IDs for texture/cache/material changes
- [ ] stale snapshot detection

---

# 6. Terrain clipmap topology

Finish explicit RD clipmap ownership.

- [ ] canonical LOD0 topology
- [ ] canonical outer-ring topology
- [ ] topology uploaded once
- [ ] no duplicated vertex topology per LOD state
- [ ] 8-byte packed clipmap vertex record
- [ ] implicit terrain Y
- [ ] part/stitch metadata packed
- [ ] per-draw/per-instance LOD state rather than per-vertex duplication
- [ ] automatic `uint16` indices when possible
- [ ] `uint32` fallback
- [ ] bounds validation
- [ ] upload checksums against CPU topology
- [ ] no VB/IB creation during camera movement

---

# 7. Patch representation

Replace MultiMesh-style terrain state entirely.

Each patch descriptor should contain only data actually needed by the renderer:

- [ ] origin
- [ ] scale
- [ ] LOD
- [ ] tier
- [ ] stitch state
- [ ] flags
- [ ] bounds
- [ ] previous-frame state where velocity requires it

Behavior:

- [ ] update only dirty patch ranges
- [ ] camera snap updates relevant records only
- [ ] no descriptor recreation
- [ ] no topology recreation

---

# 8. Near / middle / far terrain rendering

Maintain the fork's tiered renderer.

- [ ] Near pipeline
- [ ] Middle pipeline
- [ ] Far pipeline
- [ ] tier selection parity with original fork
- [ ] correct boundary transitions
- [ ] correct clipmap snapping
- [ ] correct stitching
- [ ] no cracks
- [ ] correct camera-relative placement
- [ ] support existing world-coordinate range

---

# 9. Complete terrain material port

Do not leave `Terrain3DMaterial` as a native-material publication layer.

Port all production-relevant material functionality into explicit RD shader/data structures.

At minimum audit and preserve:

- [ ] terrain texture arrays
- [ ] control maps
- [ ] height maps
- [ ] color maps
- [ ] region maps
- [ ] layer IDs
- [ ] albedo
- [ ] normal mapping
- [ ] roughness
- [ ] metallic
- [ ] AO
- [ ] cavity
- [ ] displacement
- [ ] texture scale
- [ ] texture rotation
- [ ] UV/projection behavior
- [ ] texture blending
- [ ] dual scaling
- [ ] macro variation
- [ ] world noise
- [ ] near sampling modes
- [ ] adaptive sampling
- [ ] weighted sampling
- [ ] distance-adaptive sampling
- [ ] middle caches
- [ ] far color caches
- [ ] far normal caches
- [ ] far surface caches
- [ ] background/holes
- [ ] editor paint visualization
- [ ] debug visualization
- [ ] region borders where present
- [ ] navigation/debug overlays where part of Terrain3D
- [ ] existing simplified Blinn-Phong path
- [ ] fog/environment integration
- [ ] production lighting parameters

Build fixed GPU ABI structs.

- [ ] static assertions for CPU sizes
- [ ] static assertions for offsets
- [ ] GLSL layout matches C++
- [ ] stable material-table indices
- [ ] persistent descriptors

---

# 10. Generated textures and terrain GPU caches

All Terrain3D-owned generated textures must be direct RD resources.

- [ ] height texture arrays
- [ ] control arrays
- [ ] color arrays
- [ ] region/cache textures
- [ ] middle cache
- [ ] far cache
- [ ] generated displacement resources
- [ ] editing-related generated resources

Requirements:

- [ ] no `texture_get_rd_texture()` bridge for Terrain3D-owned resources
- [ ] no duplicate RS + RD copy
- [ ] explicit format
- [ ] explicit usage flags
- [ ] explicit byte accounting
- [ ] dirty-region updates
- [ ] deterministic recreation when dimensions/formats change

---

# 11. Terrain compute operations

Audit every existing compute path.

- [ ] displacement
- [ ] terrain editing compute
- [ ] cache generation
- [ ] authoring/readback operations
- [ ] any normal/height processing
- [ ] memory barriers
- [ ] synchronization
- [ ] local vs global RD usage

Unify utilities where practical.

- [ ] no parallel incompatible RD abstraction
- [ ] correct render-device for visible resources
- [ ] local devices only where isolation/readback actually requires them

---

# 12. Terrain culling

Complete and validate the compute culling path.

- [ ] frustum culling
- [ ] CPU reference culler
- [ ] GPU/CPU visible-set comparison mode
- [ ] bounds validation
- [ ] indirect command bounds checking
- [ ] near/middle/far bucketing
- [ ] topology bucketing
- [ ] no out-of-range indirect records

Optional only after parity:

- [ ] GPU tier selection
- [ ] GPU LOD selection
- [ ] Hi-Z occlusion
- [ ] visibility history

---

# 13. Indirect terrain submission

- [ ] indexed indirect draws
- [ ] persistent indirect command buffers
- [ ] no CPU per-patch draw loop in production path
- [ ] explicit fallback/debug direct-draw implementation
- [ ] indirect counters in diagnostics
- [ ] validated command offsets/strides

---

# 14. Full regular-instancer RD conversion

This is mandatory for completion.

Implement `Terrain3DRdMeshCache`.

Mesh ingestion:

- [ ] read imported mesh surfaces once
- [ ] extract geometry
- [ ] extract required vertex attributes
- [ ] pack into Terrain3D RD format
- [ ] convert indices to 16-bit where possible
- [ ] preserve 32-bit where required
- [ ] store surface/material ranges

GPU geometry:

- [ ] large static vertex arena
- [ ] large static index arena
- [ ] explicit capacity policy
- [ ] arena growth policy
- [ ] no allocation-per-small-mesh
- [ ] mesh handles
- [ ] LOD handles
- [ ] surface handles

Instance data:

- [ ] packed position
- [ ] packed/appropriate rotation
- [ ] packed scale
- [ ] mesh handle
- [ ] material/variant handle
- [ ] color
- [ ] custom data required by original instancer
- [ ] flags
- [ ] region/cell handle
- [ ] previous transform if motion vectors require it

Rendering:

- [ ] coarse region/cell culling
- [ ] per-instance culling where useful
- [ ] distance visibility
- [ ] LOD selection
- [ ] material grouping
- [ ] mesh grouping
- [ ] indirect-command generation
- [ ] grouped indexed indirect drawing

Functional parity:

- [ ] transforms
- [ ] colors
- [ ] custom data
- [ ] visibility
- [ ] LODs
- [ ] shadows
- [ ] editor updates
- [ ] runtime streaming
- [ ] region residency

No MultiMesh remains.

---

# 15. Full HLOD RD conversion

Reuse the same mesh cache.

- [ ] no HLOD RenderingServer instances
- [ ] HLOD package resolves to RD mesh handles
- [ ] HLOD descriptor buffer
- [ ] HLOD transform/state buffer
- [ ] HLOD residency remains CPU-controlled
- [ ] HLOD visibility culling
- [ ] HLOD LOD/distance rules
- [ ] HLOD indirect draws
- [ ] shadow participation
- [ ] correct unload/reload behavior
- [ ] separate arena class only if actual lifetime differences justify it

---

# 16. Instance/HLOD material system

Translate the material subset actually used by the fork.

- [ ] imported material extraction
- [ ] BaseMaterial3D parameters used by assets
- [ ] albedo texture
- [ ] normal texture
- [ ] ORM/PBR fields used by project
- [ ] alpha scissor
- [ ] culling mode
- [ ] double-sided behavior
- [ ] vertex color
- [ ] instance color
- [ ] roughness
- [ ] metallic
- [ ] AO
- [ ] emissive if supported by original fork
- [ ] texture filtering
- [ ] sampler policy
- [ ] shadow alpha behavior
- [ ] material-table deduplication

No native material RID is required at runtime.

---

# 17. Shadows

Remove the native shadow proxy completely.

Implement RD directional terrain/instance/HLOD shadows.

Required:

- [ ] terrain shadow pass
- [ ] instance shadow pass
- [ ] HLOD shadow pass
- [ ] shadow-caster setting parity
- [ ] shadow texture ownership
- [ ] light-space matrices
- [ ] bias
- [ ] filtering
- [ ] cascade/range strategy matching project requirements
- [ ] vegetation alpha-scissor shadows where applicable
- [ ] terrain self-shadowing
- [ ] dynamic sun movement

Evaluate specialized heightfield terrain shadows where they reduce cost without losing required behavior.

No RenderingServer proxy geometry.

---

# 18. Lighting

Reproduce the functional lighting behavior required by the original fork.

- [ ] directional sun
- [ ] ambient/environment lighting
- [ ] baked terrain lighting data
- [ ] AO/cavity
- [ ] fog
- [ ] lightmap-related behavior required by the fork
- [ ] instance material lighting
- [ ] HLOD lighting
- [ ] shadow receiving

If arbitrary Godot local-light parity is required by the original branch:

- [ ] implement the required explicit local-light data path or
- [ ] implement the engine-side Forward+ submission bridge

Do not silently drop local lights.

---

# 19. Forward+ integration decision

For **exact native Forward+ feature parity**, implement a small Godot engine-side submission bridge.

The bridge must support:

- [ ] Terrain3D-owned RD buffer references
- [ ] index ranges
- [ ] vertex formats
- [ ] pipeline/material key
- [ ] world transform or patch index
- [ ] AABB
- [ ] render layers
- [ ] depth flags
- [ ] shadow flags
- [ ] previous transform/state
- [ ] velocity
- [ ] native opaque ordering

Then provide:

- [ ] native depth prepass participation
- [ ] native clustered light participation
- [ ] native shadow-list participation where selected
- [ ] native motion vectors
- [ ] native GI interaction
- [ ] decals
- [ ] reflection probes

If this bridge is part of the branch target, include the required Godot patch/source files in the repository.

---

# 20. Motion vectors

Required for temporal rendering compatibility.

- [ ] previous view-projection
- [ ] previous terrain patch state
- [ ] previous instance transforms
- [ ] previous HLOD transforms
- [ ] velocity attachment output
- [ ] camera-origin changes handled correctly
- [ ] clipmap snapping does not create bogus velocities

Validate with:

- [ ] TAA
- [ ] XeSS
- [ ] DLSS where environment permits
- [ ] FSR temporal path where applicable

---

# 21. Depth behavior

- [ ] terrain depth write
- [ ] instance depth
- [ ] HLOD depth
- [ ] alpha-scissor depth
- [ ] correct reversed-Z handling
- [ ] MSAA
- [ ] internal resolution scaling
- [ ] multiview
- [ ] depth consumers receive correct terrain data

If compositor timing cannot provide original feature parity, use the Forward+ bridge instead of pretending parity exists.

---

# 22. Multiview and viewport lifecycle

- [ ] multiple views
- [ ] stereo-safe matrices
- [ ] framebuffer cache keyed by actual attachment state
- [ ] resize
- [ ] render scaling
- [ ] MSAA changes
- [ ] viewport recreation
- [ ] editor viewport
- [ ] runtime viewport
- [ ] multiple cameras
- [ ] resource invalidation without stale RIDs

---

# 23. Editor rendering

The editor must remain usable without native Terrain3D geometry.

Port:

- [ ] terrain painting visualization
- [ ] brush overlays
- [ ] region highlighting
- [ ] selection
- [ ] debug overlays
- [ ] terrain wireframe/debug modes if present
- [ ] editor camera rendering
- [ ] texture/material update feedback
- [ ] visibility controls
- [ ] editor gizmo-related rendering owned by Terrain3D

No hidden native terrain proxy.

---

# 24. Asset thumbnail/preview rendering

Remove the explicit RenderingServer thumbnail path.

Implement either:

- [ ] dedicated RD preview framebuffer
- [ ] RD mesh upload/cache reuse
- [ ] preview camera matrices
- [ ] preview lighting
- [ ] image readback

or another implementation that does not recreate the native Terrain3D rendering architecture.

Validate:

- [ ] mesh thumbnails
- [ ] material thumbnails where used
- [ ] editor preview refresh
- [ ] no leaked preview resources

---

# 25. Collision

Preserve original collision behavior independently from rendering.

- [ ] height collision generation
- [ ] collision residency
- [ ] update scheduling
- [ ] editor collision
- [ ] runtime collision
- [ ] no dependency on RenderingServer callbacks
- [ ] no frame-order regression caused by renderer removal

---

# 26. Navigation and non-rendering Terrain3D systems

Audit all original functionality and preserve it.

- [ ] navigation integration
- [ ] terrain sampling
- [ ] raycasts/helpers
- [ ] region management
- [ ] editing
- [ ] undo/redo
- [ ] import/export
- [ ] terrain data save/load
- [ ] region residency
- [ ] active-height residency
- [ ] CPU caches
- [ ] GPU region caches
- [ ] baking workflows
- [ ] lightmap-related workflows
- [ ] editor tools
- [ ] runtime tools

Renderer rewrite must not break unrelated systems.

---

# 27. C# integration

Restore and validate every C# wrapper/project file present in the reference repo.

- [ ] enumerate original `.cs`
- [ ] enumerate RD branch `.cs`
- [ ] restore missing files
- [ ] update renamed enums/classes
- [ ] remove backend-switch APIs that no longer exist
- [ ] update shadow enums
- [ ] update rendering API exposure
- [ ] compile C# projects
- [ ] resolve stale generated bindings
- [ ] no references to deleted native APIs

---

# 28. GDScript integration

- [ ] restore every original `.gd`
- [ ] update backend/proxy UI removal
- [ ] update diagnostics
- [ ] update editor plugin code
- [ ] update profiler controls
- [ ] update HLOD tooling
- [ ] update instance tooling
- [ ] update shader/material tooling
- [ ] eliminate stale native-renderer assumptions
- [ ] parse every `.gd` with Godot 4.7.1
- [ ] cacheless editor startup with zero script errors

---

# 29. Project files and build system

Restore and validate:

- [ ] `project.godot`
- [ ] `.gdextension`
- [ ] SConstruct
- [ ] build helpers
- [ ] Godot-cpp configuration
- [ ] exact Godot 4.7.1 extension API
- [ ] Visual Studio solution/project files where maintained
- [ ] C# solution/project files
- [ ] Linux build
- [ ] Windows/MSVC build definitions
- [ ] include paths
- [ ] source lists
- [ ] shader/resource packaging
- [ ] editor plugin registration

Fresh checkout must contain everything needed to build.

---

# 30. Memory architecture

Track physical RD ownership explicitly.

Counters:

- [ ] topology VB bytes
- [ ] topology IB bytes
- [ ] static mesh vertex-arena bytes
- [ ] static mesh index-arena bytes
- [ ] material-table bytes
- [ ] instance bytes
- [ ] HLOD bytes
- [ ] patch bytes
- [ ] indirect bytes
- [ ] UBO bytes
- [ ] storage-buffer bytes
- [ ] scratch bytes
- [ ] Terrain3D-owned texture bytes
- [ ] shadow bytes

Steady-state requirements:

- [ ] no VB creation
- [ ] no IB creation
- [ ] no shader creation
- [ ] no pipeline creation
- [ ] no uniform-set creation
- [ ] no mesh allocation
- [ ] no accidental arena growth

during ordinary stable traversal after warmup.

---

# 31. Diagnostics

Expose:

- [ ] GPU frame ID
- [ ] snapshot generation
- [ ] bytes uploaded/frame
- [ ] buffer update calls
- [ ] VB creates/frees
- [ ] IB creates/frees
- [ ] pipelines creates/rebuilds
- [ ] uniform-set creates/rebuilds
- [ ] terrain patch count
- [ ] visible terrain patches
- [ ] static instance count
- [ ] visible instances
- [ ] HLOD count
- [ ] visible HLODs
- [ ] indirect command count
- [ ] compute dispatch count
- [ ] topology memory
- [ ] mesh-arena memory
- [ ] texture memory
- [ ] last successful pass
- [ ] last successful GPU frame
- [ ] failure state

---

# 32. Device failure handling

- [ ] failed resource creation stops dependent draws
- [ ] no invalid RID cascades
- [ ] explicit renderer error state
- [ ] deterministic RD teardown
- [ ] deterministic RD reconstruction
- [ ] viewport reconfiguration recovery
- [ ] device/resource invalidation recovery where Godot exposes enough information
- [ ] no fallback to removed RenderingServer renderer

A failure should fail visibly and diagnostically, not silently resurrect the old backend.

---

# 33. Automated tests

Add RD-specific tests.

CPU-side:

- [ ] topology packing
- [ ] index-width selection
- [ ] invalid index rejection
- [ ] patch descriptors
- [ ] tier grouping
- [ ] CPU frustum reference
- [ ] indirect command construction
- [ ] material ABI sizes
- [ ] material offsets
- [ ] mesh-arena allocation
- [ ] mesh-handle validity
- [ ] packed transforms
- [ ] HLOD descriptors
- [ ] resource accounting
- [ ] snapshot publication
- [ ] generation changes
- [ ] renderer failure state

GPU-capable where practical:

- [ ] shader compilation
- [ ] compute culling
- [ ] indirect output bounds

---

# 34. Static audit before build

Search for:

- [ ] `RenderingServer`
- [ ] `MultiMesh`
- [ ] `mesh_create`
- [ ] `instance_create`
- [ ] `instance_set_base`
- [ ] `material_set_param`
- [ ] native shadow proxy
- [ ] Hybrid
- [ ] old backend enum
- [ ] stale `RID` ownership
- [ ] TODO
- [ ] FIXME
- [ ] stub
- [ ] placeholder
- [ ] empty implementation
- [ ] deprecated 4.x APIs
- [ ] stale 4.5/4.6 assumptions
- [ ] missing includes
- [ ] stale C# names
- [ ] stale GDScript names

Every result must either be removed or explicitly justified.

---

# 35. C++ validation

Against the actual Godot 4.7.1 API:

- [ ] clean binding generation
- [ ] all Terrain3D translation units compile
- [ ] extension links
- [ ] no unresolved symbols
- [ ] no compiler errors
- [ ] remove new warnings caused by this branch
- [ ] syntax-check affected files separately where useful
- [ ] Linux x86-64 debug build
- [ ] release/template build where environment permits

---

# 36. Shader validation

Compile every raw RD shader through Godot 4.7.1.

- [ ] terrain near
- [ ] terrain middle
- [ ] terrain far
- [ ] terrain depth
- [ ] terrain shadow
- [ ] terrain culling
- [ ] instance surface
- [ ] instance depth
- [ ] instance shadow
- [ ] instance culling
- [ ] HLOD shader variants where required
- [ ] editor overlay shaders
- [ ] preview shaders

No GLSL/SPIR-V compile errors.

---

# 37. GDScript validation

- [ ] enumerate all GDScript files
- [ ] all pass Godot 4.7.1 parsing
- [ ] clean `.godot` cache
- [ ] full reimport
- [ ] no hard startup errors
- [ ] editor plugin initializes
- [ ] Terrain3D scenes/resources load

Warnings must be reviewed rather than ignored blindly.

---

# 38. C# validation

Where the original repo contains C# support:

- [ ] solution loads
- [ ] projects compile
- [ ] wrappers match native API
- [ ] no missing symbols
- [ ] no deleted backend/proxy properties referenced
- [ ] no stale enum values

---

# 39. Runtime visual validation

On actual Forward+/RD-capable hardware:

Terrain:

- [ ] appears
- [ ] correct scale
- [ ] correct orientation
- [ ] correct winding
- [ ] correct height
- [ ] correct material
- [ ] correct region selection
- [ ] correct holes
- [ ] correct displacement
- [ ] no seams
- [ ] no cracks
- [ ] correct near/middle/far transitions
- [ ] stable camera snapping

Instances:

- [ ] correct mesh
- [ ] correct position
- [ ] correct rotation
- [ ] correct scale
- [ ] correct color
- [ ] correct material
- [ ] correct LOD
- [ ] correct visibility

HLOD:

- [ ] correct loading
- [ ] correct unloading
- [ ] correct transitions
- [ ] correct transforms
- [ ] correct materials

---

# 40. Rendering feature validation

- [ ] directional sunlight
- [ ] shadows
- [ ] baked/lightmap behavior used by fork
- [ ] AO/cavity
- [ ] fog
- [ ] MSAA
- [ ] internal render scaling
- [ ] editor camera
- [ ] runtime camera
- [ ] multiple cameras
- [ ] multiview
- [ ] motion vectors if required
- [ ] GI if part of parity requirement
- [ ] decals if part of parity requirement
- [ ] reflection probes if part of parity requirement

---

# 41. Streaming validation

Traverse enough terrain to force:

- [ ] region load
- [ ] region unload
- [ ] cache replacement
- [ ] instance residency changes
- [ ] HLOD residency changes
- [ ] clipmap snaps
- [ ] editor/runtime transitions

Check:

- [ ] no crashes
- [ ] no stale handles
- [ ] no use-after-free
- [ ] no invalid RIDs
- [ ] no growing resource counts after stabilization
- [ ] no unexplained hitch from GPU allocation

---

# 42. Memory validation

Capture at initialization, warmup and after long traversal:

- [ ] live VB count
- [ ] live IB count
- [ ] VB bytes
- [ ] IB bytes
- [ ] mesh-arena used/capacity
- [ ] instance bytes
- [ ] HLOD bytes
- [ ] terrain texture bytes
- [ ] shadow bytes
- [ ] scratch bytes
- [ ] descriptor/uniform bytes
- [ ] allocation count
- [ ] grow count
- [ ] free count

Steady state must plateau.

---

# 43. Performance validation

Measure separately:

- [ ] main-thread terrain preparation
- [ ] render-thread Terrain3D CPU
- [ ] terrain culling GPU time
- [ ] terrain raster GPU time
- [ ] instance culling GPU time
- [ ] instance raster GPU time
- [ ] HLOD GPU time
- [ ] shadow GPU time
- [ ] bytes uploaded/frame
- [ ] number of update calls
- [ ] number of indirect draws
- [ ] camera-snap spikes
- [ ] streaming spikes
- [ ] VRAM usage

Do not claim an optimization without identifying the work actually removed.

---

# 44. Original-feature parity audit

Before completion, compare the experimental branch side-by-side with the original fork.

For **every public feature**, record:

```text
Original feature
Original implementation
RD implementation
Validated?
Difference?
```

No entry may remain:

```text
missing
temporary
proxy
fallback
TODO
```

unless explicitly excluded by the requested branch specification.

---

# 45. Final independent audit

After the first full validation pass:

- [ ] perform a second source-tree audit
- [ ] search for old symbols again
- [ ] inspect all changed ownership boundaries
- [ ] inspect destructors/teardown
- [ ] inspect renderer-thread calls
- [ ] inspect cache invalidation
- [ ] inspect editor/runtime separation
- [ ] inspect failure paths
- [ ] inspect build manifests
- [ ] inspect repository completeness
- [ ] rerun affected builds/tests after every discovered correction

---

# 46. Final Git state

Only after all implementation/validation work:

- [ ] squash failed intermediate implementation commits where appropriate
- [ ] preserve meaningful history
- [ ] author/committer is Noah Van Til
- [ ] branch is exactly `experiment/rendering-device`
- [ ] clean worktree
- [ ] no unrelated branches
- [ ] no OpenAI attribution
- [ ] no generated junk
- [ ] no machine-local configuration
- [ ] tag/reference original baseline if useful

---

# 47. Deliverable verification

Create the repository archive only after completion.

Then:

- [ ] extract the ZIP into a completely separate directory
- [ ] verify branch
- [ ] verify commit SHA
- [ ] verify Git history
- [ ] verify author/committer identity
- [ ] verify required files exist
- [ ] verify `.cs` files exist
- [ ] verify project files exist
- [ ] verify shaders exist
- [ ] verify dependency/build files exist
- [ ] verify extension can be rebuilt
- [ ] run `git diff --check`
- [ ] run `git fsck --full --strict`
- [ ] verify archive SHA-256

---

# Definition of Done

The branch is **not finished** because:

- terrain renders,
- shaders compile,
- C++ builds,
- the editor starts,
- or a major milestone was completed.

It is finished only when:

1. `experiment/rendering-device` is a complete repository.
2. The original fork's required public feature set has an RD implementation.
3. Terrain rendering is RD-owned.
4. Terrain textures/caches are RD-owned.
5. Normal instancing is RD-owned.
6. HLOD is RD-owned.
7. Required shadows are RD-owned or integrated through the explicit Forward+ bridge.
8. Required materials are RD implementations.
9. Editor rendering/tools work without a hidden native terrain renderer.
10. C++, GDScript and C# integration are valid.
11. There is no Terrain3D RenderingServer fallback/proxy architecture left.
12. Runtime resource lifetime is stable.
13. Real GPU validation has been performed wherever the environment permits it.
14. The repository passes the parity matrix.
15. The actual repository ZIP is produced and independently revalidated.

**If any mandatory item remains unfinished, continue implementing. Do not hand off an intermediate repository as the requested completed branch.**