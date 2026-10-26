#[versions]
reset = "#define T3D_WORLD_VIS_RESET";
cull = "#define T3D_WORLD_VIS_CULL";
cull_grouped = "#define T3D_WORLD_VIS_CULL\n#define T3D_WORLD_GROUPED_APPEND 1";
cull_small = "#define T3D_WORLD_VIS_CULL\n#define T3D_WORLD_GROUPED_APPEND 1\n#define T3D_WORLD_SINGLE_WORKGROUP 1";
finalize = "#define T3D_WORLD_VIS_FINALIZE";
scan_classify = "#define T3D_WORLD_VIS_CULL\n#define T3D_WORLD_VIS_SCAN_CLASSIFY 1";
scan_prefix = "#define T3D_WORLD_VIS_SCAN_PREFIX 1";
scan_scatter = "#define T3D_WORLD_VIS_SCAN_SCATTER 1";
scan_finalize = "#define T3D_WORLD_VIS_FINALIZE\n#define T3D_WORLD_VIS_SCAN_FINALIZE 1";

#[compute]
#version 450
#VERSION_DEFINES
// P26: only the opt-in cull_grouped variant contains grouped reservations.
#ifndef T3D_WORLD_GROUPED_APPEND
#define T3D_WORLD_GROUPED_APPEND 0
#endif
#ifndef T3D_WORLD_SINGLE_WORKGROUP
#define T3D_WORLD_SINGLE_WORKGROUP 0
#endif
#include "terrain_world_contract.glslinc"
#define T3D_HOT_EXTERNAL_BINDING
#define T3D_HOT_INDEX push_data.hot_index
#include "terrain_clipmap_contract.glslinc"

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

layout(push_constant, std430) uniform VisibilityPush {
    uvec4 control;
    uvec4 bucket_counts0;
    uvec4 bucket_counts1;
    uint hot_index;
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

#define frame t3d_hot_records[T3D_HOT_INDEX].world
#define terrain_patch_bounds t3d_hot_records[T3D_HOT_INDEX].bounds
// GPU scan: fixed bounded scratch layout, no changes to the published
// visibility/indirect/counter ABI. 64K candidates -> 1024 workgroups.
#if defined(T3D_WORLD_VIS_SCAN_CLASSIFY) || defined(T3D_WORLD_VIS_SCAN_PREFIX) || \
    defined(T3D_WORLD_VIS_SCAN_SCATTER) || defined(T3D_WORLD_VIS_SCAN_FINALIZE)
#define T3D_WORLD_SCAN_CANDIDATE_LIMIT 65536u
#define T3D_WORLD_SCAN_GROUP_LIMIT 1024u
#define T3D_WORLD_SCAN_RECORD_WORDS 4u
#define T3D_WORLD_SCAN_COUNTS_BASE (T3D_WORLD_SCAN_CANDIDATE_LIMIT * T3D_WORLD_SCAN_RECORD_WORDS)
#define T3D_WORLD_SCAN_REJECT_BASE (T3D_WORLD_SCAN_COUNTS_BASE + T3D_WORLD_SCAN_GROUP_LIMIT * 7u)
#define T3D_WORLD_SCAN_PREFIX_BASE (T3D_WORLD_SCAN_REJECT_BASE + T3D_WORLD_SCAN_GROUP_LIMIT * 4u)
#define T3D_WORLD_SCAN_ACCEPTED_BASE (T3D_WORLD_SCAN_PREFIX_BASE + T3D_WORLD_SCAN_GROUP_LIMIT * 7u)
layout(set = 0, binding = 12, std430) buffer ScanScratchBuffer {
    uint words[];
} scan_scratch;
#endif
#ifdef T3D_WORLD_VIS_RESET
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) buffer CounterBuffer {
    uint counter_storage[];
};
layout(set = 0, binding = 1, std430) buffer IndirectBuffer {
    T3DWorldDrawIndirectArgs command_storage[];
};
layout(set = 0, binding = 2, std430) readonly buffer FrameBuffer {
    T3DHotRecord t3d_hot_records[];
};

#define frame t3d_hot_records[T3D_HOT_INDEX].world
#define terrain_patch_bounds t3d_hot_records[T3D_HOT_INDEX].bounds
void main() {
    for (uint i = 0u; i < T3D_WORLD_VIS_COUNTER_COUNT; ++i)
        counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + i] = 0u;
    // Dispatch provides the exact admitted candidate count for this pass.
    counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_CANDIDATE] = push_data.control.x;
    for (uint bucket = 0u; bucket < T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT; ++bucket) {
        command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + bucket].vertexCount = t3d_world_terrain_strip_bucket_vertex_count(bucket);
        command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + bucket].instanceCount = 0u;
        command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + bucket].firstVertex = 0u;
        command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + bucket].firstInstance = terrain_bucket_base(bucket);
    }
    command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].vertexCount = T3D_WORLD_CLUSTER_RASTER_VERTEX_COUNT;
    command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].instanceCount = 0u;
    command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].firstVertex = 0u;
    command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].firstInstance =
        t3d_world_nonterrain_visible_base(frame.counts0.y, frame.counts1.x);
}
#endif

