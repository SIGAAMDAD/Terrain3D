#[compute]
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform InstanceFrame {
    mat4 view_projection[2];
    mat4 view_matrix;
    mat4 previous_view_projection[2];
    vec4 camera_position[2];
    vec4 motion_jitter;
    vec4 motion_params;
}
frame_data;

struct InstanceCullDescriptor {
    vec4 bounds_center;
    vec4 bounds_extent;
    vec3 occlusion_center;
    uint hiz_region_index;
    vec3 occlusion_extent;
    uint draw_slot;
    uint command_index;
    uint first_instance;
    uint instance_count;
    uint first_visible;
    uint visibility_layers;
    uint flags;
    float lod_begin;
    float lod_end;
};
layout(set = 0, binding = 1, std430) readonly buffer DescriptorBuffer {
    InstanceCullDescriptor descriptors[];
};
layout(set = 0, binding = 2, std430) buffer MainVisibleBuffer {
    uint main_visible[];
};
struct IndexedIndirectCommand {
    uint index_count;
    uint instance_count;
    uint first_index;
    int vertex_offset;
    uint first_instance;
};
layout(set = 0, binding = 3, std430) buffer MainIndirectBuffer {
    IndexedIndirectCommand main_commands[];
};
layout(set = 0, binding = 4, std430) buffer ShadowVisibleBuffer {
    uint shadow_visible[];
};
layout(set = 0, binding = 5, std430) buffer ShadowIndirectBuffer {
    IndexedIndirectCommand shadow_commands[];
};
struct InstanceRecord {
    vec3 position;
    uint quaternion01;
    uint quaternion23;
    uint scale01;
    uint scale2_flags;
    uint color_rgba8;
};
layout(set = 0, binding = 6, std430) readonly buffer InstanceBuffer {
    InstanceRecord instances[];
};
layout(set = 0, binding = 7) uniform sampler2D hiz_texture;
struct InstanceHiZRegionDescriptor {
    vec4 bounds_center;
    vec4 bounds_extent;
};
layout(set = 0, binding = 8, std430) readonly buffer HiZRegionDescriptorBuffer {
    InstanceHiZRegionDescriptor hiz_regions[];
};
layout(set = 0, binding = 9, std430) buffer HiZRegionVisibilityBuffer {
    uint hiz_region_visible[];
};
layout(set = 0, binding = 10, std430) buffer MainVisibleDrawSlotBuffer {
    uint main_visible_draw_slots[];
};
layout(set = 0, binding = 11, std430) buffer ShadowVisibleDrawSlotBuffer {
    uint shadow_visible_draw_slots[];
};
layout(set = 0, binding = 12, std430) buffer MainVisibleLayerBuffer {
    uint main_visible_layers[];
};
layout(set = 0, binding = 13, std430) buffer ShadowVisibleLayerBuffer {
    uint shadow_visible_layers[];
};

layout(push_constant, std430) uniform CullData {
    uint descriptor_base;
    uint descriptor_count;
    uint command_count;
    uint visibility_mask;
    uint view_count;
    uint shadow_pass;
    uint mode;
    uint hiz_enabled;
    uint hiz_width;
    uint hiz_height;
    uint hiz_mip_count;
    uint hiz_region_count;
    uint main_command_base;
    uint shadow_command_base;
    uint reserved_2;
    uint reserved_3;
}
cull_data;

const uint T3D_RD_INSTANCE_SHADOWS_ONLY = 1u << 2u;
const uint T3D_RD_INSTANCE_HLOD = 1u << 25u;
const uint T3D_RD_INSTANCE_LOD_PRESELECTED = 1u << 28u;
const uint T3D_RD_INSTANCE_HIZ_CELL = 1u << 29u;
const uint T3D_RD_INSTANCE_HIZ_INSTANCE = 1u << 30u;

shared uint shared_visible;
shared uint shared_write_base;

