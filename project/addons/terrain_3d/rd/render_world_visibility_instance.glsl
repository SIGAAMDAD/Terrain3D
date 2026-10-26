#[compute]
#version 450
#include "render_world_common_inc.glslinc"
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
float instance_radius_scale(Instance inst) {
    float f2 = dot(inst.row0.xyz, inst.row0.xyz) + dot(inst.row1.xyz, inst.row1.xyz) + dot(inst.row2.xyz, inst.row2.xyz);
    return sqrt(f2);
}
bool sphere_frustum(vec3 center, float radius) {
    if ((f.counts0.y & 1u) == 0u)
        return true;
    for (uint i = 0u; i < 6u; ++i) {
        vec4 p = f.frustumPlanes[i];
        if (dot(p.xyz, center) + p.w < -radius)
            return false;
    }
    return true;
}
bool instance_lod_visible(Instance inst, Lod lod, float radius_scale) {
    bool require_frustum = (f.counts0.y & 1u) != 0u;
    bool require_distance = (f.counts0.y & FLAG_DISTANCE) != 0u;
    if (!require_frustum && !require_distance)
        return true;
    if (lod.clusterOffset >= f.counts1.w)
        return false;
    bool any_frustum = !require_frustum;
    bool any_distance = !require_distance;
    uint clusterCount = min(lod.clusterCount, f.counts1.w - lod.clusterOffset);
    for (uint i = 0u; i < clusterCount; ++i) {
        uint clusterId = lod.clusterOffset + i;
        Cluster cl = clusters[clusterId];
        vec3 center = transform_point(inst, cl.sphere.xyz);
        float radius = cl.sphere.w * radius_scale;
        if (!any_frustum && sphere_frustum(center, radius))
            any_frustum = true;
        if (!any_distance && distance(center, f.cameraAndMaxDistance.xyz) - radius <= f.cameraAndMaxDistance.w)
            any_distance = true;
        if (any_frustum && any_distance)
            return true;
    }
    return any_frustum && any_distance;
}
void main() {
    uint visibleCellIndex = gl_GlobalInvocationID.x;
    uint cellCount = min(c.visibleCellCount, f.capacities0.x);
    if (visibleCellIndex >= cellCount)
        return;
    uint cellId = visibleCells[visibleCellIndex];
    if (cellId >= f.counts0.w)
        return;
    Cell cell = cells[cellId];
    if (cell.span.x >= f.counts1.x)
        return;
    uint cellInstanceCount = min(cell.span.y, f.counts1.x - cell.span.x);
    for (uint localIndex = 0u; localIndex < cellInstanceCount; ++localIndex) {
        uint instanceId = cell.span.x + localIndex;
        Instance inst = instances[instanceId];
        uint meshId = inst.ids.x;
        if (meshId >= f.counts1.y) {
            atomicAdd(c.instanceRejectCount, 1u);
            continue;
        }
        float d = distance(vec3(inst.row0.w, inst.row1.w, inst.row2.w), f.cameraAndMaxDistance.xyz);
        Mesh mesh = meshes[meshId];
        if (mesh.a.w == 0u || mesh.a.z >= f.counts1.z || mesh.a.w > f.counts1.z - mesh.a.z) {
            atomicAdd(c.instanceRejectCount, 1u);
            continue;
        }
        uint selected = mesh.a.w - 1u;
        for (uint i = 0u; i < mesh.a.w; ++i) {
            Lod lod = lods[mesh.a.z + i];
            if (d >= lod.minDistance && d < lod.maxDistance) {
                selected = i;
                break;
            }
        }
        Lod selectedRecord = lods[mesh.a.z + selected];
        float radiusScale = instance_radius_scale(inst);
        if (!instance_lod_visible(inst, selectedRecord, radiusScale)) {
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
