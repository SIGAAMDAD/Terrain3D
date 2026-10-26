// Wetness revision 2026-10-05: region-scaled sampling and shared signed roughness.
// Material graph revision 2026-10-05: shared bounded color sampling.
#[versions]
color = "";
color_native = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING";
compat_color_native = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING";
editor_color_native = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING";
color_specular = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR";
compat_color_specular = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR";
editor_color_specular = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR";
color_motion = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION";
compat_color_motion = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION";
editor_color_motion = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION";
color_specular_motion = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
compat_color_specular_motion = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
editor_color_specular_motion = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
color_native_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY";
compat_color_native_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY";
editor_color_native_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_specular_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
compat_color_specular_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
editor_color_specular_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_motion_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
compat_color_motion_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
editor_color_motion_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_specular_motion_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
compat_color_specular_motion_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
editor_color_specular_motion_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_native_one_sun = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN";
compat_color_native_one_sun = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN";
editor_color_native_one_sun = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN";
color_specular_one_sun = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR";
compat_color_specular_one_sun = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR";
editor_color_specular_one_sun = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR";
color_motion_one_sun = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_MOTION";
compat_color_motion_one_sun = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_MOTION";
editor_color_motion_one_sun = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_MOTION";
color_specular_motion_one_sun = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
compat_color_specular_motion_one_sun = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
editor_color_specular_motion_one_sun = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
color_native_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_RADIANCE_ARRAY";
compat_color_native_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_RADIANCE_ARRAY";
editor_color_native_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_specular_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
compat_color_specular_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
editor_color_specular_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_motion_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
compat_color_motion_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
editor_color_motion_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_specular_motion_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
compat_color_specular_motion_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
editor_color_specular_motion_one_sun_radiance_array = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
depth = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_DEPTH";
compat_depth = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_DEPTH";
editor_depth = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_DEPTH";
depth_normal_roughness = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS";
compat_depth_normal_roughness = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS";
editor_depth_normal_roughness = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS";
depth_normal_roughness_voxel_gi = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS\n#define T3D_FORWARD_VOXEL_GI";
compat_depth_normal_roughness_voxel_gi = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS\n#define T3D_FORWARD_VOXEL_GI";
editor_depth_normal_roughness_voxel_gi = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS\n#define T3D_FORWARD_VOXEL_GI";

feedback_color_native = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING";
feedback_color_native_vt_hints = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_VT_RESIDENT_ANCESTOR_HINTS 1\n#define T3D_PREPARED_MIP_BIAS 1";
feedback_color_specular = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR";
feedback_color_motion = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION";
feedback_color_specular_motion = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
feedback_color_native_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY";
feedback_color_specular_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
feedback_color_motion_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
feedback_color_specular_motion_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
feedback_color_native_one_sun = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN";
feedback_color_specular_one_sun = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR";
feedback_color_motion_one_sun = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_MOTION";
feedback_color_specular_motion_one_sun = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
feedback_color_native_one_sun_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_RADIANCE_ARRAY";
feedback_color_specular_one_sun_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
feedback_color_motion_one_sun_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
feedback_color_specular_motion_one_sun_radiance_array = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_ONE_SUN\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";

// Shared clipmap source revision 3: bump when either clipmap include changes.

color_native_compact = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
compat_color_native_compact = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
editor_color_native_compact = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
color_specular_compact = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
compat_color_specular_compact = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
editor_color_specular_compact = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
color_motion_compact = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
compat_color_motion_compact = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
editor_color_motion_compact = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
color_specular_motion_compact = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
compat_color_specular_motion_compact = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
editor_color_specular_motion_compact = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
color_native_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
compat_color_native_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
editor_color_native_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
color_specular_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
compat_color_specular_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
editor_color_specular_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
color_motion_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
compat_color_motion_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
editor_color_motion_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
color_specular_motion_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
compat_color_specular_motion_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 1\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
editor_color_specular_motion_radiance_array_compact = "#define T3D_TERRAIN_SHADER_PATH 2\n#define T3D_TERRAIN_MATERIAL_BACKEND 1\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";


// Optional shader variants, selected explicitly by the host renderer.

color_native_vt_hints = "#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_VT_RESIDENT_ANCESTOR_HINTS 1\n#define T3D_PREPARED_MIP_BIAS 1";