#ifdef T3D_WORLD_VIS_CULL
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    T3DHotRecord t3d_hot_records[];
};
layout(set = 0, binding = 1, std430) readonly buffer CandidateBuffer {
        T3DWorldVisibleItem candidates[];
};
layout(set = 0, binding = 2, std430) readonly buffer ClusterBuffer {
        T3DWorldCluster clusters[];
};
layout(set = 0, binding = 3, std430) readonly buffer TerrainBoundsBuffer {
    vec4 terrain_bounds[];
};
layout(set = 0, binding = 4, std430) readonly buffer TransformBuffer {
    T3DWorldTransform transforms[];
};
layout(set = 0, binding = 5, std430) readonly buffer InstanceBuffer {
    InstanceRecord instances[];
};
layout(set = 0, binding = 6, std430) buffer VisibleBuffer {
    T3DWorldVisibleItem visible_items[];
};
layout(set = 0, binding = 7, std430) buffer CounterBuffer {
    uint counter_storage[];
};
layout(set = 0, binding = 8, std430) buffer IndirectBuffer {
    T3DWorldDrawIndirectArgs command_storage[];
};
layout(set = 0, binding = 9) uniform sampler2D hiz_texture;
layout(set = 0, binding = 10, std430) readonly buffer MaterialBuffer {
    MaterialRecord materials[];
};
layout(set = 0, binding = 11, std430) readonly buffer TerrainClusterBuffer {
    T3DWorldTerrainCluster terrain_clusters[];
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

vec3 transform_point(T3DWorldTransform tr, vec3 p) {
    return vec3(dot(tr.row0, vec4(p, 1.0)), dot(tr.row1, vec4(p, 1.0)), dot(tr.row2, vec4(p, 1.0)));
}
float transform_radius_scale(T3DWorldTransform tr) {
    return max(length(tr.row0.xyz), max(length(tr.row1.xyz), length(tr.row2.xyz)));
}
bool sphere_frustum(vec3 center, float radius) {
    for (uint i = 0u; i < 6u; ++i) {
        vec4 p = frame.frustumPlanes[i];
        if (dot(p.xyz, center) + p.w < -radius)
            return false;
    }
    return true;
}
bool cone_visible(T3DWorldTransform tr, T3DWorldCluster cl, vec3 center) {
    if (cl.cone.w <= -0.9999)
        return true;
    vec3 axis = vec3(dot(tr.row0.xyz, cl.cone.xyz), dot(tr.row1.xyz, cl.cone.xyz), dot(tr.row2.xyz, cl.cone.xyz));
    vec3 view = frame.cameraAndMaxDistance.xyz - center;
    float denom = length(axis) * length(view);
    if (denom <= 1e-20)
        return true;
    float cosine = dot(axis, view) / denom;
    float threshold = -sqrt(max(0.0, 1.0 - cl.cone.w * cl.cone.w));
    return cosine > threshold;
}
bool hiz_occluded(vec3 center, float radius) {
    if ((push_data.control.z & 1u) == 0u || frame.hizAndFlags.z == 0u)
        return false;
    vec3 to_camera = frame.cameraAndMaxDistance.xyz - center;
    float camera_distance = length(to_camera);
    if (camera_distance <= radius + 1e-4)
        return false;
    vec4 clip = frame.viewProjection * vec4(center, 1.0);
    if (clip.w <= 1e-5)
        return false;
    // The Hi-Z pyramid comes from the previous frame's jittered depth.
    // Previous and current unjittered view-projections are identical while
    // this history is authorized. Project into the previous pixel grid, not
    // the current frame's subpixel TAA offset.
    vec2 ndc = clip.xy / clip.w - frame.motionJitter.xy + frame.motionJitter.zw;
    float projection_scale = max(abs(frame.viewProjection[0][0]), abs(frame.viewProjection[1][1]));
    float radius_ndc = projection_scale * radius / abs(clip.w);
    // Expand by one Hi-Z texel to make depth-history lookup conservative at
    // an occluder edge even with rasterization quantization and jitter.
    vec2 texel_pad = 1.0 / max(vec2(frame.hizAndFlags.xy), vec2(1.0));
    vec2 uv_min = ndc * 0.5 + 0.5 - vec2(radius_ndc * 0.5) - texel_pad;
    vec2 uv_max = ndc * 0.5 + 0.5 + vec2(radius_ndc * 0.5) + texel_pad;
    if (any(lessThanEqual(uv_min, vec2(0.0))) || any(greaterThanEqual(uv_max, vec2(1.0))))
        return false;
    vec2 pixel_extent = (uv_max - uv_min) * vec2(frame.hizAndFlags.xy);
    float footprint = max(pixel_extent.x, pixel_extent.y);
    float mip = clamp(floor(log2(max(footprint, 1.0))), 0.0, float(frame.hizAndFlags.z - 1u));
    float d0 = textureLod(hiz_texture, vec2(uv_min.x, uv_min.y), mip).r;
    float d1 = textureLod(hiz_texture, vec2(uv_max.x, uv_min.y), mip).r;
    float d2 = textureLod(hiz_texture, vec2(uv_min.x, uv_max.y), mip).r;
    float d3 = textureLod(hiz_texture, vec2(uv_max.x, uv_max.y), mip).r;
    float scene_farthest = min(min(d0, d1), min(d2, d3));
    vec3 nearest = center + to_camera * (radius / camera_distance);
    vec4 nearest_clip = frame.viewProjection * vec4(nearest, 1.0);
    if (nearest_clip.w <= 1e-5)
        return false;
    float nearest_depth = nearest_clip.z / nearest_clip.w;
    return nearest_depth < scene_farthest - 1e-5;
}
void append_visible(T3DWorldVisibleItem item) {
    uint dst = 0u;
    if (item.geometryKind == KIND_TERRAIN) {
        uint actual_vertex_count = t3d_world_terrain_vertex_count(terrain_clusters[t3d_hot_records[T3D_HOT_INDEX].world_bases[5u] + item.clusterOrTerrainId].strip0.w);
        uint bucket = t3d_world_terrain_strip_bucket_index(actual_vertex_count);
        uint lane_index = atomicAdd(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + t3d_world_terrain_strip_bucket_counter(bucket)], 1u);
        uint lane_capacity = terrain_bucket_capacity(bucket, frame.counts1.x);
        if (lane_index >= lane_capacity) {
            atomicOr(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_OVERFLOW], 1u);
            return;
        }
        dst = terrain_bucket_base(bucket) + lane_index;
        atomicAdd(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_TERRAIN_USEFUL_VERTICES], actual_vertex_count);
    } else {
        uint lane_index = atomicAdd(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_NONTERRAIN], 1u);
        uint lane_capacity = t3d_world_nonterrain_visible_capacity(frame.counts0.y, frame.counts1.x);
        if (lane_index >= lane_capacity) {
            atomicOr(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_OVERFLOW], 1u);
            return;
        }
        dst = t3d_world_nonterrain_visible_base(frame.counts0.y, frame.counts1.x) + lane_index;
    }
    visible_items[t3d_hot_records[T3D_HOT_INDEX].world_bases[15] + dst] = item;
    if (item.geometryKind == KIND_HLOD)
        atomicAdd(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_HLOD], 1u);
    else if (item.geometryKind == KIND_STATIC)
        atomicAdd(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_STATIC], 1u);
    else if (item.geometryKind == KIND_MESH)
        atomicAdd(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_MESH], 1u);
}
#if T3D_WORLD_GROUPED_APPEND || defined(T3D_WORLD_VIS_SCAN_CLASSIFY)
// One 64-lane workgroup owns seven disjoint local output buckets. No ballot
// extension, subgroup-width assumption, new buffer, or host ABI is required.
shared uint group_bucket[64];
shared uint group_vertex_count[64];
shared uint group_kind[64];
shared uint group_rank[64];
shared uint group_reservation[7];
shared uint group_rejection[64];
shared uint group_terrain_vertex_sums[6];
shared uint group_overflow[7];

