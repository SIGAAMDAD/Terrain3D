#[compute]
#version 450
#include "render_world_common_inc.glsl"
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
const uint FLAG_DISTANCE = 2u;
const uint OVERFLOW_INSTANCES = 2u;
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    Frame f;
};
layout(set = 0, binding = 1, std430) readonly buffer CellBuffer {
    Cell cells[];
};
layout(set = 0, binding = 2, std430) readonly buffer VisibleCellBuffer {
    uint visibleCells[];
};
layout(set = 0, binding = 3, std430) readonly buffer InstanceBuffer {
    Instance instances[];
};
layout(set = 0, binding = 4, std430) readonly buffer MeshBuffer {
    Mesh meshes[];
};
layout(set = 0, binding = 5, std430) readonly buffer LodBuffer {
    Lod lods[];
};
layout(set = 0, binding = 6, std430) buffer VisibleInstanceBuffer {
    uint visibleInstances[];
};
layout(set = 0, binding = 7, std430) buffer SelectedLodBuffer {
    uint selectedLod[];
};
layout(set = 0, binding = 8, std430) buffer CounterBuffer {
    Counters c;
};
layout(set = 0, binding = 9, std430) readonly buffer ClusterBuffer {
    Cluster clusters[];
};
vec3 transform_point(Instance inst, vec3 p) {
    return vec3(dot(inst.row0.xyz, p) + inst.row0.w, dot(inst.row1.xyz, p) + inst.row1.w, dot(inst.row2.xyz, p) + inst.row2.w);
}
float transform_radius(Instance inst, float r) {
    float f2 = dot(inst.row0.xyz, inst.row0.xyz) + dot(inst.row1.xyz, inst.row1.xyz) + dot(inst.row2.xyz, inst.row2.xyz);
    return r * sqrt(f2);
}
bool sphere_frustum(vec3 center, float radius) {
    if ((f.counts0.y & 1u) == 0u)
        return true;
    for (uint i = 0u; i < 6u; ++i) {
        vec4 p = f.frustumPlanes[i];
        if (dot(p.xyz, center) + p.w < -radius * length(p.xyz))
            return false;
    }
    return true;
}
bool instance_lod_in_frustum(Instance inst, Lod lod) {
    if ((f.counts0.y & 1u) == 0u)
        return true;
    for (uint i = 0u; i < lod.clusterCount; ++i) {
        uint clusterId = lod.clusterOffset + i;
        if (clusterId >= f.counts1.w)
            continue;
        Cluster cl = clusters[clusterId];
        if (sphere_frustum(transform_point(inst, cl.sphere.xyz), transform_radius(inst, cl.sphere.w)))
            return true;
    }
    return false;
}
bool instance_lod_in_distance(Instance inst, Lod lod) {
    if ((f.counts0.y & FLAG_DISTANCE) == 0u)
        return true;
    for (uint i = 0u; i < lod.clusterCount; ++i) {
        uint clusterId = lod.clusterOffset + i;
        if (clusterId >= f.counts1.w)
            continue;
        Cluster cl = clusters[clusterId];
        vec3 center = transform_point(inst, cl.sphere.xyz);
        float radius = transform_radius(inst, cl.sphere.w);
        if (distance(center, f.cameraAndMaxDistance.xyz) - radius <= f.cameraAndMaxDistance.w)
            return true;
    }
    return false;
}
void main() {
    uint visibleCellIndex = gl_GlobalInvocationID.x;
    uint cellCount = min(c.visibleCellCount, f.capacities0.x);
    if (visibleCellIndex >= cellCount)
        return;
    Cell cell = cells[visibleCells[visibleCellIndex]];
    for (uint localIndex = 0u; localIndex < cell.span.y; ++localIndex) {
        uint instanceId = cell.span.x + localIndex;
        if (instanceId >= f.counts1.x)
            continue;
        Instance inst = instances[instanceId];
        uint meshId = inst.ids.x;
        if (meshId >= f.counts1.y) {
            atomicAdd(c.instanceRejectCount, 1u);
            continue;
        }
        float d = distance(vec3(inst.row0.w, inst.row1.w, inst.row2.w), f.cameraAndMaxDistance.xyz);
        Mesh mesh = meshes[meshId];
        uint selected = max(mesh.a.w, 1u) - 1u;
        for (uint i = 0u; i < mesh.a.w; ++i) {
            Lod lod = lods[mesh.a.z + i];
            if (d >= lod.minDistance && d < lod.maxDistance) {
                selected = i;
                break;
            }
        }
        Lod selectedRecord = lods[mesh.a.z + selected];
        if (!instance_lod_in_distance(inst, selectedRecord) || !instance_lod_in_frustum(inst, selectedRecord)) {
            atomicAdd(c.instanceRejectCount, 1u);
            continue;
        }
        selectedLod[instanceId] = selected;
        uint dst = atomicAdd(c.visibleInstanceCount, 1u);
        if (dst < f.capacities0.y)
            visibleInstances[dst] = instanceId;
        else
            atomicOr(c.overflowBits, OVERFLOW_INSTANCES);
    }
}
