#[versions]
color_native = "#define T3D_FORWARD_NATIVE_LIGHTING";
color_specular = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR";
color_motion = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION";
color_specular_motion = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION";
color_native_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_specular_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_motion_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";
color_specular_motion_radiance_array = "#define T3D_FORWARD_NATIVE_LIGHTING\n#define T3D_FORWARD_SPECULAR\n#define T3D_FORWARD_MOTION\n#define T3D_FORWARD_RADIANCE_ARRAY";

#[vertex]
#version 450
#VERSION_DEFINES
#include "render_world_common_inc.glslinc"
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    Frame f;
};
layout(set = 0, binding = 1, std430) readonly buffer VertexHeap {
    uint vertexWords[];
};
layout(set = 0, binding = 2, std430) readonly buffer ClusterVertexIndexHeap {
    uint clusterVertexIndex[];
};
layout(set = 0, binding = 3, std430) readonly buffer TriangleHeap {
    uint packedTriangles[];
};
layout(set = 0, binding = 4, std430) readonly buffer ClusterBuffer {
    Cluster clusters[];
};
layout(set = 0, binding = 5, std430) readonly buffer InstanceBuffer {
    Instance instances[];
};
layout(set = 0, binding = 6, std430) readonly buffer VisibleBuffer {
    VisibleCluster visibleClusters[];
};
layout(location = 0) out vec2 outUv;
layout(location = 1) flat out uint outMaterialId;
layout(location = 2) flat out uint outTier;
layout(location = 3) out vec3 outWorldNormal;
layout(location = 4) out vec4 outWorldTangent;
layout(location = 5) out vec4 outVertexColor;
layout(location = 6) out vec3 outWorldPosition;
layout(location = 7) out vec4 outCurrentClip;
layout(location = 8) out vec4 outPreviousClip;

float t3d_rw_unpack_snorm10(uint packed, uint shift) {
    int value = int((packed >> shift) & 0x3ffu);
    if (value >= 512)
        value -= 1024;
    return clamp(float(value) / 511.0, -1.0, 1.0);
}
vec3 t3d_rw_unpack_vec3_snorm10(uint packed) {
    return normalize(vec3(t3d_rw_unpack_snorm10(packed, 0u), t3d_rw_unpack_snorm10(packed, 10u), t3d_rw_unpack_snorm10(packed, 20u)));
}
vec3 t3d_rw_transform_normal(Instance inst, vec3 n) {
    vec3 c0 = vec3(inst.row0.x, inst.row1.x, inst.row2.x);
    vec3 c1 = vec3(inst.row0.y, inst.row1.y, inst.row2.y);
    vec3 c2 = vec3(inst.row0.z, inst.row1.z, inst.row2.z);
    vec3 transformed = cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z;
    return normalize(transformed);
}
vec3 t3d_rw_transform_direction(Instance inst, vec3 v) {
    return normalize(vec3(dot(inst.row0.xyz, v), dot(inst.row1.xyz, v), dot(inst.row2.xyz, v)));
}
void main() {
    VisibleCluster visible = visibleClusters[gl_InstanceIndex];
    Cluster cl = clusters[visible.clusterId];
    Instance inst = instances[visible.instanceId];
    uint tri = uint(gl_VertexIndex) / 3u;
    uint corner = uint(gl_VertexIndex) % 3u;
    uint packedTri = packedTriangles[cl.triangleOffset + tri];
    uint localIndex = (packedTri >> (corner * 6u)) & 63u;
    uint globalVertex = clusterVertexIndex[cl.vertexIndexOffset + localIndex];
    uint base = globalVertex * 8u;
    vec3 p =
        vec3(uintBitsToFloat(vertexWords[base + 0u]), uintBitsToFloat(vertexWords[base + 1u]), uintBitsToFloat(vertexWords[base + 2u]));
    vec3 n = t3d_rw_unpack_vec3_snorm10(vertexWords[base + 3u]);
    uint packedTangent = vertexWords[base + 4u];
    vec3 tangent = t3d_rw_unpack_vec3_snorm10(packedTangent);
    float handedness = (packedTangent & (1u << 30u)) != 0u ? -1.0 : 1.0;
    vec3 world = vec3(dot(inst.row0.xyz, p) + inst.row0.w, dot(inst.row1.xyz, p) + inst.row1.w, dot(inst.row2.xyz, p) + inst.row2.w);
    vec4 currentClip = f.viewProjection * vec4(world, 1.0);
    vec4 previousClip = f.previousViewProjection * vec4(world, 1.0);
    gl_Position = currentClip;
    outUv = unpackHalf2x16(vertexWords[base + 5u]);
    outMaterialId = inst.ids.y != 0xffffffffu ? inst.ids.y : cl.materialId;
    outTier = (cl.packedLodTier >> 16u) & 0xffffu;
    outWorldNormal = t3d_rw_transform_normal(inst, n);
    outWorldTangent = vec4(t3d_rw_transform_direction(inst, tangent), handedness);
    outVertexColor = unpackUnorm4x8(vertexWords[base + 7u]);
    outWorldPosition = world;
    outCurrentClip = currentClip;
    outPreviousClip = previousClip;
}