// Early returns are limited to this helper. main() always reaches the
// workgroup barriers, including in the final partially active workgroup.
bool classify_world_candidate(uint index, out T3DWorldVisibleItem item,
                              out uint bucket, out uint actual_vertex_count, out uint rejection) {
    bucket = 7u;
    actual_vertex_count = 0u;
    rejection = 4u; // No counted rejection (e.g. invalid candidate or layer mask).
    if (index >= push_data.control.x)
        return false;
    item = candidates[t3d_hot_records[T3D_HOT_INDEX].world_bases[14u] + index];
    vec3 center;
    float radius;
    if (item.geometryKind == KIND_TERRAIN) {
        if (item.clusterOrTerrainId >= frame.counts0.y || (item.flags & push_data.control.y) == 0u)
            return false;
        if (push_data.control.w == PASS_SHADOW && (item.instanceOrPatchId & TERRAIN_CAST_SHADOWS) == 0u)
            return false;
        uint patch_id = item.instanceOrPatchId & ~TERRAIN_CAST_SHADOWS;
        vec4 bound = terrain_patch_bounds[patch_id];
        center = bound.xyz;
        radius = bound.w;
    } else {
        if (item.clusterOrTerrainId >= frame.counts0.x)
            return false;
        InstanceRecord inst = instances[t3d_hot_records[T3D_HOT_INDEX].world_bases[9u] + item.instanceOrPatchId];
        if ((inst.b.x & push_data.control.y) == 0u || inst.a.z >= frame.counts0.z)
            return false;
        T3DWorldTransform tr = transforms[t3d_hot_records[T3D_HOT_INDEX].world_bases[7u] + inst.a.z];
        T3DWorldCluster cl = clusters[t3d_hot_records[T3D_HOT_INDEX].world_bases[3u] + item.clusterOrTerrainId];
        if (push_data.control.w == PASS_SHADOW) {
            if ((item.geometryKind == KIND_MESH || item.geometryKind == KIND_HLOD) && (inst.b.y & INSTANCE_CAST_SHADOWS) == 0u)
                return false;
            uint material_id = inst.material_cell.x != INVALID_ID ? inst.material_cell.x : cl.materialId;
            if (material_id != INVALID_ID && material_id < frame.counts0.w && (materials[t3d_hot_records[T3D_HOT_INDEX].world_bases[12u] + material_id].flags & MATERIAL_CAST_SHADOWS) == 0u)
                return false;
        }
        center = transform_point(tr, cl.sphere.xyz);
        radius = cl.sphere.w * transform_radius_scale(tr);
        // ENV-P04: ground-cover vertex displacement is clamped to +/- 0.5m per axis.
        // Include the full sqrt(2)*0.5m envelope in frustum and HiZ culling.
        bool ground_cover = (push_data.control.z & 2u) != 0u &&
            item.geometryKind != KIND_STATIC && ((inst.b.y >> 26u) & 3u) == 1u;
        if (ground_cover) radius += 0.71;
        float distance_to_camera = length(frame.cameraAndMaxDistance.xyz - center);
        float lod_begin = inst.lod_range.x;
        float lod_end = inst.lod_range.y;
        float fade = max(inst.fade_margin, 0.0);
        if (lod_end > lod_begin && (distance_to_camera < max(0.0, lod_begin - fade) || distance_to_camera > lod_end + fade)) {
            rejection = 0u;
            return false;
        }
        if (!ground_cover && !cone_visible(tr, cl, center)) {
            rejection = 1u;
            return false;
        }
    }
    if (!sphere_frustum(center, radius)) {
        rejection = 2u;
        return false;
    }
    if (hiz_occluded(center, radius)) {
        rejection = 3u;
        return false;
    }
    if (item.geometryKind == KIND_TERRAIN) {
        actual_vertex_count = t3d_world_terrain_vertex_count(terrain_clusters[t3d_hot_records[T3D_HOT_INDEX].world_bases[5u] + item.clusterOrTerrainId].strip0.w);
        bucket = t3d_world_terrain_strip_bucket_index(actual_vertex_count);
    } else {
        bucket = 6u;
    }
    return true;
}

