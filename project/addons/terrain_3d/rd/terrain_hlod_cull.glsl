#[compute]
#version 450

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameData {
    mat4 view_projection;
    vec4 camera;
}
frame_data;

struct HlodCullDescriptor {
    vec4 bounds_center;
    vec4 bounds_extent;
    uint visibility_layers;
    uint command_index;
    uint first_visible;
    uint reserved_0;
};
layout(set = 0, binding = 1, std430) readonly buffer HlodDescriptorBuffer {
    HlodCullDescriptor descriptors[];
};

struct IndexedIndirectCommand {
    uint index_count;
    uint instance_count;
    uint first_index;
    int vertex_offset;
    uint first_instance;
};
layout(set = 0, binding = 2, std430) buffer IndirectBuffer {
    IndexedIndirectCommand commands[];
};

layout(push_constant, std430) uniform CullData {
    uint descriptor_count;
    uint visibility_mask;
    uint command_count;
    uint main_command_base;
}
cull_data;

bool bounds_inside_plane(vec3 center, vec3 extent, vec4 plane) {
    float projected_center = dot(plane.xyz, center);
    float support = dot(abs(plane.xyz), extent);
    // Conservative float32 tolerance: near-plane rounding must not reject
    // boxes which the original eight-corner path would have admitted.
    float tolerance = 1e-5 * (abs(projected_center) + abs(plane.w) + support + 1.0);
    return projected_center + plane.w + support >= -tolerance;
}

bool bounds_visible(HlodCullDescriptor descriptor) {
    // Six support-point tests replace eight clip-space corner transforms.
    // These are the same Vulkan half-spaces used in terrain_instance_cull.
    vec3 center = descriptor.bounds_center.xyz;
    vec3 extent = descriptor.bounds_extent.xyz;
    mat4 rows = transpose(frame_data.view_projection);
    return bounds_inside_plane(center, extent, rows[3] + rows[0])
        && bounds_inside_plane(center, extent, rows[3] - rows[0])
        && bounds_inside_plane(center, extent, rows[3] + rows[1])
        && bounds_inside_plane(center, extent, rows[3] - rows[1])
        && bounds_inside_plane(center, extent, rows[3] - rows[2])
        && bounds_inside_plane(center, extent, rows[2]);
}

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= cull_data.descriptor_count)
        return;
    HlodCullDescriptor descriptor = descriptors[id];
    if (descriptor.command_index >= cull_data.command_count)
        return;
    bool layer_visible = (descriptor.visibility_layers & cull_data.visibility_mask) != 0u;
    commands[cull_data.main_command_base + descriptor.command_index].instance_count = layer_visible && bounds_visible(descriptor) ? 1u : 0u;
}
