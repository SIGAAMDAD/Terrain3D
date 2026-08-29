#[compute]
#version 450
#include "render_world_common_inc.glsl"
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(push_constant, std430) uniform TierPush {
    uint tier;
}
pc;
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    Frame f;
};
layout(set = 0, binding = 1, std430) readonly buffer Visible {
    uvec2 visible[];
}
visible_clusters;
layout(set = 0, binding = 2, std430) readonly buffer Clusters {
    Cluster values[];
}
clusters;
layout(set = 0, binding = 3, std430) readonly buffer Instances {
    Instance values[];
}
instances;
layout(set = 0, binding = 4, std430) readonly buffer Materials {
    Material values[];
}
materials;
layout(set = 0, binding = 5, std430) readonly buffer Allocations {
    Allocation values[];
}
allocations;
layout(set = 0, binding = 6, std430) readonly buffer PageTable {
    PageEntry entries[];
}
page_table;
layout(set = 0, binding = 7, std430) buffer Hash {
    uvec4 entries[];
}
hash_table;
layout(set = 0, binding = 8, std430) buffer Counters {
    uint words[];
}
counters;

uint mip_extent(uint v, uint mip) {
    for (uint m = 0u; m < mip; ++m)
        v = v > 1u ? (v + 1u) >> 1u : 1u;
    return max(v, 1u);
}
uint mip_base_page(Allocation a, uint mip) {
    uint page = a.basePage;
    uint w = a.mip0Width;
    uint h = a.mip0Height;
    for (uint m = 0u; m < mip; ++m) {
        page += w * h;
        w = w > 1u ? (w + 1u) >> 1u : 1u;
        h = h > 1u ? (h + 1u) >> 1u : 1u;
    }
    return page;
}
bool page_is_resident(uint page) {
    if (page >= page_table.entries.length())
        return false;
    PageEntry e = page_table.entries[page];
    uint flags = e.packedMipFlags >> 16u;
    return (flags & 2u) != 0u && e.physicalSlot != 0xffffffffu;
}
void request_page(uint page, uint wantedMip, uint priority) {
    if (page >= page_table.entries.length() || page_is_resident(page))
        return;
    uint hashCount = hash_table.entries.length();
    if (hashCount == 0u)
        return;
    for (uint probe = 0u; probe < 8u; ++probe) {
        uint slot = (page * 2654435761u + probe) % hashCount;
        uint oldKey = atomicCompSwap(hash_table.entries[slot].x, 0xffffffffu, page);
        if (oldKey == 0xffffffffu || oldKey == page) {
            atomicMin(hash_table.entries[slot].y, wantedMip);
            atomicMax(hash_table.entries[slot].z, priority);
            atomicAdd(counters.words[18], 1u);
            return;
        }
    }
    atomicAdd(counters.words[19], 1u);
}
void request_allocation(
    uint allocationId, vec2 uvMinIn, vec2 uvMaxIn, float worldDiameter, float projectedPixels, float texelDensity, uint priority) {
    if (allocationId == 0xffffffffu || allocationId >= allocations.values.length())
        return;
    Allocation a = allocations.values[allocationId];
    uint mipCount = a.packedMipChannel & 0xffffu;
    if (mipCount == 0u || a.pageCount == 0u || a.mip0Width == 0u || a.mip0Height == 0u)
        return;

    float virtualExtent = float(max(a.mip0Width, a.mip0Height) * 128u);
    float sourceTexelsAcross = max(1.0, texelDensity * max(worldDiameter, 0.001) * virtualExtent);
    uint mip = uint(clamp(floor(log2(max(1.0, sourceTexelsAcross / max(projectedPixels, 1.0)))), 0.0, float(mipCount - 1u)));

    vec2 uvMin = min(uvMinIn, uvMaxIn);
    vec2 uvMax = max(uvMinIn, uvMaxIn);
    for (;;) {
        uint width = mip_extent(a.mip0Width, mip);
        uint height = mip_extent(a.mip0Height, mip);
        bool fullU = uvMin.x < 0.0 || uvMax.x > 1.0 || (uvMax.x - uvMin.x) >= 1.0;
        bool fullV = uvMin.y < 0.0 || uvMax.y > 1.0 || (uvMax.y - uvMin.y) >= 1.0;
        vec2 lo = clamp(uvMin, vec2(0.0), vec2(1.0));
        vec2 hi = clamp(uvMax, vec2(0.0), vec2(1.0));
        uint minX = fullU ? 0u : min(uint(floor(lo.x * float(width))), width - 1u);
        uint maxX = fullU ? width - 1u : min(uint(floor(hi.x * float(width))), width - 1u);
        uint minY = fullV ? 0u : min(uint(floor(lo.y * float(height))), height - 1u);
        uint maxY = fullV ? height - 1u : min(uint(floor(hi.y * float(height))), height - 1u);
        uint span = (maxX - minX + 1u) * (maxY - minY + 1u);
        if (span <= 16u || mip + 1u >= mipCount) {
            uint base = mip_base_page(a, mip);
            for (uint y = minY; y <= maxY; ++y) {
                for (uint x = minX; x <= maxX; ++x)
                    request_page(base + y * width + x, mip, priority);
            }
            atomicAdd(counters.words[17], span);
            return;
        }
        ++mip;
    }
}
void main() {
    uint i = gl_GlobalInvocationID.x;
    uint actualCount = counters.words[2u + pc.tier];
    if (i >= actualCount || i >= visible_clusters.visible.length())
        return;
    uvec2 vc = visible_clusters.visible[i];
    if (vc.x >= clusters.values.length() || vc.y >= instances.values.length())
        return;
    Cluster cl = clusters.values[vc.x];
    Instance inst = instances.values[vc.y];
    uint materialId = inst.ids.y != 0xffffffffu ? inst.ids.y : cl.materialId;
    if (materialId >= materials.values.length())
        return;
    Material material = materials.values[materialId];

    vec3 localCenter = cl.sphere.xyz;
    vec3 worldCenter = vec3(dot(inst.row0.xyz, localCenter) + inst.row0.w,
        dot(inst.row1.xyz, localCenter) + inst.row1.w,
        dot(inst.row2.xyz, localCenter) + inst.row2.w);
    float maxScale = max(length(inst.row0.xyz), max(length(inst.row1.xyz), length(inst.row2.xyz)));
    float worldRadius = cl.sphere.w * max(maxScale, 0.0001);
    float distanceToSurface = max(0.25, length(worldCenter - f.cameraAndMaxDistance.xyz) - worldRadius);
    float projectionScale = max(abs(f.viewProjection[1][1]), 0.001);
    float projectedPixels = max(1.0, worldRadius * projectionScale * max(f.viewportAndMotion.y, 1.0) / distanceToSurface);
    uint priority = uint(clamp(projectedPixels * 16.0, 1.0, 65535.0));

    vec2 clusterUvMin = unpackHalf2x16(cl.packedUvMin);
    vec2 clusterUvMax = unpackHalf2x16(cl.packedUvMax);
    vec2 uv0 = clusterUvMin * material.uvScaleBias.xy + material.uvScaleBias.zw;
    vec2 uv1 = clusterUvMax * material.uvScaleBias.xy + material.uvScaleBias.zw;
    vec2 uvMin = min(uv0, uv1);
    vec2 uvMax = max(uv0, uv1);
    float density = max(uintBitsToFloat(cl.texelDensity), 0.000001);
    float worldDiameter = worldRadius * 2.0;

    request_allocation(material.vt.x, uvMin, uvMax, worldDiameter, projectedPixels, density, priority);
    request_allocation(material.vt.y, uvMin, uvMax, worldDiameter, projectedPixels, density, priority);
    request_allocation(material.vt.z, uvMin, uvMax, worldDiameter, projectedPixels, density, priority);
    request_allocation(material.vt.w, uvMin, uvMax, worldDiameter, projectedPixels, density, priority);
}
