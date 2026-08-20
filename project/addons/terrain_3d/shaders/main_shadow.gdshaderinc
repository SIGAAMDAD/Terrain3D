#ifdef T3D_TEMPLATE_CONTEXT
shader_type spatial;
render_mode depth_draw_opaque, cull_back, unshaded, skip_vertex_transform;

/* Shadow terrain uber-shader. Composition and entry points stay in this root. */
#include "res://addons/terrain_3d/shaders/includes/terrain_common_defines.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/shadow/shadow_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/clipmap_batching_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/shadow/shadow_height_functions.gdshaderinc"

void vertex() {
#include "res://addons/terrain_3d/shaders/includes/clipmap_vertex_setup.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/shadow/shadow_vertex_body.gdshaderinc"
}

void fragment() {
}
#endif // T3D_TEMPLATE_CONTEXT
