#[compute]
#version 450
#include "render_world_common_inc.glslinc"
#include "vt/terrain_vt_common_inc.glslinc"
#include "vt/terrain_vt_filter_math_inc.glslinc"
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(push_constant, std430) uniform VtDispatchPush {
    uint tier;
    uint hashCount;
    uint requestCapacity;
    uint reserved;
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
    T3DRenderingMaterialGpu values[];
}
materials;
layout(set = 0, binding = 5, std430) readonly buffer Allocations {
    T3DVTAllocation values[];
}
allocations;
layout(set = 0, binding = 6, std430) readonly buffer PageTable {
    T3DVTPageEntry entries[];
}
page_table;
layout(set = 0, binding = 7, std430) buffer VtHashBuffer {
    uvec4 entries[];
}
hash_table;
layout(set = 0, binding = 8, std430) buffer VtCounterBuffer {
    uint words[];
}
counters;

bool page_is_resident(uint page, uint allocationGeneration) {
    if (page >= f.resourceCounts.z)
        return false;
    T3DVTPageEntry entry = page_table.entries[page];
    uint flags = t3d_vt_page_flags(entry);
    return (flags & T3D_VT_PAGE_VALID) != 0u && (flags & T3D_VT_PAGE_RESIDENT) != 0u && entry.generation == allocationGeneration
        && entry.physicalSlot != T3D_VT_INVALID_SLOT;
}

void request_page(uint page, uint wantedMip, uint priority, uint allocationGeneration) {
    if (page >= f.resourceCounts.z || page_is_resident(page, allocationGeneration))
        return;
    uint hashCount = pc.hashCount;
    if (hashCount == 0u)
        return;
    uint hashSeed = t3d_vt_request_hash_linear_seed(page);
    for (uint probe = 0u; probe < T3D_VT_REQUEST_HASH_PROBE_COUNT; ++probe) {
        uint slot = t3d_vt_request_hash_slot_from_seed(hashSeed, probe, hashCount);
        uint oldKey = atomicCompSwap(hash_table.entries[slot].x, T3D_VT_REQUEST_HASH_EMPTY_KEY, page);
        if (oldKey == T3D_VT_REQUEST_HASH_EMPTY_KEY || oldKey == page) {
            atomicMin(hash_table.entries[slot].y, wantedMip);
            atomicMax(hash_table.entries[slot].z, priority);
            atomicAdd(counters.words[18], 1u);
            return;
        }
    }
    atomicAdd(counters.words[19], 1u);
}

bool load_prediction_allocation(uint allocationId, out T3DVTAllocation allocation) {
    allocation = T3DVTAllocation(0u, 0u, 0u, 0u, T3D_VT_INVALID_TAIL, 0u, 0u, 0u);
    if (allocationId == T3D_VT_INVALID_ALLOCATION || allocationId >= f.resourceCounts.y)
        return false;
    allocation = allocations.values[allocationId];
    uint mipCount = min(t3d_vt_allocation_mip_count(allocation), T3D_VT_MAX_MIPS);
    uint mip0Width = t3d_vt_allocation_mip0_width(allocation);
    uint mip0Height = t3d_vt_allocation_mip0_height(allocation);
    if (mipCount == 0u || allocation.pageCount == 0u || mip0Width == 0u || mip0Height == 0u || !t3d_vt_allocation_quality_enabled(allocation))
        return false;
    return allocation.baseVirtualPage < f.resourceCounts.z && allocation.pageCount <= f.resourceCounts.z - allocation.baseVirtualPage;
}

uint select_prediction_mip(
    T3DVTAllocation allocation, float worldDiameter, float projectedPixels, float texelDensity) {
    uint mipCount = min(t3d_vt_allocation_mip_count(allocation), T3D_VT_MAX_MIPS);
    uint baseMip = min(t3d_vt_allocation_quality_base_mip(allocation), mipCount - 1u);
    float virtualExtent = float(max(t3d_vt_prediction_mip_extent(t3d_vt_allocation_mip0_width(allocation), baseMip),
        t3d_vt_prediction_mip_extent(t3d_vt_allocation_mip0_height(allocation), baseMip)) * T3D_VT_PAGE_INTERIOR);
    float sourceTexelsAcross = max(1.0, texelDensity * max(worldDiameter, 0.001) * virtualExtent);
    return baseMip + uint(clamp(floor(log2(max(1.0, sourceTexelsAcross / max(projectedPixels, 1.0)))), 0.0, float(mipCount - baseMip - 1u)));
}