#ifdef T3D_WORLD_VIS_SCAN_CLASSIFY
void main() {
    uint local_index = gl_LocalInvocationID.x;
    uint index = gl_GlobalInvocationID.x;
    T3DWorldVisibleItem item;
    uint bucket, actual_vertex_count, rejection;
    bool visible = classify_world_candidate(index, item, bucket, actual_vertex_count, rejection);
    group_bucket[local_index] = visible ? bucket : 7u;
    group_rejection[local_index] = rejection;
    if (index < push_data.control.x) {
        uint record = index * T3D_WORLD_SCAN_RECORD_WORDS;
        scan_scratch.words[record] = visible ? bucket : 7u;
        scan_scratch.words[record + 2u] = visible ? actual_vertex_count : 0u;
        scan_scratch.words[record + 3u] = visible ? item.geometryKind : 0u;
    }
    barrier();
    // The leaders own disjoint rank/count locations; classification is run
    // exactly once and the scatter stage consumes the stored decision.
    if (local_index < 7u) {
        uint count = 0u;
        for (uint lane = 0u; lane < 64u; ++lane) {
            if (group_bucket[lane] != local_index)
                continue;
            uint candidate = gl_WorkGroupID.x * 64u + lane;
            scan_scratch.words[candidate * T3D_WORLD_SCAN_RECORD_WORDS + 1u] = count++;
        }
        scan_scratch.words[T3D_WORLD_SCAN_COUNTS_BASE + gl_WorkGroupID.x * 7u + local_index] = count;
    } else if (local_index < 11u) {
        uint count = 0u;
        for (uint lane = 0u; lane < 64u; ++lane)
            count += uint(group_rejection[lane] == local_index - 7u);
        scan_scratch.words[T3D_WORLD_SCAN_REJECT_BASE + gl_WorkGroupID.x * 4u + local_index - 7u] = count;
    }
}
#else
void main() {
    uint local_index = gl_LocalInvocationID.x;
    uint index = gl_GlobalInvocationID.x;
    T3DWorldVisibleItem item;
    uint bucket, actual_vertex_count, rejection;
    bool visible = classify_world_candidate(index, item, bucket, actual_vertex_count, rejection);
    group_bucket[local_index] = visible ? bucket : 7u;
    group_kind[local_index] = visible ? item.geometryKind : KIND_TERRAIN;
    group_vertex_count[local_index] = visible ? actual_vertex_count : 0u;
    group_rejection[local_index] = rejection;
    barrier();

    // Seven leaders run in parallel. Each builds its lane-ordered prefix and
    // reserves a single contiguous global range for its represented bucket.
    if (local_index < 7u) {
        uint count = 0u;
        uint terrain_vertices = 0u;
        uint kind_mesh = 0u;
        uint kind_hlod = 0u;
        uint kind_static = 0u;
        for (uint lane = 0u; lane < 64u; ++lane) {
            if (group_bucket[lane] != local_index)
                continue;
            group_rank[lane] = count++;
            if (local_index < 6u)
                terrain_vertices += group_vertex_count[lane];
            else if (group_kind[lane] == KIND_MESH)
                ++kind_mesh;
            else if (group_kind[lane] == KIND_HLOD)
                ++kind_hlod;
            else if (group_kind[lane] == KIND_STATIC)
                ++kind_static;
        }
        uint reservation = 0u;
        group_overflow[local_index] = 0u;
        if (count != 0u) {
            uint counter_base = t3d_hot_records[T3D_HOT_INDEX].world_bases[16];
            uint counter = local_index < 6u
                ? t3d_world_terrain_strip_bucket_counter(local_index)
                : T3D_WORLD_VIS_COUNTER_NONTERRAIN;
#if T3D_WORLD_SINGLE_WORKGROUP
            // All candidates are dispatched in this single 64-lane workgroup.
            // Each bucket has exactly one elected writer; no atomic is needed.
            counter_storage[counter_base + counter] = count;
#else
            reservation = atomicAdd(counter_storage[counter_base + counter], count);
#endif
            uint capacity = local_index < 6u
                ? terrain_bucket_capacity(local_index, frame.counts1.x)
                : t3d_world_nonterrain_visible_capacity(frame.counts0.y, frame.counts1.x);
            uint admitted = reservation < capacity ? min(count, capacity - reservation) : 0u;
            if (admitted != count) {
                group_overflow[local_index] = 1u;
#if !T3D_WORLD_SINGLE_WORKGROUP
                atomicOr(counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_OVERFLOW], 1u);
#endif
                // Reproduce diagnostics for accepted output only when the
                // reservation crosses its capacity, including the partial lane.
                terrain_vertices = 0u;
                kind_mesh = 0u;
                kind_hlod = 0u;
                kind_static = 0u;
                for (uint lane = 0u; lane < 64u; ++lane) {
                    if (group_bucket[lane] != local_index || group_rank[lane] >= admitted)
                        continue;
                    if (local_index < 6u)
                        terrain_vertices += group_vertex_count[lane];
                    else if (group_kind[lane] == KIND_MESH)
                        ++kind_mesh;
                    else if (group_kind[lane] == KIND_HLOD)
                        ++kind_hlod;
                    else if (group_kind[lane] == KIND_STATIC)
                        ++kind_static;
                }
            }
            if (local_index == 6u) {
#if T3D_WORLD_SINGLE_WORKGROUP
                counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_MESH] = kind_mesh;
                counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_HLOD] = kind_hlod;
                counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_STATIC] = kind_static;
