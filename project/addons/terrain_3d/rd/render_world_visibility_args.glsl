#[compute]
#version 450
#include "render_world_common_inc.glsl"
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    Frame f;
};
layout(set = 0, binding = 1, std430) readonly buffer CounterBuffer {
    Counters c;
};
layout(set = 0, binding = 2, std430) buffer ArgsBuffer {
    DrawArgs args[3];
};
void main() {
    uint caps[3] = uint[3](f.capacities0.z, f.capacities0.w, f.capacities1.x);
    for (uint i = 0u; i < 3u; ++i) {
        args[i].vertexCount = 372u;
        args[i].instanceCount = min(c.visibleClusterCount[i], caps[i]);
        args[i].firstVertex = 0u;
        args[i].firstInstance = 0u;
    }
}
