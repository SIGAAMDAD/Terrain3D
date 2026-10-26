// T3D_P21_RETIRED_SOURCE_ONLY
// WG-D SH06: retired RenderWorld predictor groundwork. This file has zero production C++ loads and MUST NOT be treated as feature reachability.
// Production RenderWorld VT reset/predict/compact is project/addons/terrain_3d/rd/render_world_vt_*.glsl, owned by Terrain3DVTGpu.
#[compute]
#version 450
#include "../render_world_common_inc.glslinc"
#include "terrain_vt_common_inc.glslinc"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform VtPredictionDispatch {
    uvec4 limits;
    vec4 cameraLinearAndHorizon;
    vec4 cameraAngularAndViewport;
    uvec4 resources;
}
vt_push;

layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    Frame frame_data;
};
layout(set = 0, binding = 1, std430) readonly buffer VisibleClusterBuffer {
    uvec2 visible[];
}
visible_clusters;
layout(set = 0, binding = 2, std430) readonly buffer ClusterBuffer {
    Cluster values[];
}
clusters;
layout(set = 0, binding = 3, std430) readonly buffer InstanceBuffer {
    Instance values[];
}
instances;
layout(set = 0, binding = 4, std430) readonly buffer MaterialBuffer {
    T3DRenderingMaterialGpu values[];
}
vt_materials;
layout(set = 0, binding = 5, std430) readonly buffer VtAllocationBuffer {
    T3DVTAllocation values[];
}
vt_allocations;
layout(set = 0, binding = 6, std430) readonly buffer VtPageTableBuffer {
    T3DVTPageEntry entries[];
}
vt_page_table;
layout(set = 0, binding = 7, std430) buffer VtHashBuffer {
    uvec4 entries[];
}
vt_request_hash;
layout(set = 0, binding = 8, std430) buffer VtCounterBuffer {
    uint words[];
}
vt_request_counters;
layout(set = 0, binding = 9, std430) readonly buffer VisibilityCounterBuffer {
    uint words[];
}
visibility_counters;

#include "terrain_vt_feedback_inc.glslinc"

uint t3d_vt_render_world_category(uint tier) {
    if (tier == 1u) {
        return T3D_VT_PREDICTION_CATEGORY_CLUTTER;
    }
    if (tier == 2u) {
        return T3D_VT_PREDICTION_CATEGORY_GROUND_COVER;
    }
    return T3D_VT_PREDICTION_CATEGORY_OCCLUDER;
}

float t3d_vt_render_world_priority_scale(uint category) {
    if (category == T3D_VT_PREDICTION_CATEGORY_CLUTTER) {
        return 0.60;
    }
    if (category == T3D_VT_PREDICTION_CATEGORY_GROUND_COVER) {
        return 0.85;
    }
    return 1.35;
}

float t3d_vt_render_world_projected_pixels(vec3 center, float radius, vec3 camera_position) {
    float distance_to_surface = max(0.25, length(center - camera_position) - radius);
    float projection_scale = max(abs(frame_data.viewProjection[1][1]), 0.001);
    float viewport_height = max(vt_push.cameraAngularAndViewport.z, max(frame_data.viewportAndMotion.y, 1.0));
    return max(1.0, radius * projection_scale * viewport_height / distance_to_surface);
}