#else
                if (kind_mesh != 0u)
                    atomicAdd(counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_MESH], kind_mesh);
                if (kind_hlod != 0u)
                    atomicAdd(counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_HLOD], kind_hlod);
                if (kind_static != 0u)
                    atomicAdd(counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_STATIC], kind_static);
#endif
            }
        }
        if (local_index < 6u)
            group_terrain_vertex_sums[local_index] = terrain_vertices;
        group_reservation[local_index] = reservation;
    } else if (local_index < 11u) {
        // Four independent leaders collapse per-candidate rejection updates.
        const uint reject_counters[4] = uint[4](T3D_WORLD_VIS_COUNTER_LOD_REJECT,
            T3D_WORLD_VIS_COUNTER_CONE_REJECT, T3D_WORLD_VIS_COUNTER_FRUSTUM_REJECT,
            T3D_WORLD_VIS_COUNTER_HIZ_REJECT);
        uint count = 0u;
        for (uint lane = 0u; lane < 64u; ++lane)
            count += uint(group_rejection[lane] == local_index - 7u);
        if (count != 0u) {
            uint address = t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + reject_counters[local_index - 7u];
#if T3D_WORLD_SINGLE_WORKGROUP
            counter_storage[address] = count;
#else
            atomicAdd(counter_storage[address], count);
#endif
        }
    }
    barrier();
    if (local_index == 11u) {
        uint terrain_vertices = 0u;
        for (uint bucket = 0u; bucket < 6u; ++bucket)
            terrain_vertices += group_terrain_vertex_sums[bucket];
        uint counter_base = t3d_hot_records[T3D_HOT_INDEX].world_bases[16];
#if T3D_WORLD_SINGLE_WORKGROUP
        counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_TERRAIN_USEFUL_VERTICES] = terrain_vertices;
        uint overflow = 0u;
        for (uint bucket = 0u; bucket < 7u; ++bucket)
            overflow |= group_overflow[bucket];
        counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_OVERFLOW] = overflow;
