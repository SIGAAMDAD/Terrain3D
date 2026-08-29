#[compute]
#version 450
#include "render_world_common_inc.glsl"
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
const uint FLAG_FRUSTUM = 1u;
const uint FLAG_DISTANCE = 2u;
const uint OVERFLOW_CELLS = 1u;
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    Frame f;
};
layout(set = 0, binding = 1, std430) readonly buffer CellBuffer {
    Cell cells[];
};
layout(set = 0, binding = 2, std430) buffer VisibleCellBuffer {
    uint visibleCells[];
};
layout(set = 0, binding = 3, std430) buffer CounterBuffer {
    Counters c;
};
bool sphere_frustum(vec3 center, float radius) {
    if ((f.counts0.y & FLAG_FRUSTUM) == 0u)
        return true;
    for (uint i = 0u; i < 6u; ++i) {
        vec4 p = f.frustumPlanes[i];
        if (dot(p.xyz, center) + p.w < -radius * length(p.xyz))
            return false;
    }
    return true;
}
bool sphere_distance(vec3 center, float radius) {
    if ((f.counts0.y & FLAG_DISTANCE) == 0u)
        return true;
    return distance(center, f.cameraAndMaxDistance.xyz) - radius <= f.cameraAndMaxDistance.w;
}
void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= f.counts0.w)
        return;
    Cell cell = cells[id];
    if (!sphere_frustum(cell.sphere.xyz, cell.sphere.w) || !sphere_distance(cell.sphere.xyz, cell.sphere.w)) {
        atomicAdd(c.cellRejectCount, 1u);
        return;
    }
    uint dst = atomicAdd(c.visibleCellCount, 1u);
    if (dst < f.capacities0.x)
        visibleCells[dst] = id;
    else
        atomicOr(c.overflowBits, OVERFLOW_CELLS);
}