#[fragment]
#version 450
#VERSION_DEFINES
#if defined(T3D_FORWARD_NATIVE_LIGHTING) && !defined(T3D_FORWARD_DEPTH)
#extension GL_EXT_samplerless_texture_functions : enable
#endif
#include "render_world_common_inc.glslinc"
#define T3D_FORWARD_RUNTIME_SHADOW_TOGGLE
#include "terrain_forward_lighting_inc.glslinc"
#undef T3D_FORWARD_RUNTIME_SHADOW_TOGGLE
#include "vt/terrain_vt_common_inc.glslinc"
#include "vt/terrain_vt_filter_math_inc.glslinc"
#include "vt/terrain_vt_tail_filter_inc.glslinc"
#include "vt/terrain_vt_sample_inc.glslinc"
#define T3D_VT_RESOLVE_ALLOCATION(index) t3dVtAllocations[index]
#define T3D_VT_RESOLVE_PAGE_ENTRY(index) t3dVtPageTable[index]
#define T3D_VT_RESOLVE_TAIL_ENTRY(index) t3dVtTails[index]
#include "vt/terrain_vt_resolve_core.glslinc"
#undef T3D_VT_RESOLVE_TAIL_ENTRY
#undef T3D_VT_RESOLVE_PAGE_ENTRY
#undef T3D_VT_RESOLVE_ALLOCATION
#define T3D_VT_CONSUMER_ALLOCATION(index) t3dVtAllocations[index]
#define T3D_VT_CONSUMER_MIP_LAYOUT(index) t3dVtMipLayouts[index]
#define T3D_VT_CONSUMER_TAIL(index) t3dVtTails[index]
#define T3D_VT_CONSUMER_ALBEDO_BANK_GRAD(layer, uv, grad_x, grad_y) textureGrad(t3dVtAlbedoBank0, vec3((uv), float(layer)), (grad_x), (grad_y))
#define T3D_VT_CONSUMER_NORMAL_BANK_GRAD(layer, uv, grad_x, grad_y) textureGrad(t3dVtNormalBank0, vec3((uv), float(layer)), (grad_x), (grad_y))
#define T3D_VT_CONSUMER_SURFACE_BANK_GRAD(layer, uv, grad_x, grad_y) textureGrad(t3dVtSurfaceBank0, vec3((uv), float(layer)), (grad_x), (grad_y))
#define T3D_VT_CONSUMER_ALBEDO_TAIL_LOD0(uv) textureLod(t3dVtAlbedoTail, (uv), 0.0)
#define T3D_VT_CONSUMER_NORMAL_TAIL_LOD0(uv) textureLod(t3dVtNormalTail, (uv), 0.0)
#define T3D_VT_CONSUMER_SURFACE_TAIL_LOD0(uv) textureLod(t3dVtSurfaceTail, (uv), 0.0)
#define T3D_VT_CONSUMER_ALBEDO_TAIL_SIZE() textureSize(t3dVtAlbedoTail, 0)
#define T3D_VT_CONSUMER_NORMAL_TAIL_SIZE() textureSize(t3dVtNormalTail, 0)
#define T3D_VT_CONSUMER_SURFACE_TAIL_SIZE() textureSize(t3dVtSurfaceTail, 0)
#include "vt/terrain_vt_consumer_sample_inc.glslinc"
#undef T3D_VT_CONSUMER_SURFACE_TAIL_SIZE
#undef T3D_VT_CONSUMER_NORMAL_TAIL_SIZE
#undef T3D_VT_CONSUMER_ALBEDO_TAIL_SIZE
#undef T3D_VT_CONSUMER_SURFACE_TAIL_LOD0
#undef T3D_VT_CONSUMER_NORMAL_TAIL_LOD0
#undef T3D_VT_CONSUMER_ALBEDO_TAIL_LOD0
#undef T3D_VT_CONSUMER_SURFACE_BANK_GRAD
#undef T3D_VT_CONSUMER_NORMAL_BANK_GRAD
#undef T3D_VT_CONSUMER_ALBEDO_BANK_GRAD
#undef T3D_VT_CONSUMER_TAIL
#undef T3D_VT_CONSUMER_MIP_LAYOUT
#undef T3D_VT_CONSUMER_ALLOCATION
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    Frame f;
};
layout(set = 0, binding = 7, std430) readonly buffer MaterialBuffer {
    T3DRenderingMaterialGpu materials[];
};

