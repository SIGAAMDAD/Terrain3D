// T3D_P21_RETIRED_SOURCE_ONLY
// WG-D SH06: retired compact groundwork. This file has zero production C++ loads and MUST NOT be treated as feature reachability.
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

layout(set = 0, binding = 0, std430) readonly buffer VtHashBuffer {
    uvec4 entries[];
}
vt_request_hash;
layout(set = 0, binding = 1, std430) buffer VtRequestBuffer {
    T3DVTRequest entries[];
}
vt_requests;
layout(set = 0, binding = 2, std430) buffer VtCounterBuffer {
    uint words[];
}
vt_request_counters;

void main() {
    uint index = gl_GlobalInvocationID.x;
    if (index >= vt_push.limits.x) {
        return;
    }

    uvec4 entry = vt_request_hash.entries[index];
    if (entry.x == T3D_VT_REQUEST_HASH_EMPTY_KEY) {
        return;
    }

    uint destination = atomicAdd(vt_request_counters.words[T3D_VT_COUNTER_COMPACTED_REQUEST_COUNT], 1u);
    if (destination < vt_push.limits.y) {
        vt_requests.entries[destination].virtualPage = entry.x;
        vt_requests.entries[destination].packedWantedMipPriority = (min(entry.y, 0xffffu) & 0xffffu) | (min(entry.z, 0xffffu) << 16u);
        return;
    }

    atomicAdd(vt_request_counters.words[T3D_VT_COUNTER_REQUEST_LIST_OVERFLOW_COUNT], 1u);
}