void request_page_set_rect(uvec4 baseVirtualPages,
    uvec4 generations,
    uint allocationCount,
    uint diagnosticAllocationCount,
    T3DVTAllocation primary,
    vec2 uvMin,
    vec2 uvMax,
    float worldDiameter,
    float projectedPixels,
    float texelDensity,
    uint priority) {
    uint mipCount = min(t3d_vt_allocation_mip_count(primary), T3D_VT_MAX_MIPS);
    uint mip = select_prediction_mip(primary, worldDiameter, projectedPixels, texelDensity);
    for (;;) {
        T3DVTPredictionRect rect = t3d_vt_prediction_rect(primary, mip, uvMin, uvMax);
        if (rect.page_span > T3D_VT_REQUEST_RECT_PAGE_CAP && mip + 1u < mipCount) {
            ++mip;
            continue;
        }

        if (rect.page_span <= T3D_VT_REQUEST_RECT_PAGE_CAP) {
            // Hot path: build the bounded relative-page list once, then replay it
            // channel-major. Channel-major replay preserves the exact old hash
            // insertion order while removing repeated page-coordinate arithmetic.
            uint relativePages[T3D_VT_REQUEST_RECT_PAGE_CAP];
            uint emitted = 0u;
            for (uint y = rect.min_y; y <= rect.max_y; ++y) {
                for (uint x = rect.min_x; x <= rect.max_x; ++x) {
                    uint relativePage = rect.relative_mip_base + y * rect.width + x;
                    if (relativePage < primary.pageCount)
                        relativePages[emitted++] = relativePage;
                }
            }
            for (uint channel = 0u; channel < allocationCount; ++channel) {
                uint baseVirtualPage = baseVirtualPages[channel];
                uint generation = generations[channel];
                for (uint pageIndex = 0u; pageIndex < emitted; ++pageIndex)
                    request_page(baseVirtualPage + relativePages[pageIndex], mip, priority, generation);
            }
        } else {
            // Cold terminal-mip fallback. The old predictor emitted the whole final
            // rectangle when it could no longer coarsen, even if it exceeded the
            // nominal rectangle cap. Preserve that behavior exactly.
            for (uint channel = 0u; channel < allocationCount; ++channel) {
                uint baseVirtualPage = baseVirtualPages[channel];
                uint generation = generations[channel];
                for (uint y = rect.min_y; y <= rect.max_y; ++y) {
                    for (uint x = rect.min_x; x <= rect.max_x; ++x) {
                        uint relativePage = rect.relative_mip_base + y * rect.width + x;
                        if (relativePage < primary.pageCount)
                            request_page(baseVirtualPage + relativePage, mip, priority, generation);
                    }
                }
            }
        }
        // Preserve the old diagnostic meaning: count one predicted rectangle per
        // logical channel allocation even though rectangle derivation is shared.
        atomicAdd(counters.words[17], rect.page_span * diagnosticAllocationCount);
        return;
    }
}