// UWR-P02: RenderWorld consumes the canonical world material/page-set ABI.
#define T3D_VT_RW_FEEDBACK_BASE_ALLOCATION(material_id) (materials[(material_id)].vtAllocation)
#define T3D_VT_RW_FEEDBACK_REPEAT_UV(material_id) \
    (materials[(material_id)].samplerClass == T3D_VT_SAMPLER_LINEAR_REPEAT \
        || materials[(material_id)].samplerClass == T3D_VT_SAMPLER_NEAREST_REPEAT)
#ifdef T3D_VT_FRAGMENT_FEEDBACK
#ifndef T3D_VT_RW_FEEDBACK_BASE_ALLOCATION
#error "RenderWorld must provide T3D_VT_RW_FEEDBACK_BASE_ALLOCATION(material_id)"
#endif
#ifndef T3D_VT_RW_FEEDBACK_REPEAT_UV
#error "RenderWorld must provide T3D_VT_RW_FEEDBACK_REPEAT_UV(material_id)"
#endif
#define T3D_VT_DECLARE_FRAGMENT_BINDINGS
#include "vt/terrain_vt_feedback_inc.glslinc"
#undef T3D_VT_DECLARE_FRAGMENT_BINDINGS
#endif

layout(location = 0) in vec2 inUv;
layout(location = 1) flat in uint inMaterialId;
layout(location = 2) flat in uint inTier;
layout(location = 3) in vec3 inWorldNormal;
layout(location = 4) in vec4 inWorldTangent;
layout(location = 5) in vec4 inVertexColor;
layout(location = 6) in vec3 inWorldPosition;
layout(location = 7) in vec4 inCurrentClip;
layout(location = 8) in vec4 inPreviousClip;
layout(location = 0) out vec4 outColor;
#ifdef T3D_FORWARD_SPECULAR
layout(location = 1) out vec4 outSpecular;
#endif
#ifdef T3D_FORWARD_MOTION
layout(location = 2) out vec2 outMotion;
#endif

const uint T3D_RW_INVALID_ID = 0xffffffffu;
const uint T3D_RW_MATERIAL_FLAG_ALPHA_SCISSOR = T3D_VT_MATERIAL_ALPHA_SCISSOR;
const uint T3D_RW_MATERIAL_FLAG_RECEIVE_SHADOWS = T3D_VT_MATERIAL_RECEIVE_SHADOWS;

float t3d_rw_unpack_unorm8(uint packed, uint shift) {
    return float((packed >> shift) & T3D_VT_MATERIAL_UNORM8_MASK) * (1.0 / 255.0);
}

