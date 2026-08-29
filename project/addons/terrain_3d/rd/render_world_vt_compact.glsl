#[compute]
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer Hash {
    uvec4 entries[];
}
hash_table;
layout(set = 0, binding = 1, std430) buffer Requests {
    uvec2 entries[];
}
requests;
layout(set = 0, binding = 2, std430) buffer Counters {
    uint words[];
}
counters;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= hash_table.entries.length())
        return;
    uvec4 h = hash_table.entries[i];
    if (h.x == 0xffffffffu)
        return;
    uint dst = atomicAdd(counters.words[16], 1u);
    if (dst < requests.entries.length())
        requests.entries[dst] = uvec2(h.x, (min(h.y, 65535u) & 0xffffu) | (min(h.z, 65535u) << 16u));
    else
        atomicAdd(counters.words[20], 1u);
}