feedback_color_native_compact = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
feedback_color_specular_compact = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
feedback_color_motion_compact = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
feedback_color_specular_motion_compact = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
feedback_color_native_radiance_array_compact = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
feedback_color_specular_radiance_array_compact = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
feedback_color_motion_radiance_array_compact = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";
feedback_color_specular_motion_radiance_array_compact = "#define T3D_VT_FRAGMENT_FEEDBACK\n#define T3D_VT_COARSE_FEEDBACK\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY\n#define T3D_FORWARD_COMPACT_CLUSTER_LISTS 1";

#[vertex]
#version 450
#extension GL_EXT_multiview : enable
#VERSION_DEFINES
#define T3D_TIER 1
#include "terrain_strip_topology_inc.glslinc"
#include "vt/terrain_vt_common_inc.glslinc"
#include "terrain_clipmap_contract.glslinc"
#include "terrain_clipmap_vertex_interface.glslinc"
#include "terrain_far_height_vertex_v1.glslinc"
#include "terrain_clipmap_editor_brush_overlay.glslinc"
#include "terrain_clipmap_vertex_inc.glslinc"

#[fragment]
#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_EXT_multiview : enable
#VERSION_DEFINES
#include "vt/terrain_vt_common_inc.glslinc"
#include "terrain_material_vt_contract.glslinc"
#include "vt/terrain_vt_filter_math_inc.glslinc"
#include "vt/terrain_vt_tail_filter_inc.glslinc"
#if defined(T3D_FORWARD_NATIVE_LIGHTING) && !defined(T3D_FORWARD_DEPTH)
#extension GL_EXT_samplerless_texture_functions : enable
#endif
#if defined(T3D_VT_FRAGMENT_FEEDBACK) && (!defined(T3D_FORWARD_DEPTH) || defined(T3D_FORWARD_NORMAL_ROUGHNESS))
#define T3D_VT_DECLARE_FRAGMENT_BINDINGS
#include "vt/terrain_vt_feedback_inc.glslinc"
#undef T3D_VT_DECLARE_FRAGMENT_BINDINGS
#endif
#include "terrain_forward_lighting_inc.glslinc"
#define T3D_TIER 1
#include "terrain_clipmap_contract.glslinc"
#include "terrain_clipmap_fragment_interface.glslinc"
// Keep includes at the RDShaderFile stage root: nested includes are not expanded.
// Mesh-specialized world-raster variants do not compile terrain material helpers.
#ifndef T3D_WORLD_RASTER_MESH
#include "terrain_composed_material_math.glslinc"
// The decoding implementation is supplied later by terrain_clipmap_fragment_inc.
vec3 decode_surface_normal(vec2 encoded_xy);
#include "terrain_composed_surface_sampling.glslinc"
#endif
#include "terrain_clipmap_editor_brush_overlay.glslinc"
#include "terrain_clipmap_fragment_inc.glslinc"
#define T3D_VT_RESOLVE_ALLOCATION(index)                                                                                                   \
    T3DVTAllocation(terrain_vt_allocations[index].base_page,                                                                               \
        terrain_vt_allocations[index].page_count,                                                                                          \
        terrain_vt_allocations[index].packed_mip0_extent,                                                                                  \
        terrain_vt_allocations[index].packed_mip_count_flags,                                                                              \
        terrain_vt_allocations[index].tail_index,                                                                                          \
        terrain_vt_allocations[index].generation,                                                                                          \
        terrain_vt_allocations[index].content_id,                                                                                          \
        terrain_vt_allocations[index].reserved)
#define T3D_VT_RESOLVE_PAGE_ENTRY(index)                                                                                                   \
    T3DVTPageEntry(terrain_vt_page_table[index].physical_slot,                                                                             \
        terrain_vt_page_table[index].parent_page,                                                                                          \
        terrain_vt_page_table[index].generation,                                                                                           \
        terrain_vt_page_table[index].packed_resolved_mip_flags)
#define T3D_VT_RESOLVE_TAIL_ENTRY(index) terrain_vt_tails[index]
#if defined(T3D_VT_RESIDENT_ANCESTOR_HINTS) && T3D_VT_RESIDENT_ANCESTOR_HINTS
#define T3D_VT_RESOLVE_HINT(index) terrain_vt_ancestor_hints[index]
#endif
#include "vt/terrain_vt_resolve_core.glslinc"
#ifdef T3D_VT_RESOLVE_HINT
#undef T3D_VT_RESOLVE_HINT
#endif
#undef T3D_VT_RESOLVE_TAIL_ENTRY
#undef T3D_VT_RESOLVE_PAGE_ENTRY
#undef T3D_VT_RESOLVE_ALLOCATION
