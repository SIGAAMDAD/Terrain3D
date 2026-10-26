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

bool bounds_visible(HlodCullDescriptor descriptor) {
    vec3 c = descriptor.bounds_center.xyz;
    vec3 e = descriptor.bounds_extent.xyz;
    vec4 corners[8];
    corners[0] = frame_data.view_projection * vec4(c.x - e.x, c.y - e.y, c.z - e.z, 1.0);
    corners[1] = frame_data.view_projection * vec4(c.x + e.x, c.y - e.y, c.z - e.z, 1.0);
    corners[2] = frame_data.view_projection * vec4(c.x - e.x, c.y + e.y, c.z - e.z, 1.0);
    corners[3] = frame_data.view_projection * vec4(c.x + e.x, c.y + e.y, c.z - e.z, 1.0);
    corners[4] = frame_data.view_projection * vec4(c.x - e.x, c.y - e.y, c.z + e.z, 1.0);
    corners[5] = frame_data.view_projection * vec4(c.x + e.x, c.y - e.y, c.z + e.z, 1.0);
    corners[6] = frame_data.view_projection * vec4(c.x - e.x, c.y + e.y, c.z + e.z, 1.0);
    corners[7] = frame_data.view_projection * vec4(c.x + e.x, c.y + e.y, c.z + e.z, 1.0);
    bool outside_left = true;
    bool outside_right = true;
    bool outside_bottom = true;
    bool outside_top = true;
    bool outside_near = true;
    bool outside_far = true;
    for (int i = 0; i < 8; ++i) {
        vec4 p = corners[i];
        outside_left = outside_left && p.x < -p.w;
        outside_right = outside_right && p.x > p.w;
        outside_bottom = outside_bottom && p.y < -p.w;
        outside_top = outside_top && p.y > p.w;
        outside_near = outside_near && p.z > p.w;
        outside_far = outside_far && p.z < 0.0;
    }
    return !(outside_left || outside_right || outside_bottom || outside_top || outside_near || outside_far);
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
