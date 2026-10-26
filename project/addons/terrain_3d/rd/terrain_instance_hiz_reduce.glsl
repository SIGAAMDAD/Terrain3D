#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform sampler2D proxy_depth;
layout(set = 0, binding = 2, r32f) uniform writeonly image2D destination_depth;

layout(push_constant, std430) uniform HiZReduceData {
    uint source_width;
    uint source_height;
    uint proxy_width;
    uint proxy_height;
    uint destination_width;
    uint destination_height;
    uint combine_proxy;
    uint reserved0;
}
reduce_data;

void main() {
    uvec2 destination = gl_GlobalInvocationID.xy;
    uvec2 destination_size = uvec2(max(reduce_data.destination_width, 1u), max(reduce_data.destination_height, 1u));
    if (any(greaterThanEqual(destination, destination_size)))
        return;

    uvec2 source_size = uvec2(max(reduce_data.source_width, 1u), max(reduce_data.source_height, 1u));
    uvec2 source_begin = (destination * source_size) / destination_size;
    uvec2 source_end = ((destination + uvec2(1u)) * source_size + destination_size - uvec2(1u)) / destination_size;
    source_end = max(source_end, source_begin + uvec2(1u));
    source_end = min(source_end, source_size);

    float depth_value = 1.0;
    for (uint y = source_begin.y; y < source_end.y; ++y) {
        for (uint x = source_begin.x; x < source_end.x; ++x) {
            float source_value = texelFetch(source_depth, ivec2(x, y), 0).r;
            if (reduce_data.combine_proxy != 0u) {
                uvec2 proxy_size = uvec2(max(reduce_data.proxy_width, 1u), max(reduce_data.proxy_height, 1u));
                uvec2 proxy_coord = min((uvec2(x, y) * proxy_size) / source_size, proxy_size - uvec2(1u));
                float proxy_value = texelFetch(proxy_depth, ivec2(proxy_coord), 0).r;
                source_value = max(source_value, proxy_value);
            }
            depth_value = min(depth_value, source_value);
        }
    }
    imageStore(destination_depth, ivec2(destination), vec4(depth_value));
}
