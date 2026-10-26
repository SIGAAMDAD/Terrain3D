// T3D_P21_RETIRED_SOURCE_ONLY
// WG-D SH06: retired instance predictor groundwork. This file has zero production C++ loads and MUST NOT be treated as feature reachability.
// Production RenderWorld VT reset/predict/compact is project/addons/terrain_3d/rd/render_world_vt_*.glsl, owned by Terrain3DVTGpu.
#[compute]
#version 450
#include "terrain_vt_common_inc.glslinc"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct InstanceFrame {
    mat4 viewProjection[2];
    mat4 viewMatrix;
    mat4 previousViewProjection[2];
    vec4 cameraPosition[2];
    vec4 motionJitter;
    vec4 motionParams;
};

struct IndexedIndirectCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

layout(push_constant, std430) uniform VtPredictionDispatch {
    uvec4 limits;
    vec4 cameraLinearAndHorizon;
    vec4 cameraAngularAndViewport;
    uvec4 resources;
}
vt_push;

layout(set = 0, binding = 0, std140) uniform InstanceFrameBuffer {
    InstanceFrame frame_data;
};
layout(set = 0, binding = 1, std430) readonly buffer PredictionMetadataBuffer {
    T3DVTPredictionMetadata values[];
}
vt_prediction_metadata;
layout(set = 0, binding = 2, std430) readonly buffer IndirectCommandBuffer {
    IndexedIndirectCommand commands[];
}
visible_draws;
layout(set = 0, binding = 3, std430) readonly buffer VtAllocationBuffer {
    T3DVTAllocation values[];
}
vt_allocations;
layout(set = 0, binding = 4, std430) readonly buffer VtPageTableBuffer {
    T3DVTPageEntry entries[];
}
vt_page_table;
layout(set = 0, binding = 5, std430) buffer VtHashBuffer {
    uvec4 entries[];
}
vt_request_hash;
layout(set = 0, binding = 6, std430) buffer VtCounterBuffer {
    uint words[];
}
vt_request_counters;

#include "terrain_vt_feedback_inc.glslinc"

uint t3d_vt_instance_geometry_lod(T3DVTPredictionMetadata metadata) {
    return metadata.packedLodCategoryBias & 0xffffu;
}

uint t3d_vt_instance_category(T3DVTPredictionMetadata metadata) {
    return (metadata.packedLodCategoryBias >> 16u) & 0xffu;
}

int t3d_vt_instance_authored_mip_bias(T3DVTPredictionMetadata metadata) {
    uint packed_bias = (metadata.packedLodCategoryBias >> 24u) & 0xffu;
    return packed_bias >= 128u ? int(packed_bias) - 256 : int(packed_bias);
}

float t3d_vt_instance_priority_scale(uint category) {
    if (category == T3D_VT_PREDICTION_CATEGORY_CLUTTER) {
        return 0.60;
    }
    if (category == T3D_VT_PREDICTION_CATEGORY_GROUND_COVER) {
        return 0.85;
    }
    if (category == T3D_VT_PREDICTION_CATEGORY_OCCLUDER) {
        return 1.35;
    }
    return 1.10;
}

float t3d_vt_projected_pixels(vec3 center, float radius, vec3 camera_position) {
    float distance_to_surface = max(0.25, length(center - camera_position) - radius);
    float projection_scale = max(abs(frame_data.viewProjection[0][1][1]), 0.001);
    float viewport_height = max(vt_push.cameraAngularAndViewport.z, 1.0);
    return max(1.0, radius * projection_scale * viewport_height / distance_to_surface);
}

void t3d_vt_predict_instance_allocation(uint allocation_id,
    vec2 uv_min,
    vec2 uv_max,
    float world_diameter,
    float projected_pixels,
    float texel_density,
    uint geometry_lod,
    uint category,
    int authored_bias,
    uint priority,
    uint producer_mask,
    bool overlay) {
    if (allocation_id == T3D_VT_INVALID_ALLOCATION || allocation_id >= vt_push.resources.y) {
        return;
    }

    // Authored bias is applied by reducing/increasing the projected footprint;
    // category and selected geometry LOD remain inside the common mip selector.
    float biased_pixels = projected_pixels * exp2(float(-authored_bias));
    t3d_vt_prediction_request_allocation(allocation_id,
        uv_min,
        uv_max,
        world_diameter,
        biased_pixels,
        texel_density,
        geometry_lod,
        category,
        priority,
        producer_mask,
        overlay,
        vt_push.limits.x,
        vt_push.resources.y,
        vt_push.resources.z);
}

