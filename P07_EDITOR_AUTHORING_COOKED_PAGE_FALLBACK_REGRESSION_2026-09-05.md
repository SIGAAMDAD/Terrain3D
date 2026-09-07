# Terrain3D P07 Editor Authoring Cooked-Page Fallback Regression

**Date:** 2026-09-05  
**Affected stage:** P01 -> P07 after the P06 generation-gating regression repair  
**Visible symptom:** a loaded editor scene can retain an incorrect/coarse mesh presentation until a forced update, while the control/material selection itself can already be correct.

## Root cause

P07 changed `Terrain3DGpuRegionCache::uploadGpuRegionLayer()` so a full GPU page with cooked-page residency enabled failed closed whenever no prepared cooked page was available. P06 deliberately had a different editor contract:

```text
shipping/runtime -> cooked prepared page is authoritative
editor Authoring  -> live authored maps are authoritative
```

P07 accidentally made the shipping rule apply to editor Authoring too. A clean region whose cooked page was missing, stale, rejected by source validation, or incompatible with the current persistent texture layout could therefore remain unmapped at the detailed publication boundary. A later forced edit/update made the live authored representation eligible again and the visible mesh state suddenly corrected itself.

This path is independent of tessellation/compute displacement.

## Fix

P07 accounting and throttling remain intact, but the P06 authoring authority rule is restored:

1. If a prepared page is still pending, wait for it.
2. If no prepared page was attempted:
   - shipping/runtime fails closed;
   - editor Authoring proceeds from the live region maps.
3. If a prepared cooked image cannot be published into the current persistent texture layout, editor Authoring retries exactly once with `bAllowCookedPage == false`.
4. The live publication still passes through P07's byte/call token admission and persistent GPU texture update path.

The runtime-page source-cache key is also corrected to include the material and collision source signatures used by page validation. A source previously classified absent under one signature is therefore reconsidered when those signatures change; the capacity-failure latch uses the same complete key.

## Preserved behavior

- P07 exact finite mip accounting remains.
- Middle-composite and terrain-shadow upload accounting remains.
- P07 elapsed-time upload byte/call token throttling remains.
- Shipping/runtime cooked-page authority remains fail-closed.
- P06 material/control generation fixes remain.
- P06 physical page-table generation remains independent from topology generation.
- P08 functionality remains absent.

## Regression invariants

`tools/verify_p07_authoring_page_fallback.py` enforces:

1. no-ready cooked pages fail closed only outside editor Authoring;
2. incompatible prepared GPU publications retry through the non-cooked path only in editor Authoring;
3. material/collision source signatures participate in runtime-page source-cache invalidation;
4. the capacity-failure latch is invalidated by those same signature changes.

`tools/verify_p07_upload_accounting.py` continues to enforce P07 byte/call accounting and throttling while explicitly preserving the authoring fallback.

## Runtime validation target

```text
open existing scene in editor Authoring mode
-> do not edit terrain or force a mesh/update operation
-> detailed presentation converges from authored/live data when cooked page is absent/stale/incompatible
-> control/material selection remains correct
-> force the same update that previously repaired the scene
-> there is no visible before/after change
```