#else
        if (terrain_vertices != 0u)
            atomicAdd(counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_TERRAIN_USEFUL_VERTICES], terrain_vertices);
#endif
    }
    barrier();
    if (visible) {
        uint lane_index = group_reservation[bucket] + group_rank[local_index];
        uint capacity = bucket < 6u
            ? terrain_bucket_capacity(bucket, frame.counts1.x)
            : t3d_world_nonterrain_visible_capacity(frame.counts0.y, frame.counts1.x);
        if (lane_index < capacity) {
            uint dst = bucket < 6u
                ? terrain_bucket_base(bucket) + lane_index
                : t3d_world_nonterrain_visible_base(frame.counts0.y, frame.counts1.x) + lane_index;
            visible_items[t3d_hot_records[T3D_HOT_INDEX].world_bases[15u] + dst] = item;
        }
    }
}
#endif // T3D_WORLD_VIS_SCAN_CLASSIFY
#else
void main() {
    uint index = gl_GlobalInvocationID.x;
    if (index >= push_data.control.x)
        return;
    T3DWorldVisibleItem item = candidates[t3d_hot_records[T3D_HOT_INDEX].world_bases[14u] + index];
    vec3 center;
    float radius;
    if (item.geometryKind == KIND_TERRAIN) {
        if (item.clusterOrTerrainId >= frame.counts0.y || (item.flags & push_data.control.y) == 0u)
            return;
        if (push_data.control.w == PASS_SHADOW && (item.instanceOrPatchId & TERRAIN_CAST_SHADOWS) == 0u)
            return;
        uint patch_id = item.instanceOrPatchId & ~TERRAIN_CAST_SHADOWS;
        vec4 bound = terrain_patch_bounds[patch_id];
        center = bound.xyz;
        radius = bound.w;
    } else {
        if (item.clusterOrTerrainId >= frame.counts0.x)
            return;
        InstanceRecord inst = instances[t3d_hot_records[T3D_HOT_INDEX].world_bases[9u] + item.instanceOrPatchId];
        if ((inst.b.x & push_data.control.y) == 0u || inst.a.z >= frame.counts0.z)
            return;
        T3DWorldTransform tr = transforms[t3d_hot_records[T3D_HOT_INDEX].world_bases[7u] + inst.a.z];
        T3DWorldCluster cl = clusters[t3d_hot_records[T3D_HOT_INDEX].world_bases[3u] + item.clusterOrTerrainId];
        if (push_data.control.w == PASS_SHADOW) {
            if ((item.geometryKind == KIND_MESH || item.geometryKind == KIND_HLOD) && (inst.b.y & INSTANCE_CAST_SHADOWS) == 0u)
                return;
            uint material_id = inst.material_cell.x != INVALID_ID ? inst.material_cell.x : cl.materialId;
            if (material_id != INVALID_ID && material_id < frame.counts0.w && (materials[t3d_hot_records[T3D_HOT_INDEX].world_bases[12u] + material_id].flags & MATERIAL_CAST_SHADOWS) == 0u)
                return;
        }
        center = transform_point(tr, cl.sphere.xyz);
        radius = cl.sphere.w * transform_radius_scale(tr);
        // ENV-P04: ground-cover vertex displacement is clamped to +/- 0.5m per axis.
        // Include the full sqrt(2)*0.5m envelope in frustum and HiZ culling.
        bool ground_cover = (push_data.control.z & 2u) != 0u &&
            item.geometryKind != KIND_STATIC && ((inst.b.y >> 26u) & 3u) == 1u;
        if (ground_cover) radius += 0.71;
        float distance_to_camera = length(frame.cameraAndMaxDistance.xyz - center);
        float lod_begin = inst.lod_range.x;
        float lod_end = inst.lod_range.y;
        float fade = max(inst.fade_margin, 0.0);
        if (lod_end > lod_begin && (distance_to_camera < max(0.0, lod_begin - fade) || distance_to_camera > lod_end + fade)) {
            atomicAdd(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_LOD_REJECT], 1u);
            return;
        }
        if (!ground_cover && !cone_visible(tr, cl, center)) {
            atomicAdd(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_CONE_REJECT], 1u);
            return;
        }
    }
    if (!sphere_frustum(center, radius)) {
        atomicAdd(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_FRUSTUM_REJECT], 1u);
        return;
    }
    if (hiz_occluded(center, radius)) {
        atomicAdd(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + T3D_WORLD_VIS_COUNTER_HIZ_REJECT], 1u);
        return;
    }
    append_visible(item);
}
#endif // T3D_WORLD_GROUPED_APPEND
#endif

