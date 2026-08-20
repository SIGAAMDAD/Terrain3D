#ifdef T3D_TEMPLATE_CONTEXT
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx, skip_vertex_transform;

/* Near terrain uber-shader. Composition and entry points stay in this root. */

#include "res://addons/terrain_3d/shaders/includes/terrain_common_defines.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/shared/terrain_render_samplers.gdshaderinc"

// Near-tier interface and implementation.
#include "res://addons/terrain_3d/shaders/includes/near/near_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/near/near_types.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/near/near_terrain_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/near/near_detiling_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/near/near_material_functions.gdshaderinc"

// Shared clipmap and editor support.
#include "res://addons/terrain_3d/shaders/includes/clipmap_batching_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/editor_setup.gdshaderinc"

void vertex() {
#include "res://addons/terrain_3d/shaders/includes/clipmap_vertex_setup.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/render_vertex_world_setup.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/near/near_vertex_body.gdshaderinc"
}

void fragment() {
#include "res://addons/terrain_3d/shaders/includes/near/near_fragment_terrain.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/near/near_fragment_material.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/editor_fragment.gdshaderinc"
}

#include "res://addons/terrain_3d/shaders/includes/near/near_lighting.gdshaderinc"
#endif // T3D_TEMPLATE_CONTEXT
