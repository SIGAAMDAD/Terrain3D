# Terrain3D Inspector UX (current VT/GPU branch)

The Inspector keeps Godot's native property controls, native setters, property hints,
existing scene/resource storage, and the existing Terrain3D editor toolbar. No
new serialized runtime profile or duplicate configuration authority is introduced.

## Native Inspector layout

### Terrain3D

- **Dataset & Editor:** data directory, editor execution, dataset status; region
  lightmap bake configuration is the only nested section in this group.
- **Terrain Resources:** material, texture/mesh assets, Autoshader/foliage
  profiles and lighting target.
- **Quality & Effective Profile:** requested terrain/VT quality presets and
  effective read-only profile state in one section.
- **Terrain Rendering:** clipmap geometry, Near/Middle/Far shader detail,
  shadows/directional-light policy, tessellation and displacement.
- **Streaming & Residency:** active/hot/cold regions, GPU/CPU budgets and
  derived statuses, cooked dataset requirements, instance/node/lightmap/HLOD
  residency. All current native controls remain present.
- **Frame Scheduling (Advanced), World Rendering, Memory Budgets, Collision,
  Ocean, Semantic Lighting, Editor Diagnostics & Display:** existing controls
  remain under their current native owners.

Single-setting nested subgroups have been flattened; fields with more than one
related control retain their subsection. The editor execution mode is managed
using the existing toolbar. The redundant `editor_runtime_streaming_preview`
property remains callable for API compatibility but is no longer a second
Inspector checkbox; it was editor-only and was never a stored scene value.

### Terrain3DMaterial

The shader quality preset and common terrain settings share **Material Setup**.
Near, Middle and Far material controls remain in their current detail tiers;
shader status remains read-only. Stored compatibility fields are unchanged.

## In-editor documentation

Each displayed native `Terrain3D` or `Terrain3DMaterial` property is included
in the collapsed **Property Help** section at the top of its Inspector. Search
by the property's visible name, serialized key, section, purpose, units, or
related feature. It reports the selected object's effective read-only/visibility
flags and is not another editor. This is intentionally one optional help section
rather than 245 additional visible help rows or help-only submenus.

Property descriptions live in `setting_metadata.json`, and are also used as
hover tooltips in the existing render-quality and profiler settings panels.
Native property hints (enums, resource selection, ranges, and bitmasks) still
come from the extension, not from the help catalog.

The documented legacy fields `save_16_bit`, `show_grid`,
`gpu_region_guard_regions`, and `gpu_region_prefetch_rows` are stored
compatibility properties. They are not live Inspector options; their current
native serialization/load behavior remains unchanged. The two retired UI-only
GI keys are absent from the current native property registration, so stale
metadata entries for those keys were removed.

## Implementation boundaries

The current VT spatial-demand pipeline, shader handling, cooking, rendering,
streaming, profiling, brushes, and erosion tools are preserved. The add-on
registers one read-only Inspector help plugin; it does not override
`_parse_property`, suppress native editor rows, or call native setters. All
property registrations and native method bindings remain in their original
order; the only native usage change hides the redundant editor-only preview
checkbox. No change is made to `RegionWorkbench`'s existing locking policy.

The existing shader quality and runtime profiler windows still offer their
native-backed setting panels. This patch updates their tooltips through the
shared metadata. The render-quality window now disables SpinBox controls with
Godot's actual `editable` property, and the profiler settings window locks
manual cache budgets whenever their Auto mode is active. Both continue using
the original native property setters.

## Validation

Check a newly created and existing Terrain3D scene, resource serialization,
section collapse behavior, selected-object help search, manual vs automatic
budget read-only state, shader-feature conditional properties, brush painting,
editor re-entry, and native build. The source ZIP is not an interactive Godot
editor test; those operations must be exercised in Godot 4.7.2.
