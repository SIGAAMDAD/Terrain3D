#[compute]
#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) restrict readonly buffer HeightHalo { float heights[]; };
layout(set = 0, binding = 1, std430) restrict writeonly buffer DerivedMap { uint rgba8[]; };
layout(push_constant, std430) uniform Params {
    uint width;
    uint height;
    uint halo_stride;
    float vertex_spacing;
} params;

void main() {
    uint index = gl_GlobalInvocationID.x;
    uint count = params.width * params.height;
    if (index >= count) return;
    uint x = index % params.width;
    uint y = index / params.width;
    uint center_index = (y + 1u) * params.halo_stride + (x + 1u);
    float center = heights[center_index];
    float right = heights[center_index + 1u];
    float down = heights[center_index + params.halo_stride];
    if (isnan(center) || isinf(center)) center = 0.0;
    if (isnan(right) || isinf(right)) right = center;
    if (isnan(down) || isinf(down)) down = center;
    vec3 normal = normalize(vec3(center - right, params.vertex_spacing, center - down));
    float slope = acos(clamp(normal.y, -1.0, 1.0)) / 1.5707963267948966;
    rgba8[index] = packUnorm4x8(vec4(normal * 0.5 + 0.5, clamp(slope, 0.0, 1.0)));
}
