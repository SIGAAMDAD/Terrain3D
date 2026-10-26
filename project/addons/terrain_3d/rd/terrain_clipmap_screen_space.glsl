// BR17: nonauthoritative terrain-only screen-space pilot.
// Coverage output and screen shading are independent, cold-selected variants.
// No live Forward+ cutover is implied by shader availability.
#[versions]
coverage = "#define T3D_SCREEN_SPACE_COVERAGE_PASS 1\n#define T3D_FORWARD_DEPTH\n#define T3D_RUNTIME_TIER\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0";
shade = "#define T3D_SCREEN_SPACE_SHADE_PASS 1\n#define T3D_RUNTIME_TIER\n#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_TERRAIN_SHADER_PATH 0\n#define T3D_TERRAIN_MATERIAL_BACKEND 0";
#[vertex]
#version 450
#extension GL_EXT_multiview : enable
#VERSION_DEFINES
#if !defined(T3D_SCREEN_SPACE_SHADE_PASS) || !T3D_SCREEN_SPACE_SHADE_PASS
#define T3D_TIER 0
#include "terrain_strip_topology_inc.glslinc"
#include "terrain_clipmap_contract.glslinc"
#include "terrain_clipmap_vertex_interface.glslinc"
#include "terrain_clipmap_vertex_inc.glslinc"

#else
// Draw one full-screen triangle. No geometry or vertex buffers are needed.
void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
#endif
#[fragment]
#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_EXT_multiview : enable
#VERSION_DEFINES
// Subgroup extension declarations follow RDShaderFile variant definitions.
#if defined(T3D_FORWARD_SUBGROUP_CLUSTER_LIGHTING) && T3D_FORWARD_SUBGROUP_CLUSTER_LIGHTING
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_vote : require
#extension GL_KHR_shader_subgroup_ballot : require
#endif
#if defined(T3D_SCREEN_SPACE_SHADE_PASS) && T3D_SCREEN_SPACE_SHADE_PASS
// BR17 capture target is a single-sample, five-attachment RGBA32F image.
// These owned descriptors are independent of native Forward+ sets 0 and 2.
layout(set=1, binding=0) uniform sampler2D t3d_br23_position_region_input;
layout(set=1, binding=1) uniform sampler2D t3d_br23_normal_tier_input;
layout(set=1, binding=2) uniform sampler2D t3d_br23_texel_dx_input;
layout(set=1, binding=3) uniform sampler2D t3d_br23_texel_dy_world_dx_input;
layout(set=1, binding=4) uniform sampler2D t3d_br23_world_dxz_dy_input;
#endif
#include "vt/terrain_vt_common_inc.glslinc"
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
#define T3D_TIER 0
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

#if defined(T3D_SCREEN_SPACE_SHADE_PASS) && T3D_SCREEN_SPACE_SHADE_PASS
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(t3d_br23_position_region_input, 0);
    if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, size))) {
        discard;
    }
    vec4 position_region = texelFetch(t3d_br23_position_region_input, pixel, 0);
    // A clear value of -2 is not a legitimate region index (-1 is the
    // canonical outside-region sentinel). Only rasterized terrain shades.
    if (position_region.w < -1.5) {
        discard;
    }
    vec4 normal_tier = texelFetch(t3d_br23_normal_tier_input, pixel, 0);
    vec4 texel_dx = texelFetch(t3d_br23_texel_dx_input, pixel, 0);
    vec4 texel_dy_world_dx = texelFetch(t3d_br23_texel_dy_world_dx_input, pixel, 0);
    vec4 world_dxz_dy = texelFetch(t3d_br23_world_dxz_dy_input, pixel, 0);
    world_position = position_region.xyz;
    terrain_region_layer = int(round(position_region.w));
    world_normal = normal_tier.xyz;
    patch_tier = uint(round(normal_tier.w));
    terrain_world_texel = texel_dx.xy;
    t3d_screen_space_texel_ddx = texel_dx.zw;
    t3d_screen_space_texel_ddy = texel_dy_world_dx.xy;
    t3d_screen_space_world_ddx = vec3(texel_dy_world_dx.zw, world_dxz_dy.x);
    t3d_screen_space_world_ddy = world_dxz_dy.yzw;
    t3d_screen_space_shade_fragment();
}
#endif
