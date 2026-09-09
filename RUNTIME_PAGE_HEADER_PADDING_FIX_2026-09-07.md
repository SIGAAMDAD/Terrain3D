# Runtime Page Header Padding Fix — 2026-09-07

## Symptom

`Terrain Runtime Dataset Cook` discovered every authored region but failed every page with:

`runtime page channel serialization failed`

A representative world reported 176 discovered regions, 0 cooked pages and 176 failures.

## Root cause

Runtime page v7 reserves `s_nRuntimePageHeaderBytes = 1248` bytes. That envelope is intentionally 16-byte aligned so the first staged channel starts at an aligned offset.

The concrete encoded fields consume only 1236 bytes:

- 40 bytes of fixed prefix fields;
- 24 channel descriptors x 48 bytes = 1152 bytes;
- 44 bytes of trailing source/world/region metadata.

`Terrain3DRuntimePageStore::t3dRuntimePageStoreEncodeHeader()` wrote those 1236 bytes and then returned success only when the write cursor equaled the full 1248-byte envelope. The comparison could never succeed. Consequently `t3dRuntimePageStoreFinalizePageBytes()` failed after otherwise valid channel staging, and `writePage()` collapsed that failure into the generic channel-serialization error for every region.

This defect was already present in the supplied P10 base and was carried into P11; the P11 instance work did not introduce it.

## Fix

The v7 ABI is preserved:

- header envelope remains 1248 bytes;
- page version remains 7;
- channel table count/layout remains unchanged;
- first payload remains naturally 16-byte aligned;
- decoder behavior remains compatible with existing v7 padding.

The encoder now:

1. checks that the concrete 1236-byte field layout fits inside the 1248-byte envelope before writing;
2. verifies that exactly 1236 defined bytes were emitted;
3. explicitly zero-fills the remaining 12 reserved bytes;
4. returns success after the complete 1248-byte header envelope has been materialized.

The writer now also reports final header/byte assembly failures separately from per-channel staging failures, so this class of defect no longer collapses into the misleading `runtime page channel serialization failed` message.

## Regression gate

`tools/verify_runtime_page_header_padding.py` locks the v7 relationship at 1236 defined bytes + 12 reserved alignment bytes and rejects restoration of the impossible `written == header_bytes` condition.
