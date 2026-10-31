// CS02 parity fixture: explicit-footprint, camera-independent composition of
// pre-resolved material sources. This is NOT the shipping .t3sc cooker: source
// VT residency, triplanar projection, masks and authored overlays still require
// CS02 parity integration before producing authoritative compressed pages.
#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
// All source samplers are linear UNORM representations decoded BEFORE blending.
// This fixture deliberately exposes the clipmap-versus-instance sRGB filtering
// discrepancy so CS00 golden captures determine the eventual source contract.
layout(set = 0, binding = 0, r32ui) readonly uniform uimage2D controls;
layout(set = 0, binding = 1) uniform sampler2DArray source_albedo;
layout(set = 0, binding = 2) uniform sampler2DArray source_normal;
layout(set = 0, binding = 3) uniform sampler2DArray source_orm;
layout(set = 0, binding = 4) uniform sampler2DArray source_height;
layout(set = 0, binding = 5, rgba16f) writeonly uniform image2D composed_albedo;
layout(set = 0, binding = 6, rgba16f) writeonly uniform image2D composed_normal;
layout(set = 0, binding = 7, rgba16f) writeonly uniform image2D composed_orm;

// A page is 128 texels wide plus a 4-texel border on every side.
layout(push_constant, std430) uniform ComposeParams
{
	ivec2 page_origin_texel;
	uint material_count;
	uint bilinear_controls;
	vec2 world_to_uv_scale;
	vec2 source_grad_x;
	vec2 source_grad_y;
	float blend_sharpness;
	uint top_two_controls;
}
pc;

#define T3D_DIV_255 0.0039215686274509803921568627451
#include "terrain_composed_material_math.glslinc"

uint load_control(ivec2 texel)
{
	ivec2 size = imageSize(controls);
	return imageLoad(controls, clamp(texel, ivec2(0), size - ivec2(1))).r;
}

SampledMaterial fixture_sample_layer(int id, vec2 uv, vec2 dx, vec2 dy)
{
	SampledMaterial r = material_zero();
	float layer = float(clamp(id, 0, int(pc.material_count) - 1));
	// Explicit gradients permit a true compute/fragment comparison for these
	// already-resolved sources. No implicit derivatives are used.
	vec4 albedo = textureGrad(source_albedo, vec3(uv, layer), dx, dy);
	vec4 normal = textureGrad(source_normal, vec3(uv, layer), dx, dy);
	vec4 orm = textureGrad(source_orm, vec3(uv, layer), dx, dy);
	r.height = textureGrad(source_height, vec3(uv, layer), dx, dy).r;
	r.albedo = albedo.rgb;
	vec2 xy = normal.rg * 2.0 - 1.0;
	float len2 = dot(xy, xy);
	if (len2 > 1.0)
		xy *= inversesqrt(len2);
	r.tangent_normal = vec3(xy.x, sqrt(max(1.0 - dot(xy, xy), 0.0)), xy.y);
	r.ao = orm.r;
	r.roughness = orm.g;
	r.metallic = orm.b;
	r.ao_affect = 1.0;
	return r;
}

SampledMaterial fixture_resolve_pair(uint packed_control, vec2 texel_world)
{
	int base_id;
	int overlay_id;
	uint angle_index;
	float scale;
	float blend;
	decode_control(packed_control, base_id, overlay_id, blend, angle_index, scale);
	float angle = float(angle_index) * 0.39269908169872415480783042290994;
	float sn = sin(angle), cs = cos(angle);
	mat2 rotation = mat2(vec2(cs, sn), vec2(-sn, cs));
	vec2 uv = rotation * (texel_world * pc.world_to_uv_scale * scale);
	vec2 grad_x = rotation * (pc.source_grad_x * scale);
	vec2 grad_y = rotation * (pc.source_grad_y * scale);
	SampledMaterial base = fixture_sample_layer(base_id, uv, grad_x, grad_y);
	if (base_id == overlay_id)
		return base;
	SampledMaterial overlay = fixture_sample_layer(overlay_id, uv, grad_x, grad_y);
	float fraction = t3d_comp_surf_height_blend(blend, base.height, overlay.height,
	fma(60.0, pc.blend_sharpness, 4.0));
	SampledMaterial result = material_zero();
	accumulate_material(result, base, 1.0 - fraction);
	accumulate_material(result, overlay, fraction);
	return result;
}

void main()
{
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, ivec2(136))))
		return;
	// This includes the actual halo texels. Authoring must supply neighboring
	// region controls and source pixels; border-clamp here is a test fixture.
	vec2 texel_world = vec2(pc.page_origin_texel + pos - ivec2(4)) + vec2(0.5);
	ivec2 control_texel = ivec2(floor(texel_world));
	uint c0 = load_control(control_texel);
	SampledMaterial result;
	if (pc.bilinear_controls == 0u)
	{
		result = fixture_resolve_pair(c0, texel_world);
	}
	else
	{
		uint c1 = load_control(control_texel + ivec2(1, 0));
		uint c2 = load_control(control_texel + ivec2(0, 1));
		uint c3 = load_control(control_texel + ivec2(1, 1));
		if (c0 == c1 && c0 == c2 && c0 == c3)
		{
			result = fixture_resolve_pair(c0, texel_world);
		}
		else
		{
			vec4 weights = corner_weights(texel_world);
			if (pc.top_two_controls != 0u)
				weights = select_top_two_control_weights(weights);
			weights = normalize_active_control_weights(weights);
			result = material_zero();
			if (weights.x > 0.0)
				accumulate_material(result, fixture_resolve_pair(c0, texel_world), weights.x);
			if (weights.y > 0.0)
				accumulate_material(result, fixture_resolve_pair(c1, texel_world), weights.y);
			if (weights.z > 0.0)
				accumulate_material(result, fixture_resolve_pair(c2, texel_world), weights.z);
			if (weights.w > 0.0)
				accumulate_material(result, fixture_resolve_pair(c3, texel_world), weights.w);
		}
	}
	imageStore(composed_albedo, pos, vec4(result.albedo, 1.0));
	imageStore(composed_normal, pos, vec4(normalize(result.tangent_normal), 1.0));
	imageStore(composed_orm, pos, vec4(result.ao, result.roughness, result.metallic, result.ao_affect));
}