bool aabb_visible_view(vec3 c, vec3 e, uint view_index) {
    vec4 corners[8];
    mat4 vp = frame_data.view_projection[min(view_index, 1u)];
    corners[0] = vp * vec4(c.x - e.x, c.y - e.y, c.z - e.z, 1.0);
    corners[1] = vp * vec4(c.x + e.x, c.y - e.y, c.z - e.z, 1.0);
    corners[2] = vp * vec4(c.x - e.x, c.y + e.y, c.z - e.z, 1.0);
    corners[3] = vp * vec4(c.x + e.x, c.y + e.y, c.z - e.z, 1.0);
    corners[4] = vp * vec4(c.x - e.x, c.y - e.y, c.z + e.z, 1.0);
    corners[5] = vp * vec4(c.x + e.x, c.y - e.y, c.z + e.z, 1.0);
    corners[6] = vp * vec4(c.x - e.x, c.y + e.y, c.z + e.z, 1.0);
    corners[7] = vp * vec4(c.x + e.x, c.y + e.y, c.z + e.z, 1.0);
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

bool bounds_visible_view(InstanceCullDescriptor descriptor, uint view_index) {
    return aabb_visible_view(descriptor.bounds_center.xyz, descriptor.bounds_extent.xyz, view_index);
}

bool hiz_available() {
    return cull_data.shadow_pass == 0u && cull_data.hiz_enabled != 0u && cull_data.hiz_width > 0u && cull_data.hiz_height > 0u
        && cull_data.hiz_mip_count > 0u && frame_data.motion_params.x > 0.5;
}

bool hiz_occluded_aabb(vec3 center, vec3 extent) {
    if (!hiz_available())
        return false;
    vec2 ndc_min = vec2(1.0);
    vec2 ndc_max = vec2(-1.0);
    float nearest_depth = 0.0;
    mat4 previous_vp = frame_data.previous_view_projection[0];
    for (uint corner = 0u; corner < 8u; ++corner) {
        vec3 p = center
            + vec3((corner & 1u) != 0u ? extent.x : -extent.x,
                (corner & 2u) != 0u ? extent.y : -extent.y,
                (corner & 4u) != 0u ? extent.z : -extent.z);
        vec4 clip = previous_vp * vec4(p, 1.0);
        if (clip.w <= 1e-6)
            return false;
        vec3 ndc = clip.xyz / clip.w;
        if (ndc.z < 0.0 || ndc.z > 1.0)
            return false;
        ndc_min = min(ndc_min, ndc.xy);
        ndc_max = max(ndc_max, ndc.xy);
        nearest_depth = max(nearest_depth, ndc.z);
    }
    if (ndc_max.x < -1.0 || ndc_min.x > 1.0 || ndc_max.y < -1.0 || ndc_min.y > 1.0)
        return false;
    ndc_min = clamp(ndc_min, vec2(-1.0), vec2(1.0));
    ndc_max = clamp(ndc_max, vec2(-1.0), vec2(1.0));
    vec2 viewport = vec2(cull_data.hiz_width, cull_data.hiz_height);
    vec2 viewport_max = viewport - vec2(1.0);
    vec2 pixel_min = clamp((ndc_min * 0.5 + 0.5) * viewport - vec2(1.0), vec2(0.0), viewport_max);
    vec2 pixel_max = clamp((ndc_max * 0.5 + 0.5) * viewport + vec2(1.0), vec2(0.0), viewport_max);
    float extent_pixels = max(pixel_max.x - pixel_min.x + 1.0, pixel_max.y - pixel_min.y + 1.0);
    uint mip = 0u;
    float cover = 1.0;
    while (mip + 1u < cull_data.hiz_mip_count && cover < extent_pixels) {
        cover *= 2.0;
        ++mip;
    }
    uint scale = 1u << mip;
    ivec2 p0 = ivec2(uvec2(pixel_min) / scale);
    ivec2 p1 = ivec2(uvec2(pixel_max) / scale);
    int mip_i = int(mip);
    ivec2 mip_size = textureSize(hiz_texture, mip_i);
    ivec2 mip_max = mip_size - ivec2(1);
    p0 = clamp(p0, ivec2(0), mip_max);
    p1 = clamp(p1, ivec2(0), mip_max);
    float d = min(min(texelFetch(hiz_texture, ivec2(p0.x, p0.y), mip_i).r, texelFetch(hiz_texture, ivec2(p1.x, p0.y), mip_i).r),
        min(texelFetch(hiz_texture, ivec2(p0.x, p1.y), mip_i).r, texelFetch(hiz_texture, ivec2(p1.x, p1.y), mip_i).r));
    return d > nearest_depth + 0.001;
}

mat3 quaternion_basis(vec4 q) {
    vec3 q2 = q.xyz + q.xyz;
    vec3 qq = q.xyz * q2;
    vec3 qw = q.www * q2;
    vec3 qxy = vec3(q.x * q2.y, q.x * q2.z, q.y * q2.z);
    return mat3(vec3(1.0 - qq.y - qq.z, qxy.x + qw.z, qxy.y - qw.y),
        vec3(qxy.x - qw.z, 1.0 - qq.x - qq.z, qxy.z + qw.x),
        vec3(qxy.y + qw.y, qxy.z - qw.x, 1.0 - qq.x - qq.y));
}

bool instance_proxy_occluded(InstanceRecord record, vec3 local_center, vec3 local_extent) {
    vec2 q01 = unpackSnorm2x16(record.quaternion01);
    vec2 q23 = unpackSnorm2x16(record.quaternion23);
    vec4 q = normalize(vec4(q01, q23));
    vec2 scale01 = unpackHalf2x16(record.scale01);
    vec2 scale2_flags = unpackHalf2x16(record.scale2_flags);
    vec3 scale = vec3(scale01, scale2_flags.x);
    mat3 basis = quaternion_basis(q);
    vec3 scaled_center = local_center * scale;
    vec3 scaled_extent = local_extent * abs(scale);
    vec3 world_center = record.position + basis * scaled_center;
    mat3 abs_basis = mat3(abs(basis[0]), abs(basis[1]), abs(basis[2]));
    vec3 world_extent = abs_basis * scaled_extent;
    return hiz_occluded_aabb(world_center, world_extent);
}

bool descriptor_visible(InstanceCullDescriptor descriptor) {
    if (descriptor.instance_count == 0u || (descriptor.visibility_layers & cull_data.visibility_mask) == 0u)
        return false;
    if (cull_data.shadow_pass == 0u) {
        if ((descriptor.flags & T3D_RD_INSTANCE_SHADOWS_ONLY) != 0u)
            return false;
        if ((descriptor.flags & (T3D_RD_INSTANCE_HLOD | T3D_RD_INSTANCE_LOD_PRESELECTED)) == 0u) {
            vec3 delta = descriptor.bounds_center.xyz - frame_data.camera_position[0].xyz;
            float distance_sq = dot(delta, delta);
            if (descriptor.lod_begin > 0.001 && distance_sq < descriptor.lod_begin * descriptor.lod_begin)
                return false;
            if (descriptor.lod_end > 0.001 && distance_sq >= descriptor.lod_end * descriptor.lod_end)
                return false;
        }
    }
    uint views = clamp(cull_data.view_count, 1u, 2u);
    bool frustum_visible = false;
    for (uint view = 0u; view < views; ++view) {
        if (bounds_visible_view(descriptor, view)) {
            frustum_visible = true;
            break;
        }
    }
    if (!frustum_visible)
        return false;
    if (hiz_available() && descriptor.hiz_region_index < cull_data.hiz_region_count
        && hiz_region_visible[descriptor.hiz_region_index] == 0u)
        return false;
    if ((descriptor.flags & T3D_RD_INSTANCE_HIZ_CELL) != 0u && hiz_available()) {
        bool per_instance = (descriptor.flags & T3D_RD_INSTANCE_HIZ_INSTANCE) != 0u;
        vec3 center = per_instance ? descriptor.bounds_center.xyz : descriptor.occlusion_center.xyz;
        vec3 extent = per_instance ? descriptor.bounds_extent.xyz : descriptor.occlusion_extent.xyz;
        if (hiz_occluded_aabb(center, extent))
            return false;
    }
    return true;
}

void main() {
    if (cull_data.mode == 2u) {
        uint region_index = gl_GlobalInvocationID.x;
        if (region_index < cull_data.hiz_region_count) {
            InstanceHiZRegionDescriptor region = hiz_regions[region_index];
            bool visible = aabb_visible_view(region.bounds_center.xyz, region.bounds_extent.xyz, 0u);
            if (visible && hiz_available())
                visible = !hiz_occluded_aabb(region.bounds_center.xyz, region.bounds_extent.xyz);
            hiz_region_visible[region_index] = visible ? 1u : 0u;
        }
        return;
    }
    if (cull_data.mode == 0u) {
        uint command_index = gl_GlobalInvocationID.x;
        if (command_index < cull_data.command_count) {
            if (cull_data.shadow_pass != 0u)
                shadow_commands[cull_data.shadow_command_base + command_index].instance_count = 0u;
            else
                main_commands[cull_data.main_command_base + command_index].instance_count = 0u;
        }
        return;
    }

    uint local_index = gl_LocalInvocationID.x;
    uint descriptor_index = gl_WorkGroupID.x;
    if (descriptor_index >= cull_data.descriptor_count)
        return;
    InstanceCullDescriptor descriptor = descriptors[cull_data.descriptor_base + descriptor_index];
    if (descriptor.command_index >= cull_data.command_count)
        return;
    if (local_index == 0u) {
        shared_visible = descriptor_visible(descriptor) ? 1u : 0u;
        shared_write_base = 0u;
        if (shared_visible != 0u && (descriptor.flags & T3D_RD_INSTANCE_HIZ_INSTANCE) == 0u) {
            shared_write_base = cull_data.shadow_pass != 0u
                ? atomicAdd(shadow_commands[cull_data.shadow_command_base + descriptor.command_index].instance_count, descriptor.instance_count)
                : atomicAdd(main_commands[cull_data.main_command_base + descriptor.command_index].instance_count, descriptor.instance_count);
        }
    }
    barrier();
    if (shared_visible == 0u)
        return;

    bool per_instance_hiz = cull_data.shadow_pass == 0u && hiz_available() && (descriptor.flags & T3D_RD_INSTANCE_HIZ_INSTANCE) != 0u;
    if (!per_instance_hiz) {
        uint destination_base = descriptor.first_visible + shared_write_base;
        for (uint i = local_index; i < descriptor.instance_count; i += 64u) {
            if (cull_data.shadow_pass != 0u) {
                shadow_visible[destination_base + i] = descriptor.first_instance + i;
                shadow_visible_draw_slots[destination_base + i] = descriptor.draw_slot;
                shadow_visible_layers[destination_base + i] = descriptor.visibility_layers;
            } else {
                main_visible[destination_base + i] = descriptor.first_instance + i;
                main_visible_draw_slots[destination_base + i] = descriptor.draw_slot;
                main_visible_layers[destination_base + i] = descriptor.visibility_layers;
            }
        }
        return;
    }

    for (uint i = local_index; i < descriptor.instance_count; i += 64u) {
        uint instance_index = descriptor.first_instance + i;
        if (instance_proxy_occluded(instances[instance_index], descriptor.occlusion_center.xyz, descriptor.occlusion_extent.xyz))
            continue;
        uint destination = atomicAdd(main_commands[cull_data.main_command_base + descriptor.command_index].instance_count, 1u);
        uint visible_slot = descriptor.first_visible + destination;
        main_visible[visible_slot] = instance_index;
        main_visible_draw_slots[visible_slot] = descriptor.draw_slot;
        main_visible_layers[visible_slot] = descriptor.visibility_layers;
    }
}
