#[versions]
reset = "#define T3D_WORLD_VIS_RESET";
cull = "#define T3D_WORLD_VIS_CULL";
finalize = "#define T3D_WORLD_VIS_FINALIZE";

#[compute]
#version 450
#VERSION_DEFINES
#include "terrain_world_contract.glslinc"

struct WorldFrame {
    mat4 view_projection;
    mat4 previous_view_projection;
    vec4 frustum_planes[6];
    vec4 camera_and_max_distance;
    vec4 viewport_and_inv_size;
    vec4 motion_jitter;
    uvec4 counts0;
    uvec4 counts1;
    uvec4 generations;
    uvec4 hiz;
};
struct VisibleItem {
    uint id;
    uint instance_id;
    uint kind;
    uint flags;
};
struct Cluster {
    vec4 sphere;
    vec4 cone;
    uvec4 geometry;
    uvec4 material;
};
struct TransformRecord {
    vec4 row0;
    vec4 row1;
    vec4 row2;
};
struct InstanceRecord {
    uvec4 a;
    uvec4 b;
    uvec2 material_cell;
    vec2 lod_range;
    float fade_margin;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};
struct DrawArgs {
    uint vertex_count;
    uint instance_count;
    uint first_vertex;
    uint first_instance;
};
struct MaterialRecord {
    uint vt_allocation;
    uint overlay_vt_allocation;
    uint flags;
    uint packed_scalars0;
    vec4 base_color;
    vec4 uv_scale_bias;
    uint packed_emission;
    uint packed_scalars1;
    uint sampler_class;
    uint reserved;
};
struct TerrainCluster {
    uvec4 strip0;
    uint flags;
    float origin_x;
    float origin_z;
    float scale;
    uvec4 material_and_reserved;
};

layout(push_constant, std430) uniform VisibilityPush {
    uvec4 control;
    uvec4 bucket_counts0;
    uvec4 bucket_counts1;
} push_data;
uint terrain_bucket_count(uint bucket) {
    return bucket < 4u ? push_data.bucket_counts0[bucket] : push_data.bucket_counts1[bucket - 4u];
}
uint terrain_bucket_base(uint bucket) {
    uint base = 0u;
    for (uint i = 0u; i < bucket; ++i)
        base += terrain_bucket_count(i);
    return base;
}
uint terrain_bucket_capacity(uint bucket, uint visible_capacity) {
    uint base = terrain_bucket_base(bucket);
    return visible_capacity > base ? min(terrain_bucket_count(bucket), visible_capacity - base) : 0u;
}

#ifdef T3D_WORLD_VIS_RESET
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) buffer CounterBuffer {
    uint counters[T3D_WORLD_VIS_COUNTER_COUNT];
};
layout(set = 0, binding = 1, std430) buffer IndirectBuffer {
    DrawArgs args[T3D_WORLD_INDIRECT_COMMAND_COUNT];
};
layout(set = 0, binding = 2, std430) readonly buffer FrameBuffer {
    WorldFrame frame;
};
void main() {
    for (uint i = 0u; i < T3D_WORLD_VIS_COUNTER_COUNT; ++i)
        counters[i] = 0u;
    for (uint bucket = 0u; bucket < T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT; ++bucket) {
        args[bucket].vertex_count = t3d_world_terrain_strip_bucket_vertex_count(bucket);
        args[bucket].instance_count = 0u;
        args[bucket].first_vertex = 0u;
        args[bucket].first_instance = terrain_bucket_base(bucket);
    }
    args[T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].vertex_count = T3D_WORLD_CLUSTER_RASTER_VERTEX_COUNT;
    args[T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].instance_count = 0u;
    args[T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].first_vertex = 0u;
    args[T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].first_instance =
        t3d_world_nonterrain_visible_base(frame.counts0.y, frame.counts1.x);
}
#endif

