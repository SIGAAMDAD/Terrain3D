#[versions]
color = "";
color_native = "#define T3D_FORWARD_NATIVE_LIGHTING";
color_specular = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR";
color_motion = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION";
color_specular_motion = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
color_native_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_specular_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_motion_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_specular_motion_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
depth = "#define T3D_FORWARD_DEPTH";
depth_normal_roughness = "#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS";
depth_normal_roughness_voxel_gi = "#define T3D_FORWARD_DEPTH\n#define T3D_FORWARD_NORMAL_ROUGHNESS\n#define T3D_FORWARD_VOXEL_GI";


#[vertex]
#version 450
#extension GL_EXT_multiview : enable
#VERSION_DEFINES
#define T3D_TIER 2
#include "terrain_clipmap_vertex_inc.glsl"

#[fragment]
#version 450
#extension GL_EXT_multiview : enable
#VERSION_DEFINES
#include "terrain_forward_lighting_inc.glsl"
#define T3D_TIER 2
#include "terrain_clipmap_fragment_inc.glsl"