// The prefix dispatch has exactly one workgroup. Each bucket gets a single
// lane-ordered writer, avoiding inter-workgroup atomics and preserving the
// existing raw (pre-capacity) bucket counts and rejection counters.
#ifdef T3D_WORLD_VIS_SCAN_PREFIX
layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer ScanFrameBuffer {
    T3DHotRecord t3d_hot_records[];
};
layout(set = 0, binding = 7, std430) buffer ScanCounterBuffer {
    uint counter_storage[];
};
void main() {
    uint lane = gl_LocalInvocationID.x;
    uint group_count = (push_data.control.x + 63u) / 64u;
    uint counter_base = t3d_hot_records[T3D_HOT_INDEX].world_bases[16];
    if (lane < 7u) {
        uint running = 0u;
        for (uint group = 0u; group < group_count; ++group) {
            uint source = T3D_WORLD_SCAN_COUNTS_BASE + group * 7u + lane;
            scan_scratch.words[T3D_WORLD_SCAN_PREFIX_BASE + group * 7u + lane] = running;
            running += scan_scratch.words[source];
        }
        uint counter = lane < 6u ? t3d_world_terrain_strip_bucket_counter(lane)
                                 : T3D_WORLD_VIS_COUNTER_NONTERRAIN;
        counter_storage[counter_base + counter] = running;
    } else if (lane < 11u) {
        uint total = 0u;
        for (uint group = 0u; group < group_count; ++group)
            total += scan_scratch.words[T3D_WORLD_SCAN_REJECT_BASE + group * 4u + lane - 7u];
        uint counter = lane == 7u ? T3D_WORLD_VIS_COUNTER_LOD_REJECT
                     : lane == 8u ? T3D_WORLD_VIS_COUNTER_CONE_REJECT
                     : lane == 9u ? T3D_WORLD_VIS_COUNTER_FRUSTUM_REJECT
                                   : T3D_WORLD_VIS_COUNTER_HIZ_REJECT;
        counter_storage[counter_base + counter] = total;
    } else if (lane == 11u) {
        uint overflow = 0u;
        for (uint bucket = 0u; bucket < 7u; ++bucket) {
            uint total = 0u;
            for (uint group = 0u; group < group_count; ++group)
                total += scan_scratch.words[T3D_WORLD_SCAN_COUNTS_BASE + group * 7u + bucket];
            uint capacity = bucket < 6u ? terrain_bucket_capacity(bucket, frame.counts1.x)
                : t3d_world_nonterrain_visible_capacity(frame.counts0.y, frame.counts1.x);
            if (total > capacity)
                overflow = 1u;
        }
        counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_OVERFLOW] = overflow;
    }
}
#endif

