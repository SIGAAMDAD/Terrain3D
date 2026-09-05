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
layout(set = 0, binding = 0, std430) readonly buffer VtHashBuffer {
    uvec4 entries[];
}
hash_table;
layout(set = 0, binding = 1, std430) buffer VtRequestBuffer {
    uvec2 entries[];
}
requests;
layout(set = 0, binding = 2, std430) buffer VtCounterBuffer {
    uint words[];
}
counters;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.hashCount)
        return;
    uvec4 h = hash_table.entries[i];
    if (h.x == 0xffffffffu)
        return;
    uint dst = atomicAdd(counters.words[16], 1u);
    if (dst < pc.requestCapacity)
        requests.entries[dst] = uvec2(h.x, (min(h.y, 65535u) & 0xffffu) | (min(h.z, 65535u) << 16u));
    else
        atomicAdd(counters.words[20], 1u);
}
