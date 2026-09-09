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
    uint words[];
}
indirect_args;

void write_dispatch(uint wordOffset, uint count) {
    indirect_args.words[wordOffset + 0u] = (count + 63u) >> 6u;
    indirect_args.words[wordOffset + 1u] = 1u;
    indirect_args.words[wordOffset + 2u] = 1u;
}

void write_draw(uint wordOffset, uint instanceCount) {
    indirect_args.words[wordOffset + 0u] = 372u;
    indirect_args.words[wordOffset + 1u] = instanceCount;
    indirect_args.words[wordOffset + 2u] = 0u;
    indirect_args.words[wordOffset + 3u] = 0u;
}

void main() {
    uint caps[3] = uint[3](f.capacities0.z, f.capacities0.w, f.capacities1.x);
    write_draw(0u, min(c.visibleClusterCount[0], caps[0]));
    write_draw(4u, min(c.visibleClusterCount[1], caps[1]));
    write_draw(8u, min(c.visibleClusterCount[2], caps[2]));

    // Byte 48 / word 12: visible-cell driven instance stage.
    write_dispatch(12u, min(c.visibleCellCount, f.capacities0.x));
    // Byte 60 / word 15: visible-instance driven cluster stage.
    write_dispatch(15u, min(c.visibleInstanceCount, f.capacities0.y));
    // Bytes 72/84/96: VT tiers driven by live compacted visible clusters.
    write_dispatch(18u, min(c.visibleClusterCount[0], caps[0]));
    write_dispatch(21u, min(c.visibleClusterCount[1], caps[1]));
    write_dispatch(24u, min(c.visibleClusterCount[2], caps[2]));
}
