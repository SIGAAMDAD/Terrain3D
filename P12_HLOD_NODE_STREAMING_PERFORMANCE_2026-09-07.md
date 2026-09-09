# Terrain3D P12 - HLOD Transition Lists and Node Migration Cadence

**Date:** 2026-09-07  
**Base:** `Terrain3D_P01_P11_2026-09-07_runtime_page_header_padding_fixed`  
**Source patch family:** `Terrain3D_P12_P13_P14_Performance_128m_Memory_Patched_2026-09-04.zip`  
**Scope:** P12 only. P13 and P14 are intentionally not included.

## Implemented

### P12A - HLOD residency is generation/list driven

`Terrain3DHLODStreamer` no longer clears or scans every manifest entry during ordinary transition servicing.

The streamer now keeps bounded, cold-reserved transition lists:

- `m_activeEntries`
- `m_loadingEntries`
- `m_retiringEntries`
- `m_loadCandidates`

Desired state is represented by `m_nDesiredGeneration` rather than a per-entry boolean that must be cleared across the whole manifest. Selection rebuilds advance the generation, recursively mark only selected entries, and populate the load-candidate list. Stable transition ticks service only loading, candidate and retiring entries.

Active entries carry swap-remove positions for O(1) list removal. All four transition vectors reserve to `m_entries.size()` during manifest compilation, so the runtime transition path does not require backing growth.

`publishRepresentationMap()` now iterates the active list rather than all HLOD manifest entries. Runtime diagnostics expose active/loading/retiring/candidate counts and `transition_entries_examined_last_tick`.

### P12B - movable node migration cadence

`Terrain3DNodeStreamer::updateResidency()` no longer scans movable-node ownership every physics tick during steady camera residency.

The default cadence is four ticks:

```text
m_nMovableMigrationCadence = 4
```

Migration still runs immediately when:

- the update is forced;
- the streaming center changes;
- cadence expires.

The existing movable-node list remains the scanned population; no new per-frame container is introduced. Diagnostics expose `migration_scan_skipped_last_tick` and `movable_migration_cadence`.

## Compatibility preserved

- P01-P11 source remains the base.
- The 2026-09-07 runtime-page v7 header-padding fix is preserved.
- P11 instance dirty-range / GPU-culling state remains intact.
- No GDScript API or editor-script contract changes were made.
- No P13 ocean/clipmap/far-atlas work was imported.
- No P14 coordinator/runtime-validation work was imported.

## Validation

P12 contract inspection passes:

- generation-based HLOD desired state is present;
- active/loading/retiring/load-candidate lists are present;
- `updateResidency()` no longer performs a full `m_entries.size()` transition sweep;
- movable-node migration cadence and skip telemetry are present.

Existing repository gates including the dedicated P12 verifier: **30/30 PASS**.

Header-dependent C++ front-end validation: **14/14 translation units PASS** with:

```text
clang++ -std=c++17 -fsyntax-only -DGDEXTENSION
```

using the bundled Godot C++ / EASTL / EABase headers.

Full SCons/MSVC linking and Godot runtime/editor smoke testing are not available in this environment.
