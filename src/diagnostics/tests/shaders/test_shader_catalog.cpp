// RET-07: retired compact terrain IDs are tombstones; UCMP owns terrain materials.
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"
#include <assert.h>
#include <string.h>

int main() {
    for ( uint32_t raw = T3D_RD_SHADER_TERRAIN_COMPACT_BASE;
          raw < T3D_RD_SHADER_INSTANCE_COMPACT_BASE; ++raw ) {
        const t3dRdShaderId_t id = ( t3dRdShaderId_t ) raw;
        const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
        assert( !entry.m_path && !entry.m_version );
        assert( !t3dRdShaderCatalogIsActive( id ) );
        assert( !t3dRdShaderNeedsStartupPhysical( id ) );
    }
    for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT; ++variant ) {
        const t3dRdShaderId_t id = t3dRdUnifiedTerrainId( 0u, variant, false, false, true );
        const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
        assert( entry.m_path && entry.m_version && strstr( entry.m_version, "_compact" ) );
    }
    assert( !t3dRdShaderCatalogIsActive( T3D_RD_SHADER_BRUSH_COMPUTE ) );
    assert( !t3dRdShaderCatalogIsActive( T3D_RD_SHADER_DERIVED_COMPUTE ) );
    const t3dRdShaderCatalogEntry_t compute = t3dRdShaderCatalogGet( T3D_RD_SHADER_CLUSTER_COMPACT );
    assert( compute.m_path && strstr( compute.m_path, "terrain_cluster_compact.glsl" ) );
    return 0;
}
