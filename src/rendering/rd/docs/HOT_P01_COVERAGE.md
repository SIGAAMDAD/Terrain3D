# P01 — HOT producer and consumer coverage

Source inspected: HOT record in `src/rendering/rd/core/terrain_3d_rd_context.h`,
the three `writeHotRecord()` producer files and the `terrain_world_visibility`,
`terrain_world_raster`, and terrain-strip compute/raster GLSL consumers.

| HOT record section | C++ producer | Consumer / condition | Default when absent |
| --- | --- | --- | --- |
| `terrain` | `Terrain3DRdClipmapRenderer::uploadFrameHot` | terrain raster, terrain cull, scene pass | Zero |
| `world` | `Terrain3DWorldResources::uploadFrameHot` | World raster/visibility | Zero |
| `editor` | `Terrain3DRdClipmapRenderer::uploadSnapshotBuffers` | editor overlay and selection | Zero |
| `patches[]` | same; writes `snapshot.m_nPatchCount` | terrain clipmap and visibility | Unwritten patches zero |
| `bounds[]` | `Terrain3DWorldVisibilityGpu::dispatchHot` | World/terrain visibility | Unwritten bounds zero |
| `world_bases[20]` | `Terrain3DWorldResources::uploadFrameHot` | World visibility/raster data indices | Zero |
| `terrain_bases[4]` | `Terrain3DRdClipmapRenderer::uploadFrameHot` | terrain material/region/strip offsets | Zero |
| `scratch_bases[8]` | same | compute/visible indices and indirect scratch | Zero |
| `cpu_visible[64]` | `Terrain3DRdClipmapRenderer::stageCpuIndirect` | CPU-culling fallback | Zero |
| `cpu_indexed[20]` | same | CPU-culling fallback | Zero |
| `cpu_strip[384]` | same | CPU-culling strip indirect | Zero |

**Coverage contract:** The old pass allocator explicitly zeroed the entire
4,336-byte HOT record, then overlaid these producer writes. The new ABI-2
publisher reconstructs **exactly the same logical record**: for each allocated
record, the bytes written by producers override defaults; all unwritten bytes
become zero, including shortened patch lists, unused command indices, padding,
and sections not used by a particular depth/opaque/motion/shadow pass. Dirty
comparison applies *after* that reconstruction, against the committed image
for the correct physical backing slot. Therefore a pass cannot inherit a stale
optional section merely because it was not written this time.

P01 does not change shader layouts, fields, command index calculations, binding
strides, record capacity or the World/terrain selection logic. Tests exercise
partial/overlapping writes, patch-count-style shrink, slot rotation, failed
publication, and first-use unknown-memory defaults.
