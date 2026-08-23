# Batch G — RenderingDevice editor and preview convergence

Batch G completes contract Sections 23-24 and the implementation work for Sections 27-29. It preserves the original C# and GDScript file sets while removing the obsolete RenderingServer/Direct/Hybrid backend and native-proxy control surface.

## Editor rendering

Terrain editor visualization remains part of the direct RD terrain path. `Terrain3DRenderCoordinator` publishes editor decal/reticle state and the direct-RD brush texture in the immutable frame snapshot. The near terrain shader consumes the editor overlay buffer and brush texture and retains region, vertex, instancer-grid, contour, navigation, paint/debug and material-debug visualization. Production presentation is forced to `setRdPresentation(false, false)`; no caller may re-enable Terrain3D native visual or shadow geometry.

## Asset previews

The former RenderingServer preview scenario/viewport/camera/light/mesh-instance renderer was removed from `Terrain3DAssets`. Mesh thumbnails now call the internal `Terrain3D::createRdMeshPreview()` route. A dedicated `Terrain3DRdInstanceRenderer` reuses the existing RD mesh/material ingestion, creates RD-owned color/depth preview attachments and framebuffer-compatible pipelines, supplies preview camera/projection state, draws the selected LOD0 mesh, reads the color attachment back through `RenderingDevice::texture_get_data()`, and releases preview resources deterministically. Preview snapshots explicitly include disabled editor assets so disabling runtime placement does not make their thumbnails disappear. RenderingServer is used only as the permitted render-thread synchronization gateway (`call_on_render_thread`/`force_sync`), not for preview resource ownership or drawing.

## API and project integration

The stale `RenderingServer / RenderingDevice Direct / Hybrid` backend enum and RD native-proxy mode were removed from native storage, ClassDB bindings, settings metadata, profiler UI and editor workbench assumptions. The production route is now singular: Terrain3D-owned RenderingDevice resources submitted through the patched Forward+ bridge.

The reference `.cs` and `.gd` file sets are preserved exactly. The root Visual Studio solution now includes `project/Terrain3D.csproj`; the solution generator owns that relationship so regeneration cannot drop it. Release solution configurations map to the managed `ExportRelease` configuration. SConstruct continues to consume the retained Godot 4.7.1 extension API and recursively enumerates native sources; GDExtension metadata retains Windows/Linux build names and the editor plugin remains registered in `project.godot`.

## Validation evidence

- Batch G static contract: 25 checks covering editor overlays, proxy elimination, preview ownership/readback, scripting parity, and build metadata.
- Native compile: all 80 Terrain3D C++ translation units compile with C++17/GDEXTENSION against generated bindings from the retained Godot 4.7.1 API; zero compiler errors were observed. After the disabled-asset preview correction, the two affected translation units were recompiled successfully.
- Godot executable: supplied `4.7.1.stable.official.a13da4feb` verified.
- Cacheless Godot import/editor execution was attempted. The supplied archive contains no compiled Terrain3D `.so`, so Godot cannot register native Terrain3D classes and dependent GDScripts report missing native types. This is an environment/build-artifact limitation, not accepted as a GDScript validation pass.
- SCons is unavailable and cannot be installed because the environment has no package-network access. A linked extension build is therefore deferred to the environment with SCons/toolchain support.
- .NET/MSBuild/Mono are unavailable, so the managed solution cannot be compiled in this environment. C# source parity and stale-symbol checks are static only.
- `tools/rd.py` now rejects Godot runs whose process exits zero but whose log contains script or GDExtension load errors, preventing false-positive import/editor validation. The repository `full` gate was executed: all static A-G contracts and `git fsck --full --strict` passed; its only failures were missing SCons, missing linked extension for Godot import/editor type registration, and missing .NET.

Runtime/GPU preview correctness is not claimed by this batch validation. Batch H retains final runtime, full-build, dataset, memory/performance and deliverable validation.