void request_material_page_sets(
    uvec4 materialAllocations, vec2 uvMin, vec2 uvMax, float worldDiameter, float projectedPixels, float texelDensity, uint priority) {
    bool consumed[4] = bool[4](false, false, false, false);
    for (uint i = 0u; i < 4u; ++i) {
        if (consumed[i])
            continue;
        uint allocationId = materialAllocations[i];
        T3DVTAllocation primary;
        if (!load_prediction_allocation(allocationId, primary)) {
            consumed[i] = true;
            continue;
        }

        uvec4 groupIds = uvec4(allocationId, T3D_VT_INVALID_ALLOCATION, T3D_VT_INVALID_ALLOCATION, T3D_VT_INVALID_ALLOCATION);
        uvec4 groupBaseVirtualPages = uvec4(primary.baseVirtualPage, 0u, 0u, 0u);
        uvec4 groupGenerations = uvec4(primary.generation, 0u, 0u, 0u);
        uint groupCount = 1u;
        uint diagnosticAllocationCount = 1u;
        consumed[i] = true;
        for (uint j = i + 1u; j < 4u; ++j) {
            if (consumed[j])
                continue;
            if (materialAllocations[j] == T3D_VT_INVALID_ALLOCATION) {
                // Invalid slots emitted no old request, so crossing them does not
                // change the request order of valid allocations.
                consumed[j] = true;
                continue;
            }
            bool duplicateGroupAllocation = materialAllocations[j] == groupIds.x
                || (groupCount > 1u && materialAllocations[j] == groupIds.y)
                || (groupCount > 2u && materialAllocations[j] == groupIds.z)
                || (groupCount > 3u && materialAllocations[j] == groupIds.w);
            if (duplicateGroupAllocation) {
                // Exact duplicate allocation IDs have identical request identity.
                // Keep the old predicted-rectangle diagnostic count even though
                // the duplicate hash/request emission itself is eliminated.
                ++diagnosticAllocationCount;
                consumed[j] = true;
                continue;
            }
            T3DVTAllocation sibling;
            if (pc.reserved != 0u && load_prediction_allocation(materialAllocations[j], sibling)
                    && t3d_vt_prediction_page_set_sibling(allocationId, primary, materialAllocations[j], sibling)) {
                groupIds[groupCount] = materialAllocations[j];
                groupBaseVirtualPages[groupCount] = sibling.baseVirtualPage;
                groupGenerations[groupCount] = sibling.generation;
                ++groupCount;
                ++diagnosticAllocationCount;
                consumed[j] = true;
                continue;
            }
            // Stop at the first valid unrelated allocation. Grouping across it
            // would reorder hash insertion and could change overflow behavior.
            break;
        }
        request_page_set_rect(
            groupBaseVirtualPages, groupGenerations, groupCount, diagnosticAllocationCount, primary, uvMin, uvMax, worldDiameter, projectedPixels, texelDensity, priority);
    }
}

void main() {
    if (pc.tier >= 3u)
        return;
    uint i = gl_GlobalInvocationID.x;
    uint actualCount = counters.words[2u + pc.tier];
    uint visibleCapacity = pc.tier == 0u ? f.capacities0.z : (pc.tier == 1u ? f.capacities0.w : f.capacities1.x);
    if (i >= actualCount || i >= visibleCapacity)
        return;
    uvec2 vc = visible_clusters.visible[i];
    if (vc.x >= f.counts1.w || vc.y >= f.counts1.x)
        return;
    Cluster cl = clusters.values[vc.x];
    Instance inst = instances.values[vc.y];
    uint materialId = inst.ids.y != T3D_VT_INVALID_ALLOCATION ? inst.ids.y : cl.materialId;
    if (materialId >= f.resourceCounts.x)
        return;
    T3DRenderingMaterialGpu material = materials.values[materialId];

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

    // Canonical V3 material ABI stores one base allocation for the three runtime
    // material channels, plus one optional overlay allocation. Reconstruct the
    // bounded predictor list without wrapping INVALID_ALLOCATION or crossing the
    // published allocation count.
    uvec4 materialAllocations = uvec4(T3D_VT_INVALID_ALLOCATION);
    if (material.vtAllocation != T3D_VT_INVALID_ALLOCATION && material.vtAllocation < f.resourceCounts.y) {
        materialAllocations.x = material.vtAllocation;
        if (f.resourceCounts.y - material.vtAllocation >= T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION) {
            materialAllocations.y = material.vtAllocation + 1u;
            materialAllocations.z = material.vtAllocation + 2u;
        }
    }
    if (material.overlayVtAllocation != T3D_VT_INVALID_ALLOCATION && material.overlayVtAllocation < f.resourceCounts.y)
        materialAllocations.w = material.overlayVtAllocation;

    // WG-F P16: preserve every required channel page, but group allocations that
    // provably belong to the same V3 material page set. Mip selection, UV rectangle,
    // page coordinates and coarsening are then derived once for the group.
    request_material_page_sets(materialAllocations, uvMin, uvMax, worldDiameter, projectedPixels, density, priority);
}