#ifdef T3D_WORLD_VIS_CULL
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    WorldFrame frame;
};
layout(set = 0, binding = 1, std430) readonly buffer CandidateBuffer {
    VisibleItem candidates[];
};
layout(set = 0, binding = 2, std430) readonly buffer ClusterBuffer {
    Cluster clusters[];
};
layout(set = 0, binding = 3, std430) readonly buffer TerrainBoundsBuffer {
    vec4 terrain_bounds[];
};
layout(set = 0, binding = 4, std430) readonly buffer TransformBuffer {
    TransformRecord transforms[];
};
layout(set = 0, binding = 5, std430) readonly buffer InstanceBuffer {
    InstanceRecord instances[];
};
layout(set = 0, binding = 6, std430) buffer VisibleBuffer {
    VisibleItem visible_items[];
};
layout(set = 0, binding = 7, std430) buffer CounterBuffer {
    uint counters[T3D_WORLD_VIS_COUNTER_COUNT];
};
layout(set = 0, binding = 8, std430) buffer IndirectBuffer {
    DrawArgs args[T3D_WORLD_INDIRECT_COMMAND_COUNT];
};
layout(set = 0, binding = 9) uniform sampler2D hiz_texture;
layout(set = 0, binding = 10, std430) readonly buffer MaterialBuffer {
    MaterialRecord materials[];
};
layout(set = 0, binding = 11, std430) readonly buffer TerrainClusterBuffer {
    TerrainCluster terrain_clusters[];
};
layout(set = 0, binding = 12, std430) readonly buffer TerrainPatchBoundsBuffer {
    vec4 terrain_patch_bounds[];
};

const uint KIND_TERRAIN = 0u;
const uint KIND_MESH = 1u;
const uint KIND_HLOD = 2u;
const uint KIND_STATIC = 3u;
const uint PASS_SHADOW = 3u;
const uint INSTANCE_CAST_SHADOWS = 1u << 0u;
const uint MATERIAL_CAST_SHADOWS = 1u << 2u;
const uint INVALID_ID = 0xffffffffu;
const uint TERRAIN_CAST_SHADOWS = 0x80000000u;

