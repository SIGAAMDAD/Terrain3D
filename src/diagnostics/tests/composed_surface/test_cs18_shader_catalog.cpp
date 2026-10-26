// Headless CS18 catalog/ID validation, no RenderingDevice required.
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"
#include <assert.h>
#include <string.h>

int main() {
    static const char *const expected[] = {
        "cs_terrain_strip_color_native", "cs_terrain_strip_color_specular",
        "cs_terrain_strip_color_motion", "cs_terrain_strip_color_specular_motion",
        "cs_terrain_strip_color_native_radiance_array",
        "cs_terrain_strip_color_specular_radiance_array",
        "cs_terrain_strip_color_motion_radiance_array",
        "cs_terrain_strip_color_specular_motion_radiance_array",
        "cs_terrain_strip_depth_normal_roughness",
        "cs_terrain_strip_depth_normal_roughness_voxel_gi"
    };
    static_assert(T3D_RD_SHADER_WORLD_CS_END - T3D_RD_SHADER_WORLD_CS_BASE == 10, "CS18 catalog size");
    assert(!t3dRdShaderNeedsStartupPhysical(T3D_RD_SHADER_WORLD_CS_BASE));
    assert(!t3dRdShaderNeedsStartupPhysical(T3D_RD_SHADER_COUNT));
    for (unsigned i=0; i < 10; ++i) {
        const auto id = static_cast<t3dRdShaderId_t>(T3D_RD_SHADER_WORLD_CS_BASE + i);
        const auto entry = t3dRdShaderCatalogGet(id);
        assert(entry.m_id == id);
        assert(entry.m_path && !strcmp(entry.m_path, "res://addons/terrain_3d/rd/terrain_world_raster.glsl"));
        assert(entry.m_version && !strcmp(entry.m_version, expected[i]));
    }
    // P25/P26 add IDs *after* the ten optional CS18 variants without moving
    // the existing shader IDs or changing the CS18 enabled-policy range.
    assert(T3D_RD_SHADER_INSTANCE_CULL_GROUPED == T3D_RD_SHADER_WORLD_CS_END);
    assert(T3D_RD_SHADER_WORLD_VISIBILITY_CULL_GROUPED == T3D_RD_SHADER_INSTANCE_CULL_GROUPED + 1u);
    const auto groupedInstance = t3dRdShaderCatalogGet(T3D_RD_SHADER_INSTANCE_CULL_GROUPED);
    const auto groupedWorld = t3dRdShaderCatalogGet(T3D_RD_SHADER_WORLD_VISIBILITY_CULL_GROUPED);
    assert(groupedInstance.m_version && !strcmp(groupedInstance.m_version, "grouped"));
    assert(groupedWorld.m_version && !strcmp(groupedWorld.m_version, "cull_grouped"));
    assert(!t3dRdShaderNeedsStartupPhysical(T3D_RD_SHADER_INSTANCE_CULL_GROUPED));
    assert(!t3dRdShaderNeedsStartupPhysical(T3D_RD_SHADER_WORLD_VISIBILITY_CULL_GROUPED));
    // P30 adds four optional small-page reduction variants after grouped CULL.
    assert(T3D_RD_SHADER_PAGE_COMPUTE_R32F_REDUCE == T3D_RD_SHADER_WORLD_VISIBILITY_CULL_GROUPED + 1u);
    static const char *const reduce_versions[] = {
        "r32f_reduce", "rgba8_reduce", "r16ui_reduce", "r32ui_reduce"
    };
    for (unsigned i = 0; i < 4u; ++i) {
        const auto id = static_cast<t3dRdShaderId_t>(T3D_RD_SHADER_PAGE_COMPUTE_R32F_REDUCE + i);
        const auto entry = t3dRdShaderCatalogGet(id);
        assert(entry.m_id == id);
        assert(entry.m_path && !strcmp(entry.m_path, "res://addons/terrain_3d/compute/terrain_page_storage.glsl"));
        assert(entry.m_version && !strcmp(entry.m_version, reduce_versions[i]));
        assert(!t3dRdShaderNeedsStartupPhysical(id));
    }
    // Previous source-VT feedback family still has its own catalog boundary.
    for (unsigned i=0; i < 3u * T3D_RD_FORWARD_COLOR_VARIANT_COUNT; ++i) {
        const auto id = static_cast<t3dRdShaderId_t>(T3D_RD_SHADER_TERRAIN_FEEDBACK_ONE_SUN_BASE + i);
        const auto entry = t3dRdShaderCatalogGet(id);
        assert(entry.m_id == id);
        assert(entry.m_version && strncmp(entry.m_version, "feedback_", 9) == 0);
    }
    return 0;
}
