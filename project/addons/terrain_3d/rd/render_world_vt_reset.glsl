#[compute]
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(push_constant, std430) uniform VtDispatchPush {
    uint tier;
    uint hashCount;
    uint requestCapacity;
    uint reserved;
}
pc;
layout(set = 0, binding = 0, std430) buffer VtCounterBuffer {
    uint words[];
}
counters;
layout(set = 0, binding = 1, std430) buffer VtHashBuffer {
    uvec4 entries[];
}
hash_table;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < pc.hashCount)
        hash_table.entries[i] = uvec4(0xffffffffu, 0xffffu, 0u, 0u);
    if (i == 0u)
        for (uint c = 16u; c < 24u; ++c)
            counters.words[c] = 0u;
}
