#[compute]
#version 450
#include "render_world_common_inc.glslinc"
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
const uint FLAG_FRUSTUM = 1u;
const uint FLAG_CONE = 4u;
const uint FLAG_HIZ = 8u;
const uint FLAG_CAMERA_CUT = 16u;
const uint FLAG_NO_OCCLUSION = 32u;
const uint INSTANCE_CONE_SAFE = 1u;
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    Frame f;
};
layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer {
    Instance instances[];
};
layout(set = 0, binding = 2, std430) readonly buffer MeshBuffer {
    Mesh meshes[];
};
layout(set = 0, binding = 3, std430) readonly buffer LodBuffer {
    Lod lods[];
};
layout(set = 0, binding = 4, std430) readonly buffer ClusterBuffer {
    Cluster clusters[];
};
layout(set = 0, binding = 5, std430) readonly buffer VisibleInstanceBuffer {
    uint visibleInstances[];
};
layout(set = 0, binding = 6, std430) readonly buffer SelectedLodBuffer {
    uint selectedLod[];
};
layout(set = 0, binding = 7, std430) buffer WorldBuffer {
    VisibleCluster worldList[];
};
layout(set = 0, binding = 8, std430) buffer ClutterBuffer {
    VisibleCluster clutterList[];
};
layout(set = 0, binding = 9, std430) buffer FoliageBuffer {
    VisibleCluster foliageList[];
};
layout(set = 0, binding = 10, std430) buffer CounterBuffer {
    Counters c;
};
layout(set = 0, binding = 11) uniform sampler2D hizTexture;
layout(set = 0, binding = 12, std430) buffer HistoryBuffer {
    uint lastVisibleFrame[];
};
vec3 transform_point(Instance inst, vec3 p) {
    return vec3(dot(inst.row0.xyz, p) + inst.row0.w, dot(inst.row1.xyz, p) + inst.row1.w, dot(inst.row2.xyz, p) + inst.row2.w);
}
float instance_radius_scale(Instance inst) {
    float f2 = dot(inst.row0.xyz, inst.row0.xyz) + dot(inst.row1.xyz, inst.row1.xyz) + dot(inst.row2.xyz, inst.row2.xyz);
    return sqrt(f2);
}
bool sphere_frustum(vec3 center, float radius) {
    if ((f.counts0.y & FLAG_FRUSTUM) == 0u)
        return true;
    for (uint i = 0u; i < 6u; ++i) {
        vec4 p = f.frustumPlanes[i];
        if (dot(p.xyz, center) + p.w < -radius)
            return false;
    }
    return true;
}
bool cone_visible(Instance inst, Cluster cl, vec3 center) {
    if ((f.counts0.y & FLAG_CONE) == 0u || (inst.ids.z & INSTANCE_CONE_SAFE) == 0u || cl.cone.w <= -0.9999)
        return true;
    vec3 axisRaw = vec3(dot(inst.row0.xyz, cl.cone.xyz), dot(inst.row1.xyz, cl.cone.xyz), dot(inst.row2.xyz, cl.cone.xyz));
    vec3 viewRaw = f.cameraAndMaxDistance.xyz - center;
    float axisLength = length(axisRaw);
    float viewLength = length(viewRaw);
    if (axisLength <= 1e-20 || viewLength <= 1e-20)
        return true;
    float inverse_length_product = 1.0 / (axisLength * viewLength);
    float cosine = dot(axisRaw, viewRaw) * inverse_length_product;
    float threshold = -sqrt(max(0.0, 1.0 - cl.cone.w * cl.cone.w));
    return cosine > threshold;
}
bool hiz_occluded(vec3 center, float radius, uint clusterId) {
    if ((f.counts0.y & FLAG_HIZ) == 0u || (f.counts0.y & FLAG_CAMERA_CUT) != 0u || (f.counts0.y & FLAG_NO_OCCLUSION) != 0u
        || f.counts0.z != 0u)
        return false;
    uint last = lastVisibleFrame[clusterId];
    if (last != 0u && f.counts0.x >= last && f.counts0.x - last <= 2u)
        return false;
    vec2 ndcMin = vec2(1.0);
    vec2 ndcMax = vec2(-1.0);
    float nearestDepth = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner) {
        vec3 p = center
            + vec3((corner & 1u) != 0u ? radius : -radius, (corner & 2u) != 0u ? radius : -radius, (corner & 4u) != 0u ? radius : -radius);
        vec4 clip = f.viewProjection * vec4(p, 1.0);
        if (clip.w <= 1e-6)
            return false;
        vec3 ndc = clip.xyz / clip.w;
        if (ndc.z < 0.0 || ndc.z > 1.0)
            return false;
        ndcMin = min(ndcMin, ndc.xy);
        ndcMax = max(ndcMax, ndc.xy);
        nearestDepth = max(nearestDepth, ndc.z);
    }
    if (ndcMax.x < -1.0 || ndcMin.x > 1.0 || ndcMax.y < -1.0 || ndcMin.y > 1.0)
        return false;
    ndcMin = clamp(ndcMin, vec2(-1.0), vec2(1.0));
    ndcMax = clamp(ndcMax, vec2(-1.0), vec2(1.0));
    vec2 viewport = max(f.viewportAndMotion.xy, vec2(1.0));
    float expand = max(f.viewportAndMotion.z, 0.0);
    vec2 pixelMin = (ndcMin * 0.5 + 0.5) * viewport - vec2(expand);
    vec2 pixelMax = (ndcMax * 0.5 + 0.5) * viewport + vec2(expand);
    vec2 viewportMax = viewport - vec2(1.0);
    pixelMin = clamp(pixelMin, vec2(0.0), viewportMax);
    pixelMax = clamp(pixelMax, vec2(0.0), viewportMax);
    float extent = max(pixelMax.x - pixelMin.x + 1.0, pixelMax.y - pixelMin.y + 1.0);
    uint mip = 0u;
    float cover = 1.0;
    while (mip + 1u < f.capacities1.w && cover < extent) {
        cover *= 2.0;
        mip++;
    }
    uint scale = 1u << mip;
    ivec2 p0 = ivec2(uvec2(pixelMin) / scale);
    ivec2 p1 = ivec2(uvec2(pixelMax) / scale);
    int mipI = int(mip);
    ivec2 mipSize = textureSize(hizTexture, mipI);
    ivec2 mipMax = mipSize - ivec2(1);
    p0 = clamp(p0, ivec2(0), mipMax);
    p1 = clamp(p1, ivec2(0), mipMax);
    float d = min(min(texelFetch(hizTexture, ivec2(p0.x, p0.y), mipI).r, texelFetch(hizTexture, ivec2(p1.x, p0.y), mipI).r),
        min(texelFetch(hizTexture, ivec2(p0.x, p1.y), mipI).r, texelFetch(hizTexture, ivec2(p1.x, p1.y), mipI).r));
    return d > nearestDepth + 0.001;
}
void append_cluster(uint tier, VisibleCluster item) {
    uint dst = atomicAdd(c.visibleClusterCount[tier], 1u);
    uint cap = tier == 0u ? f.capacities0.z : (tier == 1u ? f.capacities0.w : f.capacities1.x);
    if (dst >= cap) {
        atomicOr(c.overflowBits, 4u << tier);
        return;
    }
    if (tier == 0u)
        worldList[dst] = item;
    else if (tier == 1u)
        clutterList[dst] = item;
    else
        foliageList[dst] = item;
}
void main() {
    uint visibleIndex = gl_GlobalInvocationID.x;
    uint visibleCount = min(c.visibleInstanceCount, f.capacities0.y);
    if (visibleIndex >= visibleCount)
        return;
    uint instanceId = visibleInstances[visibleIndex];
    if (instanceId >= f.counts1.x)
        return;
    Instance inst = instances[instanceId];
    if (inst.ids.x >= f.counts1.y)
        return;
    Mesh mesh = meshes[inst.ids.x];
    if (mesh.a.w == 0u || mesh.a.z >= f.counts1.z || mesh.a.w > f.counts1.z - mesh.a.z)
        return;
    uint lodIndex = selectedLod[instanceId];
    if (lodIndex >= mesh.a.w)
        return;
    Lod lod = lods[mesh.a.z + lodIndex];
    if (lod.clusterOffset >= f.counts1.w)
        return;
    float radiusScale = instance_radius_scale(inst);
    uint clusterCount = min(lod.clusterCount, f.counts1.w - lod.clusterOffset);
    for (uint i = 0u; i < clusterCount; ++i) {
        uint clusterId = lod.clusterOffset + i;
        Cluster cl = clusters[clusterId];
        atomicAdd(c.candidateClusterCount, 1u);
        vec3 center = transform_point(inst, cl.sphere.xyz);
        float radius = cl.sphere.w * radiusScale;
        if (!sphere_frustum(center, radius)) {
            atomicAdd(c.clusterFrustumRejectCount, 1u);
            continue;
        }
        if (!cone_visible(inst, cl, center)) {
            atomicAdd(c.clusterConeRejectCount, 1u);
            continue;
        }
        if (hiz_occluded(center, radius, clusterId)) {
            atomicAdd(c.clusterHiZRejectCount, 1u);
            continue;
        }
        uint tier = (cl.packedLodTier >> 16u) & 0xffffu;
        if (tier < 3u) {
            append_cluster(tier, VisibleCluster(clusterId, instanceId));
            lastVisibleFrame[clusterId] = f.counts0.x;
        }
    }
}
