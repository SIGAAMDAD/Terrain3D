#[vertex]
#version 450

layout(location = 0) in vec3 in_position;

layout(set = 0, binding = 0, std140) uniform InstanceFrame
{
	mat4 view_projection[2];
	mat4 view_matrix;
	mat4 previous_view_projection[2];
	vec4 camera_position[2];
	vec4 motion_jitter;
	vec4 motion_params;
	vec4 nomad_sun_shape;
	vec4 nomad_sun_diffuse;
	vec4 nomad_sun_specular;
	vec4 nomad_sun_rim;
}
frame_data;

struct InstanceRecord
{
	vec3 position;
	uint quaternion01;
	uint quaternion23;
	uint scale01;
	uint scale2_flags;
	uint color_rgba8;
};
layout(set = 0, binding = 1, std430) readonly buffer InstanceRecords
{
	InstanceRecord records[];
};
layout(set = 0, binding = 2, std430) readonly buffer VisibleIndices
{
	uint visible_indices[];
};
layout(push_constant, std430) uniform InstancePush
{
	uint material_index;
	uint overlay_material_index;
	uint visible_base;
	uint flags;
	uint visibility_layers;
	uint reserved0;
	uint reserved1;
	uint reserved2;
}
push_data;

vec3 rotate_quaternion(vec4 q, vec3 v)
{
	return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main()
{
	uint record_index = visible_indices[push_data.visible_base + uint(gl_InstanceIndex)];
	InstanceRecord record = records[record_index];
	vec2 q01 = unpackSnorm2x16(record.quaternion01);
	vec2 q23 = unpackSnorm2x16(record.quaternion23);
	vec4 quaternion = normalize(vec4(q01.x, q01.y, q23.x, q23.y));
	vec2 scale01 = unpackHalf2x16(record.scale01);
	vec2 scale2_flags = unpackHalf2x16(record.scale2_flags & 0x0000ffffu);
	vec3 local_position = in_position * vec3(scale01, scale2_flags.x);
	vec3 world_position = record.position + rotate_quaternion(quaternion, local_position);
	gl_Position = frame_data.view_projection[0] * vec4(world_position, 1.0);
}

#[fragment]
#version 450
void main()
{
}