void main() {
    uint index = gl_GlobalInvocationID.x;
    uint tier = vt_push.resources.w & 0xffu;
    uint actual_count = visibility_counters.words[2u + tier];
    uint visible_capacity = tier == 0u ? frame_data.capacities0.z : (tier == 1u ? frame_data.capacities0.w : frame_data.capacities1.x);
    if (index >= actual_count || index >= visible_capacity) {
        return;
    }

    uvec2 visible = visible_clusters.visible[index];
    if (visible.x >= frame_data.counts1.w || visible.y >= frame_data.counts1.x) {
        return;
    }

    Cluster cluster = clusters.values[visible.x];
    Instance instance = instances.values[visible.y];
    uint material_id = instance.ids.y != 0xffffffffu ? instance.ids.y : cluster.materialId;
    if (material_id >= frame_data.resourceCounts.x) {
        return;
    }
    T3DRenderingMaterialGpu material = vt_materials.values[material_id];

    vec3 local_center = cluster.sphere.xyz;
    vec3 world_center = vec3(dot(instance.row0.xyz, local_center) + instance.row0.w,
        dot(instance.row1.xyz, local_center) + instance.row1.w,
        dot(instance.row2.xyz, local_center) + instance.row2.w);
    float max_scale = max(length(instance.row0.xyz), max(length(instance.row1.xyz), length(instance.row2.xyz)));
    float world_radius = max(cluster.sphere.w * max(max_scale, 0.0001), 0.001);
    float world_diameter = world_radius * 2.0;
    float projected_pixels = t3d_vt_render_world_projected_pixels(world_center, world_radius, frame_data.cameraAndMaxDistance.xyz);
    vec2 cluster_uv_min = unpackHalf2x16(cluster.packedUvMin);
    vec2 cluster_uv_max = unpackHalf2x16(cluster.packedUvMax);
    vec2 uv0 = cluster_uv_min * material.uvScaleBias.xy + material.uvScaleBias.zw;
    vec2 uv1 = cluster_uv_max * material.uvScaleBias.xy + material.uvScaleBias.zw;
    vec2 uv_min = min(uv0, uv1);
    vec2 uv_max = max(uv0, uv1);
    float texel_density = max(uintBitsToFloat(cluster.texelDensity), 0.000001);
    uint geometry_lod = cluster.packedLodTier & 0xffffu;
    uint category = t3d_vt_render_world_category(tier);
    uint visible_priority = uint(clamp(projected_pixels * 32.0 * t3d_vt_render_world_priority_scale(category), 1.0, 65535.0));

    t3d_vt_prediction_request_allocation(material.vtAllocation,
        uv_min,
        uv_max,
        world_diameter,
        projected_pixels,
        texel_density,
        geometry_lod,
        category,
        visible_priority,
        T3D_VT_REQUEST_PRODUCER_VISIBLE,
        false,
        vt_push.limits.x,
        vt_push.resources.y,
        vt_push.resources.z);
    t3d_vt_prediction_request_allocation(material.overlayVtAllocation,
        uv_min,
        uv_max,
        world_diameter,
        projected_pixels,
        texel_density,
        geometry_lod,
        category,
        visible_priority,
        T3D_VT_REQUEST_PRODUCER_VISIBLE,
        true,
        vt_push.limits.x,
        vt_push.resources.y,
        vt_push.resources.z);

    float horizon = max(vt_push.cameraLinearAndHorizon.w, 0.0);
    float future_priority_scale = clamp(vt_push.cameraAngularAndViewport.w, 0.0, 0.95);
    if (horizon <= 0.0 || future_priority_scale <= 0.0) {
        return;
    }

    vec3 future_camera = frame_data.cameraAndMaxDistance.xyz + vt_push.cameraLinearAndHorizon.xyz * horizon;
    float angular_expand = 1.0 + max(vt_push.cameraAngularAndViewport.x, 0.0) * horizon;
    float future_pixels = t3d_vt_render_world_projected_pixels(world_center, world_radius * angular_expand, future_camera);
    float motion_ratio = clamp((length(vt_push.cameraLinearAndHorizon.xyz) * horizon) / max(world_diameter, 0.001)
            + max(vt_push.cameraAngularAndViewport.x, 0.0) * horizon,
        0.0,
        0.25);
    vec2 uv_expand = vec2(motion_ratio);
    uint future_priority = uint(clamp(float(visible_priority) * future_priority_scale, 1.0, float(max(visible_priority - 1u, 1u))));

    t3d_vt_prediction_request_allocation(material.vtAllocation,
        uv_min - uv_expand,
        uv_max + uv_expand,
        world_diameter,
        future_pixels * 0.5,
        texel_density,
        geometry_lod,
        category,
        future_priority,
        T3D_VT_REQUEST_PRODUCER_FUTURE,
        false,
        vt_push.limits.x,
        vt_push.resources.y,
        vt_push.resources.z);
    t3d_vt_prediction_request_allocation(material.overlayVtAllocation,
        uv_min - uv_expand,
        uv_max + uv_expand,
        world_diameter,
        future_pixels * 0.5,
        texel_density,
        geometry_lod,
        category,
        future_priority,
        T3D_VT_REQUEST_PRODUCER_FUTURE,
        true,
        vt_push.limits.x,
        vt_push.resources.y,
        vt_push.resources.z);
}