void main() {
    if (inMaterialId >= f.resourceCounts.x)
        discard;
    T3DRenderingMaterialGpu material = materials[inMaterialId];
    vec2 materialUv = inUv * material.uvScaleBias.xy + material.uvScaleBias.zw;
    bool repeatUv = material.samplerClass == T3D_VT_SAMPLER_LINEAR_REPEAT || material.samplerClass == T3D_VT_SAMPLER_NEAREST_REPEAT;
    uint vtFilterMode = (material.packedScalars1 & T3D_VT_MATERIAL_FILTER_MASK) >> T3D_VT_MATERIAL_FILTER_SHIFT;
#ifdef T3D_VT_FRAGMENT_FEEDBACK
    bool feedbackRepeatUv = T3D_VT_RW_FEEDBACK_REPEAT_UV(inMaterialId);
    t3d_vt_fragment_request_material_page_set(T3D_VT_RW_FEEDBACK_BASE_ALLOCATION(inMaterialId),
        materialUv,
        dFdx(materialUv),
        dFdy(materialUv),
        0.0,
        T3D_VT_FRAGMENT_PRIORITY_COLOR,
        feedbackRepeatUv,
        vtFilterMode);
#endif
    T3DVTConsumerFootprint vtFootprint = t3d_vt_consumer_prepare_footprint(
        materialUv, dFdx(materialUv), dFdy(materialUv), 0.0, repeatUv, vtFilterMode, f.resourceCounts.y, f.resourceCounts.z);
    T3DVTConsumerPageSet3 pageSet = t3d_vt_consumer_resolve_page_set3_v3(material.vtAllocation, vtFootprint);
    vec4 albedoSample = t3d_vt_consumer_sample_albedo(pageSet.channel0);
    vec4 base = material.baseColor * albedoSample * inVertexColor;
    uint materialFlags = material.flags;
    float metallic = t3d_rw_unpack_unorm8(material.packedScalars0, T3D_VT_MATERIAL_METALLIC_SHIFT);
    float roughness = t3d_rw_unpack_unorm8(material.packedScalars0, T3D_VT_MATERIAL_ROUGHNESS_SHIFT);
    float aoStrength = t3d_rw_unpack_unorm8(material.packedScalars0, T3D_VT_MATERIAL_AO_SHIFT);
    float alphaThreshold = t3d_rw_unpack_unorm8(material.packedScalars0, T3D_VT_MATERIAL_ALPHA_SCISSOR_SHIFT);
    float normalScale = max(unpackHalf2x16(material.packedScalars1).x, 0.0);
    uint surfaceLayout = (material.packedScalars1 & T3D_VT_MATERIAL_SURFACE_LAYOUT_MASK) >> T3D_VT_MATERIAL_SURFACE_LAYOUT_SHIFT;
    if ((materialFlags & T3D_RW_MATERIAL_FLAG_ALPHA_SCISSOR) != 0u && base.a < alphaThreshold)
        discard;

    vec4 surface = t3d_vt_consumer_sample_orm_v3(pageSet.channel2);
    float ao = (materialFlags & T3D_VT_MATERIAL_AO_ENABLED) != 0u ? mix(1.0, surface.r, aoStrength) : 1.0;
    if (surfaceLayout == 0u) {
        if ((materialFlags & T3D_VT_MATERIAL_DIRECT_ORM) != 0u) {
            roughness = clamp(surface.g, 0.04, 1.0);
            metallic = clamp(surface.b, 0.0, 1.0);
        } else {
            roughness = clamp(roughness * surface.g, 0.02, 1.0);
            metallic = clamp(metallic * surface.b, 0.0, 1.0);
        }
    } else {
        uint channel = min(surfaceLayout - 1u, 3u);
        float surfaceScalar = channel == 0u ? surface.r : (channel == 1u ? surface.g : (channel == 2u ? surface.b : surface.a));
        roughness = clamp(roughness * surfaceScalar, 0.02, 1.0);
        metallic = clamp(metallic, 0.0, 1.0);
    }
    vec3 worldNormal = normalize(inWorldNormal);
    if ((materialFlags & T3D_VT_MATERIAL_NORMAL_MAPPING) != 0u) {
        vec3 tangent = normalize(inWorldTangent.xyz);
        tangent = normalize(tangent - worldNormal * dot(worldNormal, tangent));
        vec3 bitangent = normalize(cross(worldNormal, tangent)) * inWorldTangent.w;
        vec2 encoded = t3d_vt_consumer_sample_normal_v3(pageSet.channel1).rg * 2.0 - 1.0;
        encoded *= normalScale;
        float z = sqrt(max(1.0 - dot(encoded, encoded), 0.0));
        worldNormal = normalize(mat3(tangent, bitangent, worldNormal) * vec3(encoded, z));
    }

    const uint surfaceMask = 0xffffffffu;
    bool receiveShadows = (materialFlags & T3D_RW_MATERIAL_FLAG_RECEIVE_SHADOWS) != 0u;
    vec3 albedo = base.rgb;
    vec3 emission = vec3(0.0);
    vec3 viewPosition = (f.viewMatrix * vec4(inWorldPosition, 1.0)).xyz;
    vec3 viewNormal = normalize(mat3(f.viewMatrix) * worldNormal);
    T3DForwardShadingContext shadingContext = t3d_forward_make_shading_context(viewPosition, viewNormal);
    t3d_forward_apply_decals(shadingContext, surfaceMask, albedo, roughness, metallic, ao, emission);
    emission *= t3d_forward_light_frame.environment_params0.y;

    vec3 directDiffuse;
    vec3 directSpecular;
    t3d_forward_direct_lighting(albedo,
        shadingContext,
        roughness,
        metallic,
        1.0 - roughness,
        surfaceMask,
        3u,
        receiveShadows,
        0u,
        1.0,
        directDiffuse,
        directSpecular);
    vec3 ambientDiffuse;
    vec3 environmentSpecular;
    t3d_forward_environment_lighting(albedo, shadingContext, roughness, metallic, ao, 1.0 - roughness, ambientDiffuse, environmentSpecular);
    vec3 reflectionSpecular = t3d_forward_reflection(shadingContext, roughness, metallic, albedo, surfaceMask);
    vec3 specular = directSpecular + environmentSpecular + reflectionSpecular;
    vec3 lit = ambientDiffuse + directDiffuse * ao + emission;
#ifndef T3D_FORWARD_SPECULAR
    lit += specular;
#endif
    outColor = vec4(lit, base.a);
#ifdef T3D_FORWARD_SPECULAR
    outSpecular = vec4(specular, metallic);
#endif
#ifdef T3D_FORWARD_MOTION
    vec2 currentClip = inCurrentClip.xy / max(abs(inCurrentClip.w), 1e-6) - f.motionJitter.xy;
    vec2 previousClip = inPreviousClip.xy / max(abs(inPreviousClip.w), 1e-6) - f.motionJitter.zw;
    outMotion = (previousClip - currentClip) * 0.5;
#endif
}