vec3 transform_point(TransformRecord tr, vec3 p) {
    return vec3(dot(tr.row0, vec4(p, 1.0)), dot(tr.row1, vec4(p, 1.0)), dot(tr.row2, vec4(p, 1.0)));
}
float transform_radius_scale(TransformRecord tr) {
    return max(length(tr.row0.xyz), max(length(tr.row1.xyz), length(tr.row2.xyz)));
}
bool sphere_frustum(vec3 center, float radius) {
    for (uint i = 0u; i < 6u; ++i) {
        vec4 p = frame.frustum_planes[i];
        if (dot(p.xyz, center) + p.w < -radius)
            return false;
    }
    return true;
}
bool cone_visible(TransformRecord tr, Cluster cl, vec3 center) {
    if (cl.cone.w <= -0.9999)
        return true;
    vec3 axis = vec3(dot(tr.row0.xyz, cl.cone.xyz), dot(tr.row1.xyz, cl.cone.xyz), dot(tr.row2.xyz, cl.cone.xyz));
    vec3 view = frame.camera_and_max_distance.xyz - center;
    float denom = length(axis) * length(view);
    if (denom <= 1e-20)
        return true;
    float cosine = dot(axis, view) / denom;
    float threshold = -sqrt(max(0.0, 1.0 - cl.cone.w * cl.cone.w));
    return cosine > threshold;
}
bool hiz_occluded(vec3 center, float radius) {
    if ((push_data.control.z & 1u) == 0u || frame.hiz.z == 0u)
        return false;
    vec3 to_camera = frame.camera_and_max_distance.xyz - center;
    float camera_distance = length(to_camera);
    if (camera_distance <= radius + 1e-4)
        return false;
    vec4 clip = frame.view_projection * vec4(center, 1.0);
    if (clip.w <= 1e-5)
        return false;
    vec2 ndc = clip.xy / clip.w;
    float projection_scale = max(abs(frame.view_projection[0][0]), abs(frame.view_projection[1][1]));
    float radius_ndc = projection_scale * radius / abs(clip.w);
    vec2 uv_min = ndc * 0.5 + 0.5 - vec2(radius_ndc * 0.5);
    vec2 uv_max = ndc * 0.5 + 0.5 + vec2(radius_ndc * 0.5);
    if (any(lessThanEqual(uv_min, vec2(0.0))) || any(greaterThanEqual(uv_max, vec2(1.0))))
        return false;
    vec2 pixel_extent = (uv_max - uv_min) * vec2(frame.hiz.xy);
    float footprint = max(pixel_extent.x, pixel_extent.y);
    float mip = clamp(floor(log2(max(footprint, 1.0))), 0.0, float(frame.hiz.z - 1u));
    float d0 = textureLod(hiz_texture, vec2(uv_min.x, uv_min.y), mip).r;
    float d1 = textureLod(hiz_texture, vec2(uv_max.x, uv_min.y), mip).r;
    float d2 = textureLod(hiz_texture, vec2(uv_min.x, uv_max.y), mip).r;
    float d3 = textureLod(hiz_texture, vec2(uv_max.x, uv_max.y), mip).r;
    float scene_farthest = min(min(d0, d1), min(d2, d3));
    vec3 nearest = center + to_camera * (radius / camera_distance);
    vec4 nearest_clip = frame.view_projection * vec4(nearest, 1.0);
    if (nearest_clip.w <= 1e-5)
        return false;
    float nearest_depth = nearest_clip.z / nearest_clip.w;
    return nearest_depth < scene_farthest - 1e-5;
}
void append_visible(VisibleItem item) {
    uint dst = 0u;
    if (item.kind == KIND_TERRAIN) {
        uint actual_vertex_count = t3d_world_terrain_vertex_count(terrain_clusters[item.id].strip0.w);
        uint bucket = t3d_world_terrain_strip_bucket_index(actual_vertex_count);
        uint lane_index = atomicAdd(counters[t3d_world_terrain_strip_bucket_counter(bucket)], 1u);
        uint lane_capacity = terrain_bucket_capacity(bucket, frame.counts1.x);
        if (lane_index >= lane_capacity) {
            atomicOr(counters[T3D_WORLD_VIS_COUNTER_OVERFLOW], 1u);
            return;
        }
        dst = terrain_bucket_base(bucket) + lane_index;
        atomicAdd(counters[T3D_WORLD_VIS_COUNTER_TERRAIN], 1u);
        atomicAdd(counters[T3D_WORLD_VIS_COUNTER_TERRAIN_USEFUL_VERTICES], actual_vertex_count);
    } else {
        uint lane_index = atomicAdd(counters[T3D_WORLD_VIS_COUNTER_NONTERRAIN], 1u);
        uint lane_capacity = t3d_world_nonterrain_visible_capacity(frame.counts0.y, frame.counts1.x);
        if (lane_index >= lane_capacity) {
            atomicOr(counters[T3D_WORLD_VIS_COUNTER_OVERFLOW], 1u);
            return;
        }
        dst = t3d_world_nonterrain_visible_base(frame.counts0.y, frame.counts1.x) + lane_index;
    }
    visible_items[dst] = item;
    atomicAdd(counters[T3D_WORLD_VIS_COUNTER_VISIBLE], 1u);
    if (item.kind == KIND_HLOD)
        atomicAdd(counters[T3D_WORLD_VIS_COUNTER_HLOD], 1u);
    else if (item.kind == KIND_STATIC)
        atomicAdd(counters[T3D_WORLD_VIS_COUNTER_STATIC], 1u);
    else if (item.kind == KIND_MESH)
        atomicAdd(counters[T3D_WORLD_VIS_COUNTER_MESH], 1u);
}
void main() {
    uint index = gl_GlobalInvocationID.x;
    if (index >= push_data.control.x)
        return;
    atomicAdd(counters[T3D_WORLD_VIS_COUNTER_CANDIDATE], 1u);
    VisibleItem item = candidates[index];
    vec3 center;
    float radius;
    if (item.kind == KIND_TERRAIN) {
        if (item.id >= frame.counts0.y || (item.flags & push_data.control.y) == 0u)
            return;
        if (push_data.control.w == PASS_SHADOW && (item.instance_id & TERRAIN_CAST_SHADOWS) == 0u)
            return;
        uint patch_id = item.instance_id & ~TERRAIN_CAST_SHADOWS;
        vec4 bound = terrain_patch_bounds[patch_id];
        center = bound.xyz;
        radius = bound.w;
    } else {
        if (item.id >= frame.counts0.x)
            return;
        InstanceRecord inst = instances[item.instance_id];
        if ((inst.b.x & push_data.control.y) == 0u || inst.a.z >= frame.counts0.z)
            return;
        TransformRecord tr = transforms[inst.a.z];
        Cluster cl = clusters[item.id];
        if (push_data.control.w == PASS_SHADOW) {
            if ((item.kind == KIND_MESH || item.kind == KIND_HLOD) && (inst.b.y & INSTANCE_CAST_SHADOWS) == 0u)
                return;
            uint material_id = inst.material_cell.x != INVALID_ID ? inst.material_cell.x : cl.material.x;
            if (material_id != INVALID_ID && material_id < frame.counts0.w && (materials[material_id].flags & MATERIAL_CAST_SHADOWS) == 0u)
                return;
        }
        center = transform_point(tr, cl.sphere.xyz);
        radius = cl.sphere.w * transform_radius_scale(tr);
        float distance_to_camera = length(frame.camera_and_max_distance.xyz - center);
        float lod_begin = inst.lod_range.x;
        float lod_end = inst.lod_range.y;
        float fade = max(inst.fade_margin, 0.0);
        if (lod_end > lod_begin && (distance_to_camera < max(0.0, lod_begin - fade) || distance_to_camera > lod_end + fade)) {
            atomicAdd(counters[T3D_WORLD_VIS_COUNTER_LOD_REJECT], 1u);
            return;
        }
        if (!cone_visible(tr, cl, center)) {
            atomicAdd(counters[T3D_WORLD_VIS_COUNTER_CONE_REJECT], 1u);
            return;
        }
    }
    if (!sphere_frustum(center, radius)) {
        atomicAdd(counters[T3D_WORLD_VIS_COUNTER_FRUSTUM_REJECT], 1u);
        return;
    }
    if (hiz_occluded(center, radius)) {
        atomicAdd(counters[T3D_WORLD_VIS_COUNTER_HIZ_REJECT], 1u);
        return;
    }
    append_visible(item);
}
#endif

