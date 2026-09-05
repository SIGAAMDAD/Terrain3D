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
#include "render_world_common_inc.glsl"
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
    outUv = vec2(uintBitsToFloat(vertexWords[base + 5u]), uintBitsToFloat(vertexWords[base + 6u]));
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
#include "render_world_common_inc.glsl"
#include "terrain_forward_lighting_inc.glsl"
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    Frame f;
};
layout(set = 0, binding = 7, std430) readonly buffer MaterialBuffer {
    Material materials[];
};
layout(set = 0, binding = 8, std430) readonly buffer AllocationBuffer {
    Allocation allocations[];
};
layout(set = 0, binding = 9, std430) readonly buffer PageTableBuffer {
    PageEntry pageTable[];
};
layout(set = 0, binding = 10) uniform sampler2DArray vtAlbedoBank0;
layout(set = 0, binding = 11) uniform sampler2DArray vtAlbedoBank1;
layout(set = 0, binding = 12) uniform sampler2DArray vtAlbedoBank2;
layout(set = 0, binding = 13) uniform sampler2DArray vtAlbedoBank3;
layout(set = 0, binding = 14) uniform sampler2DArray vtAlbedoBank4;
layout(set = 0, binding = 15) uniform sampler2DArray vtAlbedoBank5;
layout(set = 0, binding = 16) uniform sampler2DArray vtAlbedoBank6;
layout(set = 0, binding = 17) uniform sampler2DArray vtAlbedoBank7;
layout(set = 0, binding = 18) uniform sampler2DArray vtAlbedoBank8;
layout(set = 0, binding = 19) uniform sampler2DArray vtAlbedoBank9;
layout(set = 0, binding = 20) uniform sampler2DArray vtAlbedoBank10;
layout(set = 0, binding = 21) uniform sampler2DArray vtAlbedoBank11;
layout(set = 0, binding = 22) uniform sampler2DArray vtAlbedoBank12;
layout(set = 0, binding = 23) uniform sampler2DArray vtAlbedoBank13;
layout(set = 0, binding = 24) uniform sampler2DArray vtAlbedoBank14;
layout(set = 0, binding = 25) uniform sampler2DArray vtAlbedoBank15;
layout(set = 0, binding = 26) uniform sampler2DArray vtNormalBank0;
layout(set = 0, binding = 27) uniform sampler2DArray vtNormalBank1;
layout(set = 0, binding = 28) uniform sampler2DArray vtNormalBank2;
layout(set = 0, binding = 29) uniform sampler2DArray vtNormalBank3;
layout(set = 0, binding = 30) uniform sampler2DArray vtNormalBank4;
layout(set = 0, binding = 31) uniform sampler2DArray vtNormalBank5;
layout(set = 0, binding = 32) uniform sampler2DArray vtNormalBank6;
layout(set = 0, binding = 33) uniform sampler2DArray vtNormalBank7;
layout(set = 0, binding = 34) uniform sampler2DArray vtNormalBank8;
layout(set = 0, binding = 35) uniform sampler2DArray vtNormalBank9;
layout(set = 0, binding = 36) uniform sampler2DArray vtNormalBank10;
layout(set = 0, binding = 37) uniform sampler2DArray vtNormalBank11;
layout(set = 0, binding = 38) uniform sampler2DArray vtNormalBank12;
layout(set = 0, binding = 39) uniform sampler2DArray vtNormalBank13;
layout(set = 0, binding = 40) uniform sampler2DArray vtNormalBank14;
layout(set = 0, binding = 41) uniform sampler2DArray vtNormalBank15;
layout(set = 0, binding = 42) uniform sampler2DArray vtSurfaceBank0;
layout(set = 0, binding = 43) uniform sampler2DArray vtSurfaceBank1;
layout(set = 0, binding = 44) uniform sampler2DArray vtSurfaceBank2;
layout(set = 0, binding = 45) uniform sampler2DArray vtSurfaceBank3;
layout(set = 0, binding = 46) uniform sampler2DArray vtSurfaceBank4;
layout(set = 0, binding = 47) uniform sampler2DArray vtSurfaceBank5;
layout(set = 0, binding = 48) uniform sampler2DArray vtSurfaceBank6;
layout(set = 0, binding = 49) uniform sampler2DArray vtSurfaceBank7;
layout(set = 0, binding = 50) uniform sampler2DArray vtSurfaceBank8;
layout(set = 0, binding = 51) uniform sampler2DArray vtSurfaceBank9;
layout(set = 0, binding = 52) uniform sampler2DArray vtSurfaceBank10;
layout(set = 0, binding = 53) uniform sampler2DArray vtSurfaceBank11;
layout(set = 0, binding = 54) uniform sampler2DArray vtSurfaceBank12;
layout(set = 0, binding = 55) uniform sampler2DArray vtSurfaceBank13;
layout(set = 0, binding = 56) uniform sampler2DArray vtSurfaceBank14;
layout(set = 0, binding = 57) uniform sampler2DArray vtSurfaceBank15;

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
const uint T3D_RW_MATERIAL_FLAG_ALPHA_SCISSOR = 1u << 0u;
const uint T3D_RW_MATERIAL_FLAG_RECEIVE_SHADOWS = 1u << 3u;

