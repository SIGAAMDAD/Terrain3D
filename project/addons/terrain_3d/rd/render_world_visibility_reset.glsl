#[compute]
#version 450
#include "render_world_common_inc.glsl"
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) buffer CounterBuffer {
    Counters c;
};
layout(set = 0, binding = 1, std430) buffer ArgsBuffer {
    DrawArgs args[3];
};
void main() {
    c.visibleCellCount = 0u;
    c.visibleInstanceCount = 0u;
    c.visibleClusterCount[0] = 0u;
    c.visibleClusterCount[1] = 0u;
    c.visibleClusterCount[2] = 0u;
    c.candidateClusterCount = 0u;
    c.cellRejectCount = 0u;
    c.instanceRejectCount = 0u;
    c.clusterFrustumRejectCount = 0u;
    c.clusterConeRejectCount = 0u;
    c.clusterHiZRejectCount = 0u;
    c.overflowBits = 0u;
    c.fallbackCount = 0u;
    c.reserved0 = 0u;
    c.reserved1 = 0u;
    c.reserved2 = 0u;
    for (uint i = 0u; i < 3u; ++i) {
        args[i].vertexCount = 372u;
        args[i].instanceCount = 0u;
        args[i].firstVertex = 0u;
        args[i].firstInstance = 0u;
    }
}
