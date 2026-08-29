#[versions]
shadow = "#define T3D_RW_AUX_SHADOW";
motion = "#define T3D_RW_AUX_MOTION";
aux = "#define T3D_RW_AUX_GENERIC";

#[vertex]
#version 450
#VERSION_DEFINES
#include "render_world_common_inc.glsl"
struct Vertex {
    vec3 position;
    uint normal;
    uint tangent;
    vec2 uv;
    uint color;
};
struct Visible {
    uint clusterId;
    uint instanceId;
};
layout(set = 0, binding = 0, std430) readonly buffer Frames {
    Frame f;
};
layout(set = 0, binding = 1, std430) readonly buffer Vertices {
    Vertex vertices[];
};
layout(set = 0, binding = 2, std430) readonly buffer VertexIndices {
    uint vertexIndices[];
};
layout(set = 0, binding = 3, std430) readonly buffer Triangles {
    uint triangles[];
};
layout(set = 0, binding = 4, std430) readonly buffer Clusters {
    Cluster clusters[];
};
layout(set = 0, binding = 5, std430) readonly buffer Instances {
    Instance instances[];
};
layout(set = 0, binding = 6, std430) readonly buffer VisibleClusters {
    Visible visibleClusters[];
};
layout(location = 0) out vec4 vCurrentClip;
layout(location = 1) out vec4 vPreviousClip;
void main() {
    Visible v = visibleClusters[gl_InstanceIndex];
    Cluster c = clusters[v.clusterId];
    uint tri = triangles[c.triangleOffset + (uint(gl_VertexIndex) / 3u)];
    uint lane = uint(gl_VertexIndex) % 3u;
    uint local = (tri >> (lane * 6u)) & 63u;
    uint vertexId = vertexIndices[c.vertexIndexOffset + local];
    vec3 p = vertices[vertexId].position;
    Instance inst = instances[v.instanceId];
    vec4 world = vec4(dot(inst.row0, vec4(p, 1.0)), dot(inst.row1, vec4(p, 1.0)), dot(inst.row2, vec4(p, 1.0)), 1.0);
    vCurrentClip = f.viewProjection * world;
    vPreviousClip = f.previousViewProjection * world;
    gl_Position = vCurrentClip;
}

#[fragment]
#version 450
#VERSION_DEFINES
#include "render_world_common_inc.glsl"
#ifdef T3D_RW_AUX_SHADOW
void main() {
}
#elif defined(T3D_RW_AUX_MOTION)
layout(location = 0) in vec4 vCurrentClip;
layout(location = 1) in vec4 vPreviousClip;
layout(location = 0) out vec4 outMotion;
void main() {
    vec2 c = vCurrentClip.xy / max(abs(vCurrentClip.w), 1e-6);
    vec2 p = vPreviousClip.xy / max(abs(vPreviousClip.w), 1e-6);
    outMotion = vec4(c - p, 0.0, 1.0);
}
#else
layout(location = 0) out vec4 outAux;
void main() {
    outAux = vec4(1.0, 0.0, 0.0, 1.0);
}
#endif