uint t3d_rw_mip_extent(uint v, uint mip) {
    for (uint m = 0u; m < mip; ++m)
        v = v > 1u ? (v + 1u) >> 1u : 1u;
    return max(v, 1u);
}
uint t3d_rw_mip_base_page(Allocation a, uint mip) {
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
bool t3d_rw_resolve_page(uint wantedPage, out uint physicalPacked, out uint resolvedMip) {
    uint page = wantedPage;
    for (uint i = 0u; i < 18u; ++i) {
        if (page == T3D_RW_INVALID_ID || page >= f.resourceCounts.z)
            break;
        PageEntry e = pageTable[page];
        uint flags = e.packedMipFlags >> 16u;
        if ((flags & 2u) != 0u && e.physicalSlot != T3D_RW_INVALID_ID) {
            physicalPacked = e.physicalSlot;
            resolvedMip = e.packedMipFlags & 0xffffu;
            return true;
        }
        page = e.parentPage;
    }
    physicalPacked = T3D_RW_INVALID_ID;
    resolvedMip = 0u;
    return false;
}
vec4 t3d_rw_sample_albedo_bank(uint bank, uint layer, vec2 uv) {
    switch (bank) {
        case 0u:
            return texture(vtAlbedoBank0, vec3(uv, float(layer)));
        case 1u:
            return texture(vtAlbedoBank1, vec3(uv, float(layer)));
        case 2u:
            return texture(vtAlbedoBank2, vec3(uv, float(layer)));
        case 3u:
            return texture(vtAlbedoBank3, vec3(uv, float(layer)));
        case 4u:
            return texture(vtAlbedoBank4, vec3(uv, float(layer)));
        case 5u:
            return texture(vtAlbedoBank5, vec3(uv, float(layer)));
        case 6u:
            return texture(vtAlbedoBank6, vec3(uv, float(layer)));
        case 7u:
            return texture(vtAlbedoBank7, vec3(uv, float(layer)));
        case 8u:
            return texture(vtAlbedoBank8, vec3(uv, float(layer)));
        case 9u:
            return texture(vtAlbedoBank9, vec3(uv, float(layer)));
        case 10u:
            return texture(vtAlbedoBank10, vec3(uv, float(layer)));
        case 11u:
            return texture(vtAlbedoBank11, vec3(uv, float(layer)));
        case 12u:
            return texture(vtAlbedoBank12, vec3(uv, float(layer)));
        case 13u:
            return texture(vtAlbedoBank13, vec3(uv, float(layer)));
        case 14u:
            return texture(vtAlbedoBank14, vec3(uv, float(layer)));
        case 15u:
            return texture(vtAlbedoBank15, vec3(uv, float(layer)));
        default:
            return vec4(1.0);
    }
}
vec4 t3d_rw_sample_normal_bank(uint bank, uint layer, vec2 uv) {
    switch (bank) {
        case 0u:
            return texture(vtNormalBank0, vec3(uv, float(layer)));
        case 1u:
            return texture(vtNormalBank1, vec3(uv, float(layer)));
        case 2u:
            return texture(vtNormalBank2, vec3(uv, float(layer)));
        case 3u:
            return texture(vtNormalBank3, vec3(uv, float(layer)));
        case 4u:
            return texture(vtNormalBank4, vec3(uv, float(layer)));
        case 5u:
            return texture(vtNormalBank5, vec3(uv, float(layer)));
        case 6u:
            return texture(vtNormalBank6, vec3(uv, float(layer)));
        case 7u:
            return texture(vtNormalBank7, vec3(uv, float(layer)));
        case 8u:
            return texture(vtNormalBank8, vec3(uv, float(layer)));
        case 9u:
            return texture(vtNormalBank9, vec3(uv, float(layer)));
        case 10u:
            return texture(vtNormalBank10, vec3(uv, float(layer)));
        case 11u:
            return texture(vtNormalBank11, vec3(uv, float(layer)));
        case 12u:
            return texture(vtNormalBank12, vec3(uv, float(layer)));
        case 13u:
            return texture(vtNormalBank13, vec3(uv, float(layer)));
        case 14u:
            return texture(vtNormalBank14, vec3(uv, float(layer)));
        case 15u:
            return texture(vtNormalBank15, vec3(uv, float(layer)));
        default:
            return vec4(0.5, 0.5, 1.0, 1.0);
    }
}
vec4 t3d_rw_sample_surface_bank(uint bank, uint layer, vec2 uv) {
    switch (bank) {
        case 0u:
            return texture(vtSurfaceBank0, vec3(uv, float(layer)));
        case 1u:
            return texture(vtSurfaceBank1, vec3(uv, float(layer)));
        case 2u:
            return texture(vtSurfaceBank2, vec3(uv, float(layer)));
        case 3u:
            return texture(vtSurfaceBank3, vec3(uv, float(layer)));
        case 4u:
            return texture(vtSurfaceBank4, vec3(uv, float(layer)));
        case 5u:
            return texture(vtSurfaceBank5, vec3(uv, float(layer)));
        case 6u:
            return texture(vtSurfaceBank6, vec3(uv, float(layer)));
        case 7u:
            return texture(vtSurfaceBank7, vec3(uv, float(layer)));
        case 8u:
            return texture(vtSurfaceBank8, vec3(uv, float(layer)));
        case 9u:
            return texture(vtSurfaceBank9, vec3(uv, float(layer)));
        case 10u:
            return texture(vtSurfaceBank10, vec3(uv, float(layer)));
        case 11u:
            return texture(vtSurfaceBank11, vec3(uv, float(layer)));
        case 12u:
            return texture(vtSurfaceBank12, vec3(uv, float(layer)));
        case 13u:
            return texture(vtSurfaceBank13, vec3(uv, float(layer)));
        case 14u:
            return texture(vtSurfaceBank14, vec3(uv, float(layer)));
        case 15u:
            return texture(vtSurfaceBank15, vec3(uv, float(layer)));
        default:
            return vec4(1.0);
    }
}
bool t3d_rw_resolve_vt(uint allocationId, vec2 uv, bool repeatUv, out uint bank, out uint layer, out vec2 tileUv) {
    if (allocationId == T3D_RW_INVALID_ID || allocationId >= f.resourceCounts.y)
        return false;
    Allocation a = allocations[allocationId];
    uint mipCount = a.packedMipChannel & 0xffffu;
    if (mipCount == 0u || a.pageCount == 0u || a.mip0Width == 0u || a.mip0Height == 0u)
        return false;
    vec2 wrapped = repeatUv ? fract(uv) : clamp(uv, vec2(0.0), vec2(0.999999));
    vec2 virtualTexels = vec2(float(a.mip0Width * 128u), float(a.mip0Height * 128u));
    vec2 dx = dFdx(uv * virtualTexels);
    vec2 dy = dFdy(uv * virtualTexels);
    float rho = max(length(dx), length(dy));
    uint wantedMip = uint(clamp(floor(log2(max(rho, 1.0))), 0.0, float(mipCount - 1u)));
    uint width = t3d_rw_mip_extent(a.mip0Width, wantedMip);
    uint height = t3d_rw_mip_extent(a.mip0Height, wantedMip);
    uint pageX = min(uint(floor(wrapped.x * float(width))), width - 1u);
    uint pageY = min(uint(floor(wrapped.y * float(height))), height - 1u);
    uint wantedPage = t3d_rw_mip_base_page(a, wantedMip) + pageY * width + pageX;
    uint physicalPacked;
    uint resolvedMip;
    if (!t3d_rw_resolve_page(wantedPage, physicalPacked, resolvedMip))
        return false;
    uint resolvedWidth = t3d_rw_mip_extent(a.mip0Width, resolvedMip);
    uint resolvedHeight = t3d_rw_mip_extent(a.mip0Height, resolvedMip);
    vec2 local = fract(wrapped * vec2(float(resolvedWidth), float(resolvedHeight)));
    tileUv = (vec2(4.5) + local * 127.0) / 136.0;
    bank = physicalPacked >> 16u;
    layer = physicalPacked & 0xffffu;
    return true;
}
vec4 t3d_rw_sample_albedo(uint allocationId, vec2 uv, bool repeatUv) {
    uint bank, layer;
    vec2 tileUv;
    return t3d_rw_resolve_vt(allocationId, uv, repeatUv, bank, layer, tileUv) ? t3d_rw_sample_albedo_bank(bank, layer, tileUv) : vec4(1.0);
}
vec4 t3d_rw_sample_normal(uint allocationId, vec2 uv, bool repeatUv) {
    uint bank, layer;
    vec2 tileUv;
    return t3d_rw_resolve_vt(allocationId, uv, repeatUv, bank, layer, tileUv) ? t3d_rw_sample_normal_bank(bank, layer, tileUv)
                                                                              : vec4(0.5, 0.5, 1.0, 1.0);
}
vec4 t3d_rw_sample_surface(uint allocationId, vec2 uv, bool repeatUv) {
    uint bank, layer;
    vec2 tileUv;
    return t3d_rw_resolve_vt(allocationId, uv, repeatUv, bank, layer, tileUv) ? t3d_rw_sample_surface_bank(bank, layer, tileUv) : vec4(1.0);
}
void main() {
    if (inMaterialId >= f.resourceCounts.x)
        discard;
    Material material = materials[inMaterialId];
    vec2 materialUv = inUv * material.uvScaleBias.xy + material.uvScaleBias.zw;
    bool repeatUv = material.meta.y == 0u;
    vec4 albedoSample = t3d_rw_sample_albedo(material.vt.x, materialUv, repeatUv);
    vec4 base = material.baseColor * albedoSample * inVertexColor;
    uint materialFlags = material.meta.x;
    vec2 scalar0 = unpackHalf2x16(material.meta.z);
    vec2 scalar1 = unpackHalf2x16(material.meta.w);
    float roughness = clamp(scalar0.x, 0.0, 1.0);
    float metallic = clamp(scalar0.y, 0.0, 1.0);
    float normalScale = max(scalar1.x, 0.0);
    float alphaThreshold = clamp(scalar1.y, 0.0, 1.0);
    if ((materialFlags & T3D_RW_MATERIAL_FLAG_ALPHA_SCISSOR) != 0u && base.a < alphaThreshold)
        discard;

    float ao = 1.0;
    if (material.vt.z != T3D_RW_INVALID_ID) {
        vec4 surface = t3d_rw_sample_surface(material.vt.z, materialUv, repeatUv);
        ao = clamp(surface.r, 0.0, 1.0);
        roughness = clamp(roughness * surface.g, 0.0, 1.0);
        metallic = clamp(metallic * surface.b, 0.0, 1.0);
    }
    vec3 worldNormal = normalize(inWorldNormal);
    if (material.vt.y != T3D_RW_INVALID_ID) {
        vec3 tangent = normalize(inWorldTangent.xyz);
        tangent = normalize(tangent - worldNormal * dot(worldNormal, tangent));
        vec3 bitangent = normalize(cross(worldNormal, tangent)) * inWorldTangent.w;
        vec2 encoded = t3d_rw_sample_normal(material.vt.y, materialUv, repeatUv).rg * 2.0 - 1.0;
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
    t3d_forward_apply_decals(viewPosition, surfaceMask, albedo, viewNormal, roughness, metallic, ao, emission);
    emission *= t3d_forward_light_frame.environment_params0.y;
    worldNormal = normalize(transpose(mat3(f.viewMatrix)) * viewNormal);

    vec3 directDiffuse;
    vec3 directSpecular;
    t3d_forward_direct_lighting(albedo,
        inWorldPosition,
        worldNormal,
        f.viewMatrix,
        f.cameraAndMaxDistance.xyz,
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
    viewPosition = (f.viewMatrix * vec4(inWorldPosition, 1.0)).xyz;
    viewNormal = normalize(mat3(f.viewMatrix) * worldNormal);
    vec3 viewDirection = normalize(-viewPosition);
    vec3 ambientDiffuse;
    vec3 environmentSpecular;
    t3d_forward_environment_lighting(
        albedo, viewNormal, viewDirection, roughness, metallic, ao, 1.0 - roughness, ambientDiffuse, environmentSpecular);
    vec3 reflectionSpecular = t3d_forward_reflection(viewPosition, viewNormal, viewDirection, roughness, metallic, albedo, surfaceMask);
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
