# GPU Page Capacity / Render-Tier Decoupling Fix

**Date:** 2026-09-06

## Failure

`normalizeRuntimeTierLayout()` treated the current GPU page-cache grant as authority over the authored Near/Middle/Far clipmap layout. Under aggregate-memory hard overcommit the memory coordinator is permitted to fund an automatic subsystem below its requested minimum, including zero bytes. The old normalization path then converted that transient residency shortfall into persistent render-tier mutation.

That coupling is invalid. GPU page capacity is physical residency state; shader/clipmap tier counts are render policy. A page-cache shortfall must not delete Near clipmap levels. The RD vertex shader already falls back from a missing primary page to Middle/Far height.

## Fix

- Capacity no longer writes `m_nGpuRegionDetailLods`, `m_nRuntimeNearShaderLods`, or `m_nRuntimeMiddleShaderLods`.
- A shortfall marks the owner memory policy dirty and preserves the configured tier layout.
- The warning is latched so repeated normalization does not flood the log.
- The diagnostic reports requested LODs, required pages, mapped capacity, effective budget bytes, and functional-minimum bytes.
- Capacity validation is skipped when GPU paging is disabled.
- Native build ID is changed to `p07-capacity-layout-nondestructive-2026-09-06` and printed at construction so stale DLLs are immediately identifiable.

## Runtime invariant

Terrain topology/tier layout remains valid even when Near page residency is temporarily zero. Missing Near pages use the existing Middle/Far height fallback until residency recovers.
