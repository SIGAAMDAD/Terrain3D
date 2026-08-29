#[compute]
#version 450

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameData {
    mat4 view_projection;
    vec4 light_direction_ambient;
    vec4 light_color;
    vec4 terrain_params;
    vec4 mesh_params;
    vec4 target_params;
    vec4 camera_params;
    vec4 fog_params;
    vec4 fog_depth_params;
    vec4 displacement_params;
}
frame_data;

struct PatchData {
    vec2 origin;
    float scale;
    uint lod;
    uint tier;
    uint stitch_x;
    uint stitch_z;
    uint flags;
    float bounds_min_y;
    float bounds_max_y;
    float bounds_half_extent;
    float previous_origin_x;
    float previous_origin_z;
    float previous_scale;
    float padding_0;
    float padding_1;
};
layout(set = 0, binding = 9, std430) readonly buffer PatchBuffer {
    PatchData patches[];
};
layout(set = 0, binding = 10, std430) writeonly buffer VisibleBuffer {
    uint visible_indices[];
};

struct IndexedIndirectCommand {
    uint index_count;
    uint instance_count;
    uint first_index;
    int vertex_offset;
    uint first_instance;
};
layout(set = 0, binding = 11, std430) buffer IndirectBuffer {
    IndexedIndirectCommand commands[];
};

layout(push_constant, std430) uniform CullData {
    uint patch_count;
    uint lod0_index_count;
    uint outer_index_count;
    uint max_patches;
}
cull_data;

uint draw_group(PatchData patch_data) {
    if (patch_data.lod == 0u)
        return 0u;
    return patch_data.tier == 0u ? 1u : (patch_data.tier == 1u ? 2u : 3u);
}

bool patch_visible(PatchData patch_data) {
    float half_extent = patch_data.bounds_half_extent;
    float min_y = patch_data.bounds_min_y;
    float max_y = patch_data.bounds_max_y;
    vec2 center_xz = patch_data.origin;
    vec4 corners[8];
    corners[0] = frame_data.view_projection * vec4(center_xz.x - half_extent, min_y, center_xz.y - half_extent, 1.0);
    corners[1] = frame_data.view_projection * vec4(center_xz.x + half_extent, min_y, center_xz.y - half_extent, 1.0);
    corners[2] = frame_data.view_projection * vec4(center_xz.x - half_extent, max_y, center_xz.y - half_extent, 1.0);
    corners[3] = frame_data.view_projection * vec4(center_xz.x + half_extent, max_y, center_xz.y - half_extent, 1.0);
    corners[4] = frame_data.view_projection * vec4(center_xz.x - half_extent, min_y, center_xz.y + half_extent, 1.0);
    corners[5] = frame_data.view_projection * vec4(center_xz.x + half_extent, min_y, center_xz.y + half_extent, 1.0);
    corners[6] = frame_data.view_projection * vec4(center_xz.x - half_extent, max_y, center_xz.y + half_extent, 1.0);
    corners[7] = frame_data.view_projection * vec4(center_xz.x + half_extent, max_y, center_xz.y + half_extent, 1.0);
    bool outside_left = true, outside_right = true, outside_bottom = true, outside_top = true;
    bool outside_near = true, outside_far = true;
    for (int i = 0; i < 8; ++i) {
        vec4 c = corners[i];
        outside_left = outside_left && c.x < -c.w;
        outside_right = outside_right && c.x > c.w;
        outside_bottom = outside_bottom && c.y < -c.w;
        outside_top = outside_top && c.y > c.w;
        outside_near = outside_near && c.z > c.w;
        outside_far = outside_far && c.z < 0.0;
    }
    return !(outside_left || outside_right || outside_bottom || outside_top || outside_near || outside_far);
}

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= min(cull_data.patch_count, cull_data.max_patches))
        return;
    PatchData patch_data = patches[id];
    if (!patch_visible(patch_data))
        return;
    uint group = draw_group(patch_data);
    uint slot = atomicAdd(commands[group].instance_count, 1u);
    if (slot < cull_data.max_patches)
        visible_indices[group * cull_data.max_patches + slot] = id;
}