#ifdef T3D_WORLD_VIS_SCAN_SCATTER
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer ScatterFrameBuffer {
    T3DHotRecord t3d_hot_records[];
};
layout(set = 0, binding = 1, std430) readonly buffer ScatterCandidates {
    T3DWorldVisibleItem candidates[];
};
layout(set = 0, binding = 6, std430) buffer ScatterVisible {
    T3DWorldVisibleItem visible_items[];
};
shared uvec4 scan_accepted[64];
void main() {
    uint index = gl_GlobalInvocationID.x;
    uint lane = gl_LocalInvocationID.x;
    uvec4 contribution = uvec4(0u);
    if (index < push_data.control.x) {
        uint record = index * T3D_WORLD_SCAN_RECORD_WORDS;
        uint bucket = scan_scratch.words[record];
        if (bucket < 7u) {
            uint rank = scan_scratch.words[record + 1u];
            uint global_rank = scan_scratch.words[T3D_WORLD_SCAN_PREFIX_BASE + gl_WorkGroupID.x * 7u + bucket] + rank;
            uint capacity = bucket < 6u ? terrain_bucket_capacity(bucket, frame.counts1.x)
                : t3d_world_nonterrain_visible_capacity(frame.counts0.y, frame.counts1.x);
            if (global_rank < capacity) {
                uint dst = bucket < 6u ? terrain_bucket_base(bucket) + global_rank
                    : t3d_world_nonterrain_visible_base(frame.counts0.y, frame.counts1.x) + global_rank;
                T3DWorldVisibleItem item = candidates[t3d_hot_records[T3D_HOT_INDEX].world_bases[14u] + index];
                visible_items[t3d_hot_records[T3D_HOT_INDEX].world_bases[15u] + dst] = item;
                if (bucket < 6u)
                    contribution.x = scan_scratch.words[record + 2u];
                else if (item.geometryKind == 1u)
                    contribution.y = 1u;
                else if (item.geometryKind == 2u)
                    contribution.z = 1u;
                else if (item.geometryKind == 3u)
                    contribution.w = 1u;
            }
        }
    }
    scan_accepted[lane] = contribution;
    barrier();
    if (lane == 0u) {
        uvec4 total = uvec4(0u);
        for (uint i = 0u; i < 64u; ++i)
            total += scan_accepted[i];
        uint address = T3D_WORLD_SCAN_ACCEPTED_BASE + gl_WorkGroupID.x * 4u;
        scan_scratch.words[address] = total.x;
        scan_scratch.words[address + 1u] = total.y;
        scan_scratch.words[address + 2u] = total.z;
        scan_scratch.words[address + 3u] = total.w;
    }
}
#endif

#ifdef T3D_WORLD_VIS_FINALIZE
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) buffer CounterBuffer {
    uint counter_storage[];
};
layout(set = 0, binding = 1, std430) buffer IndirectBuffer {
    T3DWorldDrawIndirectArgs command_storage[];
};
layout(set = 0, binding = 2, std430) readonly buffer FrameBuffer {
    T3DHotRecord t3d_hot_records[];
};
void main() {
#ifdef T3D_WORLD_VIS_SCAN_FINALIZE
    uint group_count = (push_data.control.x + 63u) / 64u;
    uvec4 accepted = uvec4(0u);
    for (uint group = 0u; group < group_count; ++group) {
        uint address = T3D_WORLD_SCAN_ACCEPTED_BASE + group * 4u;
        accepted += uvec4(scan_scratch.words[address], scan_scratch.words[address + 1u],
                          scan_scratch.words[address + 2u], scan_scratch.words[address + 3u]);
    }
    uint base = t3d_hot_records[T3D_HOT_INDEX].world_bases[16];
    counter_storage[base + T3D_WORLD_VIS_COUNTER_TERRAIN_USEFUL_VERTICES] = accepted.x;
    counter_storage[base + T3D_WORLD_VIS_COUNTER_MESH] = accepted.y;
    counter_storage[base + T3D_WORLD_VIS_COUNTER_HLOD] = accepted.z;
    counter_storage[base + T3D_WORLD_VIS_COUNTER_STATIC] = accepted.w;
#endif
    uint submitted_vertices = 0u;
    uint accepted_terrain = 0u;
    for (uint bucket = 0u; bucket < T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT; ++bucket) {
        uint lane_capacity = terrain_bucket_capacity(bucket, frame.counts1.x);
        uint visible_count = min(counter_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[16] + t3d_world_terrain_strip_bucket_counter(bucket)], lane_capacity);
        uint vertex_count = t3d_world_terrain_strip_bucket_vertex_count(bucket);
        command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + bucket].vertexCount = vertex_count;
        command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + bucket].instanceCount = visible_count;
        command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + bucket].firstVertex = 0u;
        command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + bucket].firstInstance = terrain_bucket_base(bucket);
        submitted_vertices += visible_count * vertex_count;
        accepted_terrain += visible_count;
    }
    uint counter_base = t3d_hot_records[T3D_HOT_INDEX].world_bases[16];
    counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_TERRAIN] = accepted_terrain;
    counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_TERRAIN_SUBMITTED_VERTICES] = submitted_vertices;
    uint nonterrain_capacity = t3d_world_nonterrain_visible_capacity(frame.counts0.y, frame.counts1.x);
    uint accepted_nonterrain = min(counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_NONTERRAIN], nonterrain_capacity);
    counter_storage[counter_base + T3D_WORLD_VIS_COUNTER_VISIBLE] = accepted_terrain + accepted_nonterrain;
    command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].vertexCount = T3D_WORLD_CLUSTER_RASTER_VERTEX_COUNT;
    command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].instanceCount =
        accepted_nonterrain;
    command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].firstVertex = 0u;
    command_storage[t3d_hot_records[T3D_HOT_INDEX].world_bases[17] + T3D_WORLD_INDIRECT_NONTERRAIN_INDEX].firstInstance =
        t3d_world_nonterrain_visible_base(frame.counts0.y, frame.counts1.x);
}
#endif
