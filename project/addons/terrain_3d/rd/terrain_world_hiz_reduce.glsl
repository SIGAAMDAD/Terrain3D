#[compute]
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1, r32f) uniform writeonly image2D destination_depth;
layout(push_constant, std430) uniform HiZReduceData {
    uint source_width;
    uint source_height;
    uint destination_width;
    uint destination_height;
}
reduce_data;
void main() {
    uvec2 destination = gl_GlobalInvocationID.xy;
    uvec2 destination_size = uvec2(max(reduce_data.destination_width, 1u), max(reduce_data.destination_height, 1u));
    if (any(greaterThanEqual(destination, destination_size)))
        return;
    uvec2 source_size = uvec2(max(reduce_data.source_width, 1u), max(reduce_data.source_height, 1u));
    // The common even 2:1 mip reduction never needs variable integer
    // division. Preserve the original scan order and reverse-Z minimum.
    float depth_value = 1.0;
    if (all(equal(source_size, destination_size))) {
        // Mip zero copies the previous frame depth without reducing it.
        // The general path otherwise performs four integer divisions per pixel.
        depth_value = texelFetch(source_depth, ivec2(destination), 0).r;
    } else if (all(equal(source_size, destination_size * 2u))) {
        ivec2 first = ivec2(destination * 2u);
        depth_value = min(depth_value, texelFetch(source_depth, first, 0).r);
        depth_value = min(depth_value, texelFetch(source_depth, first + ivec2(1, 0), 0).r);
        depth_value = min(depth_value, texelFetch(source_depth, first + ivec2(0, 1), 0).r);
        depth_value = min(depth_value, texelFetch(source_depth, first + ivec2(1, 1), 0).r);
    } else {
        uvec2 source_begin = (destination * source_size) / destination_size;
        uvec2 source_end = ((destination + uvec2(1u)) * source_size + destination_size - uvec2(1u)) / destination_size;
        source_end = min(max(source_end, source_begin + uvec2(1u)), source_size);
        for (uint y = source_begin.y; y < source_end.y; ++y)
            for (uint x = source_begin.x; x < source_end.x; ++x)
                depth_value = min(depth_value, texelFetch(source_depth, ivec2(x, y), 0).r);
    }
    imageStore(destination_depth, ivec2(destination), vec4(depth_value));
}