void main() {
    uint draw_slot = gl_GlobalInvocationID.x;
    if (draw_slot >= vt_push.resources.x) {
        return;
    }

    IndexedIndirectCommand command = visible_draws.commands[draw_slot];
    if (command.instanceCount == 0u) {
        return;
    }

    T3DVTPredictionMetadata metadata = vt_prediction_metadata.values[draw_slot];
    if (metadata.ids.w != draw_slot) {
        return;
    }

    uint geometry_lod = t3d_vt_instance_geometry_lod(metadata);
    uint category = t3d_vt_instance_category(metadata);
    int authored_bias = t3d_vt_instance_authored_mip_bias(metadata);
    float object_scale = max(metadata.densityScale.y, 0.001);
    float radius = max(metadata.worldSphere.w * object_scale, 0.001);
    float world_diameter = radius * 2.0;
    float texel_density = max(metadata.densityScale.x, 0.000001);
    vec2 uv_min = metadata.uvBounds.xy;
    vec2 uv_max = metadata.uvBounds.zw;
    vec3 camera_position = frame_data.cameraPosition[0].xyz;
    float projected_pixels = t3d_vt_projected_pixels(metadata.worldSphere.xyz, radius, camera_position);
    float priority_scale = t3d_vt_instance_priority_scale(category);
    uint visible_priority = uint(clamp(projected_pixels * 32.0 * priority_scale, 1.0, 65535.0));

    t3d_vt_predict_instance_allocation(metadata.ids.x,
        uv_min,
        uv_max,
        world_diameter,
        projected_pixels,
        texel_density,
        geometry_lod,
        category,
        authored_bias,
        visible_priority,
        T3D_VT_REQUEST_PRODUCER_VISIBLE,
        false);
    t3d_vt_predict_instance_allocation(metadata.ids.y,
        uv_min,
        uv_max,
        world_diameter,
        projected_pixels,
        texel_density,
        geometry_lod,
        category,
        authored_bias,
        visible_priority,
        T3D_VT_REQUEST_PRODUCER_VISIBLE,
        true);

    float horizon = max(vt_push.cameraLinearAndHorizon.w, 0.0);
    float future_priority_scale = clamp(vt_push.cameraAngularAndViewport.w, 0.0, 0.95);
    if (horizon <= 0.0 || future_priority_scale <= 0.0) {
        return;
    }

    vec3 future_camera = camera_position + vt_push.cameraLinearAndHorizon.xyz * horizon;
    float angular_expand = 1.0 + max(vt_push.cameraAngularAndViewport.x, 0.0) * horizon;
    float future_pixels = t3d_vt_projected_pixels(metadata.worldSphere.xyz, radius * angular_expand, future_camera);
    float motion_ratio = clamp((length(vt_push.cameraLinearAndHorizon.xyz) * horizon) / max(world_diameter, 0.001)
            + max(vt_push.cameraAngularAndViewport.x, 0.0) * horizon,
        0.0,
        0.25);
    vec2 uv_expand = vec2(motion_ratio);
    uint future_priority = uint(clamp(float(visible_priority) * future_priority_scale, 1.0, float(max(visible_priority - 1u, 1u))));

    t3d_vt_predict_instance_allocation(metadata.ids.x,
        uv_min - uv_expand,
        uv_max + uv_expand,
        world_diameter,
        future_pixels * 0.5,
        texel_density,
        geometry_lod,
        category,
        authored_bias + 1,
        future_priority,
        T3D_VT_REQUEST_PRODUCER_FUTURE,
        false);
    t3d_vt_predict_instance_allocation(metadata.ids.y,
        uv_min - uv_expand,
        uv_max + uv_expand,
        world_diameter,
        future_pixels * 0.5,
        texel_density,
        geometry_lod,
        category,
        authored_bias + 1,
        future_priority,
        T3D_VT_REQUEST_PRODUCER_FUTURE,
        true);
}
