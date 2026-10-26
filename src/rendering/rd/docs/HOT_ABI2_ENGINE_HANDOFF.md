# Godot Forward+ HOT native publication ABI 2 — engine integration contract

Source baseline: `Terrain3D_HOT_Per_Record_Rebuilt_Source.zip`. This document
records the P03/P04 engine seam. The **Godot engine source is not included** in
this Terrain3D repository. These methods cannot be completed or validated until
the matching custom Godot 4.7.2 engine fork implements them.

## Shared ABI

Copy `src/rendering/rd/core/terrain_3d_hot_range_abi.h` into the engine bridge
without changing its layout. The GDExtension registers callbacks as `uint64_t`
function addresses (matching existing ABI 1 conventions), and the engine calls
them as:

- `t3dHotRangePrepareFn`: `const t3dHotRangeBatch_t *(*)(void *, const t3dHotRangeInfo_t *) noexcept`;
- `t3dHotRangeCompleteFn`: `bool (*)(void *, const t3dHotRangeInfo_t *, bool success) noexcept`.

`HotRangeInfo` must identify the *same* physical frame backing slot that receives
the writes. `render_frame` and `device_frame` must match the Terrain3D transaction.
`byte_count == HOT_FRAME_BYTES`; `frame_slot_count == RenderingDevice::get_frame_delay()`.
The engine must never retain range pointers beyond the completion callback.

All ranges use **frame-relative** offsets, not RID-global offsets. They are
sorted, nonoverlapping, 16-byte aligned, and within `byte_count`. The engine
must revalidate these facts and the frame/slot/ABI before touching memory.
Batch `range_count == 0` means **no buffer update and no buffer flush**.

## P03 — ordinary deferred upload

Advertise:

- `get_deferred_upload_abi_version() -> 2`;
- `defer_native_buffer_update_ranges(rid, absolute_slot_offset, frame_bytes,
  prepare_callback_address, complete_callback_address, userdata_address) -> bool`.

The engine must enqueue this transaction at the same synchronization point as
ABI 1's `defer_native_buffer_update`, after lists can legally be closed and
before dependent draws consume the HOT data. At completion:

1. Construct a validated `t3dHotRangeInfo_t` (`abi=2`, `size=40`).
2. Call the prepare callback; reject/null means failure and dependent draws
   must not be submitted with incomplete data.
3. Validate batch header (`abi=2`, `size=24`), range array, offsets, lengths,
   stride bounds, and total bytes. Never treat a zero-range batch as failure.
4. For each range, stage/copy only its bytes and submit to `rid` at
   `absolute_slot_offset + range.offset_bytes`, honoring backend alignment.
   Use a preallocated staging pool and no per-range heap allocations.
5. On definitive success/failure, call complete exactly once. If any transfer
   failed or was canceled, pass `success=false` and reject dependent draws.

Keep ABI 1 functions for existing engine builds; ABI 2 is selected only if its
method exists and advertises version 2.

## P04 — persistent mapped publication

Advertise:

- `get_persistent_hot_abi_version() -> 2`;
- `get_persistent_hot_frame_slot(rid) -> int64_t` (physical slot of the mapping
  most recently selected by `buffer_persistent_map_advance`);
- `defer_native_buffer_flush_ranges(rid, frame_bytes,
  prepare_callback_address, complete_callback_address, userdata_address) -> bool`.

The GDExtension advances the persistent map *once*, asks the engine for its real
physical slot, and then stages all producer writes on a CPU mirror. In its
prepare callback, it copies **only the dirty spans** into the mapped address
selected by that earlier advance. The engine must flush/submit **only those
ranges** to the host-write visibility path and ensure the same GPU-side memory
barriers/fencing as ABI 1. For noncoherent memory, atom-size expansion of flush
ranges is allowed; the engine must never zero/write adjacent untouched bytes.
A zero-range batch needs no mapped writes and no range flush.

`buffer_flush(rid)` without ranges is *not* sufficient for ABI 2; it remains
supported for legacy and standalone paths only. Do not infer slot identity from
`frame % get_frame_delay()` or from a new implicit map advance.

## Failure and lifecycle boundaries

The GDExtension mirror is committed **only after** the engine reports success.
A failure invalidates the affected slot. The engine must not issue a successful
completion before the ranges have been published. End-of-device, shutdown,
standalone publication, frame-slot resizing, and device-loss recovery remain P05.
The Terrain3D current implementation conservatively invalidates all ABI 2
mirrors after a legacy standalone HOT upload.

## Required acceptance before selecting ABI 2 in production

- Compile and run the matching engine patch.
- Validate 2/3-frame slot rotation and `get_persistent_hot_frame_slot` identity.
- Confirm 0-byte changed frames produce 0 update/flush submissions.
- Verify callback failure cancels dependent draws and invalidates CPU mirrors.
- Verify shadow/depth/opaque/motion correctness and barrier timing on both
  ordinary and persistently mapped HOT paths.
