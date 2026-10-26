// T3D_P21_RETIRED_SOURCE_ONLY
// WG-D SH06: retired reset groundwork. This file has zero production C++ loads and MUST NOT be treated as feature reachability.
// Production RenderWorld VT reset/predict/compact is project/addons/terrain_3d/rd/render_world_vt_*.glsl, owned by Terrain3DVTGpu.
#[compute]
#version 450
#include "terrain_vt_common_inc.glslinc"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform VtPredictionDispatch {
    uvec4 limits;
    vec4 cameraLinearAndHorizon;
    vec4 cameraAngularAndViewport;
    uvec4 resources;
}
vt_push;

layout(set = 0, binding = 0, std430) buffer VtHashBuffer {
    uvec4 entries[];
}
vt_request_hash;
layout(set = 0, binding = 1, std430) buffer VtCounterBuffer {
    uint words[];
}
vt_request_counters;

void main() {
    uint index = gl_GlobalInvocationID.x;
    uint hash_capacity = vt_push.limits.x;
    if (index < hash_capacity) {
        vt_request_hash.entries[index] = uvec4(T3D_VT_REQUEST_HASH_EMPTY_KEY, 0xffffu, 0u, 0u);
    }
    if (index == 0u) {
        for (uint word = 0u; word < T3D_VT_REQUEST_COUNTER_WORDS; ++word) {
            vt_request_counters.words[word] = 0u;
        }
        vt_request_counters.words[T3D_VT_COUNTER_FRAME_ID] = vt_push.limits.z;
        vt_request_counters.words[T3D_VT_COUNTER_GENERATION] = vt_push.limits.w;
        vt_request_counters.words[T3D_VT_COUNTER_HASH_CAPACITY] = hash_capacity;
        vt_request_counters.words[T3D_VT_COUNTER_REQUEST_CAPACITY] = vt_push.limits.y;
    }
}
