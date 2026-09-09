# Instancer alpha-scissor two-sided lighting fix — 2026-09-09

## Symptom
Alpha-scissor card meshes were visible from both sides, but only one face showed the expected shaded/color result. The opposite face looked washed out / wrong.

## Actual fault
This was a shader-side two-sided lighting bug, not a placement/publication bug.

The instancer already routes double-sided materials into the no-cull pipeline when the source material uses `BaseMaterial3D::CULL_DISABLED`.
That means the backfaces were in fact being rasterized.

The failure was that `terrain_instances.glsl` lit both faces with the same tangent basis and geometric normal orientation:
- `normal` was initialized from `vertex_normal` without using `gl_FrontFacing`
- `tangent_to_world()` also built its TBN basis without correcting for backfaces

So backfaces were rendered, but shaded with the wrong orientation. On thin foliage cards that produces exactly the kind of one-sided color response shown in the screenshot.

## Patch
Updated `project/addons/terrain_3d/rd/terrain_instances.glsl` to:
1. add `face_oriented_normal()` using `gl_FrontFacing`
2. flip the fragment-space base normal for backfaces
3. rebuild the tangent-space basis for backfaces by flipping the handedness on the back side

## Files updated
- `t3d_inst_vis/project/addons/terrain_3d/rd/terrain_instances.glsl`
- `t3d_inst_vis_base/project/addons/terrain_3d/rd/terrain_instances.glsl`
- `t3d_inst_vis_applycheck/project/addons/terrain_3d/rd/terrain_instances.glsl`

## Standalone patch
- `/mnt/data/Terrain3D_Instancer_Alpha_Scissor_Two_Sided_Lighting_Fix_2026-09-09.patch`

## Expected result after rebuild
Double-sided alpha-scissor cards should now shade correctly from both sides instead of only one face receiving the expected color/lighting response.
