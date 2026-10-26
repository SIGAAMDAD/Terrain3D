// BR10-BR13: shader IDs, variant mapping and default startup policy.
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"
#include <assert.h>
#include <string.h>

int main() {
	static_assert( T3D_RD_SHADER_INSTANCE_COMPACT_BASE - T3D_RD_SHADER_TERRAIN_COMPACT_BASE == 72u,
		"BR21 terrain variant count" );
	static_assert( T3D_RD_SHADER_RETIRED_SCREEN_SPACE_COVERAGE - T3D_RD_SHADER_INSTANCE_COMPACT_BASE == 24u,
		"BR21 instance/world variant count" );
	for ( uint32_t id = T3D_RD_SHADER_TERRAIN_COMPACT_BASE; id < T3D_RD_SHADER_RETIRED_SCREEN_SPACE_COVERAGE; ++id ) {
		const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( ( t3dRdShaderId_t ) id );
		assert( entry.m_path && entry.m_version );
		assert( strstr( entry.m_version, "_compact" ) != NULL );
		assert( !t3dRdShaderNeedsStartupPhysical( ( t3dRdShaderId_t ) id ) );
	}
    const t3dRdShaderCatalogEntry_t coverage = t3dRdShaderCatalogGet( T3D_RD_SHADER_RETIRED_SCREEN_SPACE_COVERAGE );
    const t3dRdShaderCatalogEntry_t shade = t3dRdShaderCatalogGet( T3D_RD_SHADER_RETIRED_SCREEN_SPACE_SHADE );
    assert( !coverage.m_path && !coverage.m_version );
    assert( !shade.m_path && !shade.m_version );
    assert( !t3dRdShaderNeedsStartupPhysical( T3D_RD_SHADER_RETIRED_SCREEN_SPACE_SHADE ) );
	const t3dRdShaderCatalogEntry_t compute = t3dRdShaderCatalogGet( T3D_RD_SHADER_CLUSTER_COMPACT );
	assert( compute.m_path && strstr( compute.m_path, "terrain_cluster_compact.glsl" ) );
	assert( !t3dRdShaderNeedsStartupPhysical( T3D_RD_SHADER_CLUSTER_COMPACT ) );
	return 0;
}