#ifdef T3D_WORLD_VIS_FINALIZE
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) buffer CounterBuffer {
    uint counters[T3D_WORLD_VIS_COUNTER_COUNT];
};
layout(set = 0, binding = 1, std430) buffer IndirectBuffer {
    DrawArgs args[T3D_WORLD_INDIRECT_COMMAND_COUNT];
};
layout(set = 0, binding = 2, std430) readonly buffer FrameBuffer {
    WorldFrame frame;
};
void main() {
    uint submitted_vertices = 0u;
    for (uint bucket = 0u; bucket < T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT; ++bucket) {
        uint lane_capacity = terrain_bucket_capacity(bucket, frame.counts1.x);
        uint visible_count = min(counters[t3d_world_terrain_strip_bucket_counter(bucket)], lane_capacity);
        uint vertex_count = t3d_world_terrain_strip_bucket_vertex_count(bucket);
        args[bucket].vertex_count = vertex_count;
        args[bucket].instance_count = visible_count;
        args[bucket].first_vertex = 0u;
        args[bucket].first_instance = terrain_bucket_base(bucket);
        submitted_vertices += visible_count * vertex_count;
    }
    counters[T3D_WORLD_VIS_COUNTER_TERRAIN_SUBMITTED_VERTICES] = submitted_vertices;
    uint nonterrain_capacity = t3d_world_nonterrain_visible_capacity(frame.counts0.y, frame.counts1.x);
    args[T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].vertex_count = T3D_WORLD_CLUSTER_RASTER_VERTEX_COUNT;
    args[T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].instance_count =
        min(counters[T3D_WORLD_VIS_COUNTER_NONTERRAIN], nonterrain_capacity);
    args[T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].first_vertex = 0u;
    args[T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].first_instance =
        t3d_world_nonterrain_visible_base(frame.counts0.y, frame.counts1.x);
}
#endif
