# HLOD 64m source ownership (V5)

- V5 packed header extends the V4 24-byte header with a uint32 count of per-mesh subcell-mask bytes (28 bytes total). It follows the mesh-minimum-level array before the node records. Old V2/V3/V4 blobs decode with zero ownership bits, preserving their legacy behavior outside the 64m tier.
- Each mesh subcell-mask entry corresponds to the existing dense (region index * 256 + mesh ID) minimum-level table entry, with low four bits (slot 0 NW, 1 NE, 2 SW, 3 SE). Only minimum-level-one meshes can have nonzero subcell masks. A zero mask means there is no independently suppressible source.
- Each V5 node record uses its previously reserved byte as the low-four-bit authored subcell ownership mask. The GDScript editor records a bit only for a fully contained source with a published 64m package.
- The native decoder requires every nonzero ownership bit to have a corresponding 64m leaf entry, rejects out-of-range bits and rejects node/mesh levels incompatible with quadrant ownership.
- The hot streamer maintains a 4-bit **GPU-acknowledged and desired** quadrant-ready mask for each 128m region. It publishes leaf geometry independently, while region representation levels count only 128m-or-larger proxies. Changes trigger the existing representation-generation notifications.
- Instancer publication suppresses a source cell only if the cell's complete conservative source bounds belong to one authored ready quadrant. Any cell crossing quadrant or region boundaries stays detailed; the editor also revokes a mesh's quadrant bit when an overlapping uncontained source exists.
- Streamed static nodes use the baked spawn-ID mask; no transform-center inference is used for source coverage. Region-level coarse suppression remains supported.
- Both mechanisms retain existing WORLD buffers, source handles, shaders and descriptor ABI; only the editor cook and HLOD/regular detailed-source ownership contracts change.

Acceptance still requires a real Godot 4.7.2 custom-editor bake, V5 resource reload and GPU camera-transition tests on target hardware.
